// qcedid: works around a Snapdragon X display driver (qcdxkm8380.sys) limit that
// drops DisplayID timings from external monitors.
//
// The KMD keeps at most 32 modes per display and parses DisplayID extension blocks
// after the base and CTA-861 blocks. Monitors that declare many legacy modes and put
// their high refresh timings in DisplayID (common for 4K120+ and 1440p165+) lose those
// timings. qcedid builds a trimmed copy of the monitor's EDID and loads it through the
// KMD's own EDID override escape, which reads C:\Windows\edidoverride.xml.
//
// See NOTES.md for the reverse engineered interfaces.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_BLOCKS 4      // edidoverride.xml parser accepts at most 4 blocks
#define MODE_LIMIT 32     // KMD mode list capacity per display
#define LINK_KBPS 25920000 // 4-lane HBR3 payload bandwidth

static const char *const tested_driver_versions[] = { "31.0.152.1" };

static void usage(void);

// ---------------------------------------------------------------------------
// D3DKMT (gdi32) declarations
// ---------------------------------------------------------------------------

typedef UINT D3DKMT_HANDLE;
typedef LONG NTSTATUS;

typedef struct {
    LUID AdapterLuid;
    D3DKMT_HANDLE hAdapter;
} D3DKMT_OPENADAPTERFROMLUID;

typedef struct {
    union { D3DKMT_HANDLE hAdapter; VOID *pAdapter; };
    UINT Flags;
    D3DKMT_HANDLE hDevice;
    VOID *pCommandBuffer;
    UINT CommandBufferSize;
    VOID *pAllocationList;
    UINT AllocationListSize;
    VOID *pPatchLocationList;
    UINT PatchLocationListSize;
} D3DKMT_CREATEDEVICE;

typedef struct {
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    UINT Type; // D3DKMT_ESCAPE_DRIVERPRIVATE = 0
    UINT Flags;
    VOID *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    D3DKMT_HANDLE hContext;
} D3DKMT_ESCAPE;

typedef struct { D3DKMT_HANDLE hDevice; } D3DKMT_DESTROYDEVICE;
typedef struct { D3DKMT_HANDLE hAdapter; } D3DKMT_CLOSEADAPTER;

NTSTATUS APIENTRY D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID *);
NTSTATUS APIENTRY D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *);
NTSTATUS APIENTRY D3DKMTEscape(const D3DKMT_ESCAPE *);
NTSTATUS APIENTRY D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *);
NTSTATUS APIENTRY D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *);

// Escape buffer layout (qcdxkm8380.sys 31.0.152.1):
//   +0x0c escape code 0x20000100, +0x14 result, +0x88 command size, +0x8c command.
// Command: {0x106, 2 (EDID override), display id 1..10, enable}, status at +0x1c.
#define ESCAPE_SIZE 0x4854
#define CMD_OFFSET 0x8c

// Sends the EDID override command for one KMD display id. enable = 1 loads
// edidoverride.xml, 0 clears the override. Returns the command status, or -1 when
// the escape itself failed.
static int send_override(LUID luid, UINT display, UINT enable)
{
    D3DKMT_OPENADAPTERFROMLUID open = { luid, 0 };
    NTSTATUS st = D3DKMTOpenAdapterFromLuid(&open);
    if (st) {
        fprintf(stderr, "D3DKMTOpenAdapterFromLuid failed: 0x%08lx\n", st);
        return -1;
    }
    D3DKMT_CREATEDEVICE cd = { 0 };
    cd.hAdapter = open.hAdapter;
    st = D3DKMTCreateDevice(&cd);
    int status = -1;
    if (st) {
        fprintf(stderr, "D3DKMTCreateDevice failed: 0x%08lx\n", st);
    } else {
        BYTE *buf = calloc(1, ESCAPE_SIZE);
        UINT *cmd = (UINT *)(buf + CMD_OFFSET);
        *(UINT *)(buf + 0x0c) = 0x20000100;
        *(UINT *)(buf + 0x88) = ESCAPE_SIZE - CMD_OFFSET;
        cmd[0] = 0x106;
        cmd[1] = 2;
        cmd[2] = display;
        cmd[3] = enable;
        cmd[7] = 0xffffffff;
        D3DKMT_ESCAPE esc = { 0 };
        esc.hAdapter = open.hAdapter;
        esc.hDevice = cd.hDevice;
        esc.pPrivateDriverData = buf;
        esc.PrivateDriverDataSize = ESCAPE_SIZE;
        st = D3DKMTEscape(&esc);
        if (st)
            fprintf(stderr, "D3DKMTEscape failed: 0x%08lx\n", st);
        else
            status = (int)cmd[7];
        free(buf);
        D3DKMT_DESTROYDEVICE dd = { cd.hDevice };
        D3DKMTDestroyDevice(&dd);
    }
    D3DKMT_CLOSEADAPTER ca = { open.hAdapter };
    D3DKMTCloseAdapter(&ca);
    return status;
}

// ---------------------------------------------------------------------------
// EDID analysis
// ---------------------------------------------------------------------------

typedef struct {
    BYTE b[MAX_BLOCKS * 128];
    int blocks;
} Edid;

// Mode candidates the KMD derives from each part of an EDID, in parse order.
typedef struct {
    int established;
    int standard;   // standard timings, including 0xFA descriptors and CVT codes
    int dtd;        // detailed timing descriptors in the base and CTA blocks
    int svd;        // CTA short video descriptors
    int y420;       // CTA YCbCr 4:2:0 video data block entries
    int displayid;  // DisplayID type I / VII timings
    int displayid_max_khz;
} ModeCounts;

static int before_displayid(const ModeCounts *c)
{
    return c->established + c->standard + c->dtd + c->svd + c->y420;
}

static int popcount8(BYTE v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

static void set_checksum(BYTE *block)
{
    BYTE sum = 0;
    for (int i = 0; i < 127; i++)
        sum += block[i];
    block[127] = (BYTE)(0 - sum);
}

// Calls fn for every DisplayID type I / VII timing descriptor.
static void for_each_displayid_timing(const Edid *e, void (*fn)(void *, const BYTE *, int), void *ctx)
{
    for (int k = 1; k < e->blocks; k++) {
        const BYTE *b = e->b + k * 128;
        if (b[0] != 0x70)
            continue;
        BYTE version = b[1];
        int end = 5 + b[2];
        if (end > 126)
            end = 126;
        for (int i = 5; i + 3 <= end;) {
            BYTE tag = b[i];
            int len = b[i + 2];
            if (tag == 0 && len == 0)
                break;
            if (tag == 0x03 || tag == 0x22)
                for (int d = 0; d + 20 <= len && i + 3 + d + 20 <= end; d += 20)
                    fn(ctx, b + i + 3 + d, version);
            i += 3 + len;
        }
    }
}

static int displayid_khz(const BYTE *d, int version)
{
    int raw = d[0] | d[1] << 8 | d[2] << 16;
    return version >= 0x20 ? raw + 1 : (raw + 1) * 10;
}

static void count_displayid(void *ctx, const BYTE *d, int version)
{
    ModeCounts *c = ctx;
    int khz = displayid_khz(d, version);
    c->displayid++;
    if (khz > c->displayid_max_khz)
        c->displayid_max_khz = khz;
}

static void print_displayid(void *ctx, const BYTE *d, int version)
{
    (void)ctx;
    int khz = displayid_khz(d, version);
    int h = (d[4] | d[5] << 8) + 1, hb = (d[6] | d[7] << 8) + 1;
    int v = (d[12] | d[13] << 8) + 1, vb = (d[14] | d[15] << 8) + 1;
    printf("      %dx%d @ %.2f Hz (%.2f MHz)\n", h, v,
           khz * 1000.0 / ((double)(h + hb) * (v + vb)), khz / 1000.0);
}

static void count_modes(const Edid *e, ModeCounts *c)
{
    const BYTE *b = e->b;
    memset(c, 0, sizeof *c);
    c->established = popcount8(b[35]) + popcount8(b[36]) + (b[37] >> 7);
    for (int i = 38; i < 54; i += 2)
        if (!(b[i] == 1 && b[i + 1] == 1) && !(b[i] == 0 && b[i + 1] == 0))
            c->standard++;
    for (int o = 54; o <= 108; o += 18) {
        if (b[o] | b[o + 1]) {
            c->dtd++;
        } else if (b[o + 3] == 0xFA) {
            for (int i = o + 5; i < o + 17; i += 2)
                if (!(b[i] == 1 && b[i + 1] == 1))
                    c->standard++;
        } else if (b[o + 3] == 0xF7) {
            for (int i = o + 6; i < o + 12; i++)
                c->established += popcount8(b[i]);
        } else if (b[o + 3] == 0xF8) {
            for (int i = o + 6; i < o + 18; i += 3)
                if (b[i] | b[i + 1] | b[i + 2])
                    c->standard += popcount8(b[i + 2] & 0x1f);
        }
    }
    for (int k = 1; k < e->blocks; k++) {
        const BYTE *x = b + k * 128;
        if (x[0] != 0x02)
            continue;
        int d = x[2];
        for (int i = 4; i < d && i < 127;) {
            int tag = x[i] >> 5, len = x[i] & 31;
            if (tag == 2)
                c->svd += len;
            else if (tag == 7 && len >= 1 && x[i + 1] == 0x0E)
                c->y420 += len - 1;
            i += len + 1;
        }
        for (int j = d; d >= 4 && j + 18 <= 127 && (x[j] | x[j + 1]); j += 18)
            c->dtd++;
    }
    for_each_displayid_timing(e, count_displayid, c);
}

// Declared bits per colour channel of an EDID 1.4 digital input, or 0 if undefined.
static int declared_bpc(const Edid *e)
{
    BYTE in = e->b[20];
    if (!(in & 0x80) || e->b[18] != 1 || e->b[19] < 4)
        return 0;
    int depth = (in >> 4) & 7;
    return depth >= 1 && depth <= 6 ? 4 + 2 * depth : 0;
}

static void set_bpc(Edid *e, int bpc)
{
    e->b[20] = (BYTE)((e->b[20] & 0x8f) | (((bpc - 4) / 2) << 4));
}

// Vertical resolution of a CTA-861 VIC, with interlaced formats reported per field.
static int vic_height(int vic)
{
    static const int interlaced[] = { 5, 6, 7, 10, 11, 20, 21, 22, 25, 26, 39, 40, 44, 45, 46, 50, 51, 54, 55, 58, 59 };
    for (size_t i = 0; i < sizeof interlaced / sizeof *interlaced; i++)
        if (interlaced[i] == vic)
            return vic == 5 || vic == 20 || vic == 39 || vic == 40 || vic == 46 ? 540 : 240;
    if (vic <= 3 || vic == 14 || vic == 15 || vic == 35 || vic == 36 || (vic >= 48 && vic <= 49) || (vic >= 56 && vic <= 57))
        return 480;
    if (vic == 8 || vic == 9 || vic == 12 || vic == 13)
        return 240;
    if (vic == 23 || vic == 24 || vic == 27 || vic == 28)
        return 288;
    if ((vic >= 17 && vic <= 18) || (vic >= 29 && vic <= 30) || (vic >= 37 && vic <= 38) || (vic >= 42 && vic <= 43) || (vic >= 52 && vic <= 53))
        return 576;
    if (vic == 4 || vic == 19 || vic == 41 || vic == 47 || (vic >= 60 && vic <= 62) || (vic >= 65 && vic <= 71) ||
        (vic >= 79 && vic <= 85) || (vic >= 108 && vic <= 110))
        return 720;
    if (vic == 16 || (vic >= 31 && vic <= 34) || (vic >= 63 && vic <= 64) || (vic >= 72 && vic <= 78) ||
        (vic >= 86 && vic <= 92) || (vic >= 111 && vic <= 113))
        return 1080;
    return 2160;
}

static int svd_vic(BYTE svd) { return svd >= 129 && svd <= 192 ? svd & 0x7f : svd; }
static int svd_native(BYTE svd) { return svd >= 129 && svd <= 192; }

// Rewrites a CTA-861 extension block, dropping SVDs whose keep flag is 0 (in data
// block order) and, when drop_y420vdb is set, the YCbCr 4:2:0 video data blocks.
// The YCbCr 4:2:0 capability map is remapped to the remaining SVDs.
static void rebuild_cta(BYTE *x, const BYTE *keep, int drop_y420vdb)
{
    int d = x[2];
    if (d < 4)
        return;
    BYTE out[128] = { 0 };
    memcpy(out, x, 4);
    int o = 4;

    int kept_old[128], nkept = 0, sidx = 0;
    for (int i = 4; i < d;) {
        int tag = x[i] >> 5, len = x[i] & 31;
        if (tag == 2)
            for (int j = 0; j < len; j++, sidx++)
                if (keep[sidx])
                    kept_old[nkept++] = sidx;
        i += len + 1;
    }

    sidx = 0;
    for (int i = 4; i < d;) {
        int tag = x[i] >> 5, len = x[i] & 31;
        const BYTE *p = x + i + 1;
        if (tag == 2) {
            BYTE svds[31];
            int n = 0;
            for (int j = 0; j < len; j++, sidx++)
                if (keep[sidx])
                    svds[n++] = p[j];
            if (n) {
                out[o++] = (BYTE)(2 << 5 | n);
                memcpy(out + o, svds, n);
                o += n;
            }
        } else if (tag == 7 && len >= 1 && p[0] == 0x0E && drop_y420vdb) {
            // dropped
        } else if (tag == 7 && len > 1 && p[0] == 0x0F) {
            int nb = (nkept + 7) / 8;
            if (nb == 0)
                nb = 1;
            BYTE bits[16] = { 0 };
            for (int n = 0; n < nkept; n++) {
                int old = kept_old[n];
                if (old / 8 < len - 1 && (p[1 + old / 8] >> (old % 8)) & 1)
                    bits[n / 8] |= (BYTE)(1 << (n % 8));
            }
            out[o++] = (BYTE)(7 << 5 | (1 + nb));
            out[o++] = 0x0F;
            memcpy(out + o, bits, nb);
            o += nb;
        } else {
            memcpy(out + o, x + i, len + 1);
            o += len + 1;
        }
        i += len + 1;
    }
    out[2] = (BYTE)o;
    for (int j = d; j + 18 <= 127 && (x[j] | x[j + 1]); j += 18, o += 18)
        memcpy(out + o, x + j, 18);
    memcpy(x, out, 127);
    set_checksum(x);
}

// Removes mode sources the KMD parses before DisplayID until the DisplayID timings
// fit the mode list, in order of least value: established and standard timings,
// YCbCr 4:2:0-only modes, then non-native CTA modes (interlaced and lowest first).
static void trim(Edid *e, int verbose)
{
    ModeCounts c;
    count_modes(e, &c);
    int budget = MODE_LIMIT - c.displayid;
    if (before_displayid(&c) <= budget)
        return;

    BYTE *b = e->b;
    if (verbose)
        printf("  - removing %d established and %d standard timings\n", c.established, c.standard);
    memset(b + 35, 0, 3);
    memset(b + 38, 1, 16);
    for (int o = 54; o <= 108; o += 18)
        if (!(b[o] | b[o + 1]) && (b[o + 3] == 0xFA || b[o + 3] == 0xF7 || b[o + 3] == 0xF8)) {
            memset(b + o, 0, 18);
            b[o + 3] = 0x10;
        }
    set_checksum(b);

    count_modes(e, &c);
    if (before_displayid(&c) > budget && c.y420) {
        if (verbose)
            printf("  - removing %d YCbCr 4:2:0-only modes\n", c.y420);
        BYTE keep[128];
        memset(keep, 1, sizeof keep);
        for (int k = 1; k < e->blocks; k++)
            if (b[k * 128] == 0x02)
                rebuild_cta(b + k * 128, keep, 1);
        count_modes(e, &c);
    }

    for (int k = 1; k < e->blocks && before_displayid(&c) > budget; k++) {
        BYTE *x = b + k * 128;
        if (x[0] != 0x02)
            continue;
        BYTE svds[128], keep[128];
        int n = 0;
        for (int i = 4; i < x[2];) {
            int tag = x[i] >> 5, len = x[i] & 31;
            if (tag == 2)
                for (int j = 0; j < len; j++)
                    svds[n++] = x[i + 1 + j];
            i += len + 1;
        }
        memset(keep, 1, sizeof keep);
        int excess = before_displayid(&c) - budget;
        for (; excess > 0; excess--) {
            int best = -1;
            for (int s = 0; s < n; s++)
                if (keep[s] && !svd_native(svds[s]) &&
                    (best < 0 || vic_height(svd_vic(svds[s])) < vic_height(svd_vic(svds[best]))))
                    best = s;
            if (best < 0)
                break;
            keep[best] = 0;
            if (verbose)
                printf("  - removing CTA VIC %d (%dp)\n", svd_vic(svds[best]), vic_height(svd_vic(svds[best])));
        }
        rebuild_cta(x, keep, 0);
        count_modes(e, &c);
    }
}

// ---------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------

typedef struct {
    LUID adapter;
    UINT target;
    WCHAR name[64];
    WCHAR instance[256]; // DISPLAY\<id>\<instance>
    Edid edid;
    int have_edid;
} Monitor;

static int read_edid(const WCHAR *instance, Edid *e)
{
    WCHAR key[400];
    swprintf(key, 400, L"SYSTEM\\CurrentControlSet\\Enum\\%ls\\Device Parameters", instance);
    DWORD size = sizeof e->b;
    BYTE tmp[8 * 128];
    size = sizeof tmp;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, NULL, tmp, &size) != ERROR_SUCCESS)
        return 0;
    if (size < 128 || size % 128 || size > sizeof e->b)
        return 0;
    memcpy(e->b, tmp, size);
    e->blocks = (int)(size / 128);
    return 1;
}

static int is_adreno_adapter(void)
{
    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
                      KEY_READ, &cls) != ERROR_SUCCESS)
        return 0;
    int found = 0;
    WCHAR sub[16];
    for (DWORD i = 0; !found && RegEnumKeyW(cls, i, sub, 16) == ERROR_SUCCESS; i++) {
        WCHAR desc[128];
        DWORD size = sizeof desc;
        if (RegGetValueW(cls, sub, L"DriverDesc", RRF_RT_REG_SZ, NULL, desc, &size) == ERROR_SUCCESS &&
            wcsstr(desc, L"Adreno"))
            found = 1;
    }
    RegCloseKey(cls);
    return found;
}

static int driver_version(char *out, size_t len)
{
    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
                      KEY_READ, &cls) != ERROR_SUCCESS)
        return 0;
    int found = 0;
    WCHAR sub[16];
    for (DWORD i = 0; !found && RegEnumKeyW(cls, i, sub, 16) == ERROR_SUCCESS; i++) {
        WCHAR desc[128], ver[64];
        DWORD size = sizeof desc, vsize = sizeof ver;
        if (RegGetValueW(cls, sub, L"DriverDesc", RRF_RT_REG_SZ, NULL, desc, &size) == ERROR_SUCCESS &&
            wcsstr(desc, L"Adreno") &&
            RegGetValueW(cls, sub, L"DriverVersion", RRF_RT_REG_SZ, NULL, ver, &vsize) == ERROR_SUCCESS) {
            snprintf(out, len, "%ls", ver);
            found = 1;
        }
    }
    RegCloseKey(cls);
    return found;
}

// Connected external display targets with their EDIDs.
static int list_monitors(Monitor *mons, int max)
{
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS)
        return 0;
    DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
    DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
    int n = 0;
    if (QueryDisplayConfig(QDC_ALL_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < np && n < max; i++) {
            DISPLAYCONFIG_PATH_TARGET_INFO *t = &paths[i].targetInfo;
            if (!t->targetAvailable || t->outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                t->outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                t->outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED)
                continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME name = { 0 };
            name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            name.header.size = sizeof name;
            name.header.adapterId = t->adapterId;
            name.header.id = t->id;
            if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
                continue;
            int dup = 0;
            for (int j = 0; j < n; j++)
                if (mons[j].target == t->id && !memcmp(&mons[j].adapter, &t->adapterId, sizeof(LUID)))
                    dup = 1;
            if (dup)
                continue;
            Monitor *m = &mons[n++];
            memset(m, 0, sizeof *m);
            m->adapter = t->adapterId;
            m->target = t->id;
            wcsncpy(m->name, name.monitorFriendlyDeviceName, 63);
            // \\?\DISPLAY#PHLC249#3&a0c4df9&0&UID3#{guid} -> DISPLAY\PHLC249\3&a0c4df9&0&UID3
            const WCHAR *p = name.monitorDevicePath;
            if (!wcsncmp(p, L"\\\\?\\", 4))
                p += 4;
            int k = 0;
            for (; *p && k < 255 && wcsncmp(p, L"#{", 2); p++)
                m->instance[k++] = *p == L'#' ? L'\\' : *p;
            m->instance[k] = 0;
            m->have_edid = read_edid(m->instance, &m->edid);
        }
    }
    free(paths);
    free(modes);
    return n;
}

// KMD display id of a Windows display target. Observed: the internal panel is target 0
// and display id 1; a USB-C DP alt mode monitor was target 3 and display id 4.
static UINT display_id_for(const Monitor *m) { return m->target + 1; }

// ---------------------------------------------------------------------------
// Original EDID cache
// ---------------------------------------------------------------------------

// %ProgramData%\qcedid\<instance>.edid holds the monitor's unmodified EDID. Once an
// override is active, Windows reports the overridden EDID instead.
static void cache_path(const Monitor *m, WCHAR *out, size_t len)
{
    WCHAR base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy(base, L"C:\\ProgramData");
    WCHAR inst[256];
    wcscpy(inst, m->instance);
    for (WCHAR *p = inst; *p; p++)
        if (*p == L'\\' || *p == L'&')
            *p = L'_';
    swprintf(out, len, L"%ls\\qcedid\\%ls.edid", base, inst);
}

static int load_cache(const Monitor *m, Edid *e)
{
    WCHAR path[MAX_PATH];
    cache_path(m, path, MAX_PATH);
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return 0;
    size_t n = fread(e->b, 1, sizeof e->b, f);
    fclose(f);
    if (n < 128 || n % 128)
        return 0;
    e->blocks = (int)(n / 128);
    return 1;
}

static int save_cache(const Monitor *m, const Edid *e)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH];
    cache_path(m, path, MAX_PATH);
    wcscpy(dir, path);
    *wcsrchr(dir, L'\\') = 0;
    CreateDirectoryW(dir, NULL);
    FILE *f = _wfopen(path, L"wb");
    if (!f)
        return 0;
    fwrite(e->b, 1, e->blocks * 128, f);
    fclose(f);
    return 1;
}

// The monitor's unmodified EDID: the cached copy if the current EDID is one qcedid
// produced (same identity, no established or standard timings), else the current one.
static int original_edid(const Monitor *m, Edid *out, int *from_cache)
{
    Edid cached;
    *from_cache = 0;
    if (load_cache(m, &cached) && memcmp(cached.b, m->edid.b, cached.blocks * 128) &&
        !memcmp(cached.b + 8, m->edid.b + 8, 10)) {
        ModeCounts c;
        count_modes(&m->edid, &c);
        if (c.established == 0 && c.standard == 0) {
            *out = cached;
            *from_cache = 1;
            return 1;
        }
    }
    *out = m->edid;
    return 1;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static void describe(const Monitor *m)
{
    printf("[target %u] %ls  (%ls, driver display id %u)\n", m->target, m->name[0] ? m->name : L"(unnamed)",
           m->instance, display_id_for(m));
    if (!m->have_edid) {
        printf("    EDID not available\n");
        return;
    }
    Edid orig;
    int from_cache;
    original_edid(m, &orig, &from_cache);
    ModeCounts c;
    count_modes(&orig, &c);
    printf("    EDID: %d blocks, ~%d modes before DisplayID (%d established, %d standard, %d detailed, %d CTA)\n",
           orig.blocks, before_displayid(&c), c.established, c.standard, c.dtd, c.svd + c.y420);
    if (!c.displayid) {
        printf("    No DisplayID timings: not affected by the mode list limit.\n");
        return;
    }
    printf("    DisplayID timings:\n");
    for_each_displayid_timing(&orig, print_displayid, NULL);
    if (from_cache)
        printf("    Override active (original EDID from cache).\n");
    else if (before_displayid(&c) + c.displayid > MODE_LIMIT)
        printf("    Affected: the driver keeps %d modes, so DisplayID timings are dropped.\n"
               "    Fix: qcedid apply %u   (elevated), then replug the monitor\n", MODE_LIMIT, m->target);
    else
        printf("    DisplayID timings fit the mode list: not affected by the mode list limit.\n");
}

static int write_override_xml(const Edid *e)
{
    WCHAR path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\edidoverride.xml");
    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        fprintf(stderr, "Cannot write %ls: run qcedid from an elevated prompt.\n", path);
        return 0;
    }
    for (int k = 0; k < e->blocks; k++) {
        fprintf(f, "<BLOCK%d>", k);
        for (int i = 0; i < 128; i++)
            fprintf(f, "%02X", e->b[k * 128 + i]);
        fprintf(f, "</BLOCK%d>\n", k);
    }
    fclose(f);
    return 1;
}

static int check_driver(int force)
{
    char ver[64] = "";
    if (!is_adreno_adapter() || !driver_version(ver, sizeof ver)) {
        fprintf(stderr, "No Qualcomm Adreno display adapter found.\n");
        return 0;
    }
    for (size_t i = 0; i < sizeof tested_driver_versions / sizeof *tested_driver_versions; i++)
        if (!strcmp(ver, tested_driver_versions[i]))
            return 1;
    if (force) {
        printf("Driver %s is untested; continuing because of --force.\n", ver);
        return 1;
    }
    fprintf(stderr,
            "Driver %s is untested. The escape layout was reverse engineered from %s and may differ.\n"
            "Use --force to try anyway.\n",
            ver, tested_driver_versions[0]);
    return 0;
}

static Monitor *pick(Monitor *mons, int n, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-')
            continue;
        UINT target = (UINT)atoi(argv[i]);
        for (int j = 0; j < n; j++)
            if (mons[j].target == target)
                return &mons[j];
        fprintf(stderr, "No external monitor with target %u (see qcedid list).\n", target);
        return NULL;
    }
    if (n == 1)
        return &mons[0];
    fprintf(stderr, n ? "Several monitors: pass a target from qcedid list.\n" : "No external monitor found.\n");
    return NULL;
}

static const char *opt_value(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc - 1; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

static int has_flag(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], name))
            return 1;
    return 0;
}

// Turns an original EDID into the override: trims mode sources ahead of DisplayID and
// lowers the declared bpc (to bpc_opt, or to 8 when the fastest DisplayID timing would
// not fit 4-lane HBR3 at the declared depth). Prints each change.
static int build_override(Edid *e, const char *bpc_opt)
{
    ModeCounts c;
    count_modes(e, &c);
    trim(e, 1);

    int bpc = declared_bpc(e);
    if (bpc_opt) {
        int want = atoi(bpc_opt);
        if (want < 6 || want > 16 || want % 2) {
            fprintf(stderr, "--bpc must be 6, 8, 10, 12, 14 or 16.\n");
            return 0;
        }
        set_bpc(e, want);
        printf("  - declaring %d bpc (was %d)\n", want, bpc);
    } else if (bpc > 8 && (long long)c.displayid_max_khz * 3 * bpc > LINK_KBPS) {
        set_bpc(e, 8);
        printf("  - declaring 8 bpc (was %d): %.2f MHz at %d bpc exceeds 4-lane HBR3\n", bpc,
               c.displayid_max_khz / 1000.0, bpc);
    }
    set_checksum(e->b);

    ModeCounts after;
    count_modes(e, &after);
    printf("  ~%d modes before DisplayID + %d DisplayID timings (limit %d)\n", before_displayid(&after),
           after.displayid, MODE_LIMIT);
    if (before_displayid(&after) + after.displayid > MODE_LIMIT)
        printf("  Warning: still over the limit; some DisplayID timings may be dropped.\n");
    return 1;
}

static int read_edid_file(const char *path, Edid *e)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(e->b, 1, sizeof e->b, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (n < 128 || n % 128 || more)
        return 0;
    e->blocks = (int)(n / 128);
    return 1;
}

// Offline variant of apply: analyses an EDID file and optionally writes the override.
static int cmd_analyze(int argc, char **argv)
{
    if (argc < 1) {
        usage();
        return 2;
    }
    Edid e;
    if (!read_edid_file(argv[0], &e)) {
        fprintf(stderr, "%s is not a binary EDID of 1..%d blocks.\n", argv[0], MAX_BLOCKS);
        return 1;
    }
    ModeCounts c;
    count_modes(&e, &c);
    printf("%s: %d blocks, ~%d modes before DisplayID (%d established, %d standard, %d detailed, %d CTA), "
           "declared %d bpc\n",
           argv[0], e.blocks, before_displayid(&c), c.established, c.standard, c.dtd, c.svd + c.y420,
           declared_bpc(&e));
    if (!c.displayid) {
        printf("No DisplayID timings: not affected by the mode list limit.\n");
        return 0;
    }
    printf("DisplayID timings:\n");
    for_each_displayid_timing(&e, print_displayid, NULL);
    printf("Override:\n");
    if (!build_override(&e, opt_value(argc, argv, "--bpc")))
        return 1;
    const char *out = opt_value(argc, argv, "--out");
    if (out) {
        FILE *f = fopen(out, "wb");
        if (!f || fwrite(e.b, 1, e.blocks * 128, f) != (size_t)e.blocks * 128) {
            fprintf(stderr, "Cannot write %s.\n", out);
            return 1;
        }
        fclose(f);
        printf("Wrote %s.\n", out);
    }
    return 0;
}

static int cmd_apply(int argc, char **argv)
{
    int force = has_flag(argc, argv, "--force");
    if (!check_driver(force))
        return 1;
    Monitor mons[8];
    int n = list_monitors(mons, 8);
    // Skip option values when picking the target.
    char *args[16];
    int na = 0;
    for (int i = 0; i < argc && na < 16; i++) {
        if (!strcmp(argv[i], "--bpc") || !strcmp(argv[i], "--display-id")) {
            i++;
            continue;
        }
        args[na++] = argv[i];
    }
    Monitor *m = pick(mons, n, na, args);
    if (!m)
        return 1;
    if (!m->have_edid) {
        fprintf(stderr, "Cannot read the EDID of target %u.\n", m->target);
        return 1;
    }

    Edid e;
    int from_cache;
    original_edid(m, &e, &from_cache);
    if (e.blocks > MAX_BLOCKS) {
        fprintf(stderr, "EDID has %d blocks; the driver's override file supports %d.\n", e.blocks, MAX_BLOCKS);
        return 1;
    }
    ModeCounts c;
    count_modes(&e, &c);
    if (!c.displayid && !force) {
        fprintf(stderr, "This EDID has no DisplayID timings, so the mode list limit is not what hides its modes.\n");
        return 1;
    }
    if (!from_cache && !save_cache(m, &e)) {
        fprintf(stderr, "Cannot save the original EDID: run qcedid from an elevated prompt.\n");
        return 1;
    }

    printf("Building override for %ls (target %u):\n", m->name, m->target);
    if (!build_override(&e, opt_value(argc, argv, "--bpc")) || !write_override_xml(&e))
        return 1;

    UINT id = (UINT)(opt_value(argc, argv, "--display-id") ? atoi(opt_value(argc, argv, "--display-id"))
                                                            : (int)display_id_for(m));
    int st = send_override(m->adapter, id, 0);
    if (st != 0) {
        fprintf(stderr, "Driver rejected display id %u (status %d); not applying.\n", id, st);
        return 1;
    }
    st = send_override(m->adapter, id, 1);
    if (st != 0) {
        fprintf(stderr, "Loading the override for display id %u failed (status %d).\n", id, st);
        return 1;
    }
    printf("Override loaded for driver display id %u. Unplug and replug the monitor.\n"
           "It lasts until reboot or driver reload.\n", id);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    if (!check_driver(has_flag(argc, argv, "--force")))
        return 1;
    Monitor mons[8];
    int n = list_monitors(mons, 8);
    if (has_flag(argc, argv, "--all")) {
        LUID luid;
        if (n) {
            luid = mons[0].adapter;
        } else {
            fprintf(stderr, "No external monitor connected to find the adapter; connect one and retry.\n");
            return 1;
        }
        for (UINT id = 1; id <= 10; id++)
            send_override(luid, id, 0);
        printf("Cleared overrides on all display ids. Replug monitors.\n");
        return 0;
    }
    Monitor *m = pick(mons, n, argc, argv);
    if (!m)
        return 1;
    int st = send_override(m->adapter, display_id_for(m), 0);
    if (st != 0) {
        fprintf(stderr, "Clearing failed (status %d).\n", st);
        return 1;
    }
    printf("Cleared the override for %ls (driver display id %u). Replug the monitor.\n", m->name,
           display_id_for(m));
    return 0;
}

static int cmd_list(void)
{
    char ver[64] = "unknown";
    driver_version(ver, sizeof ver);
    printf("Adreno driver %s\n\n", ver);
    Monitor mons[8];
    int n = list_monitors(mons, 8);
    if (!n)
        printf("No external monitors connected.\n");
    for (int i = 0; i < n; i++) {
        describe(&mons[i]);
        printf("\n");
    }
    return 0;
}

static void usage(void)
{
    printf("qcedid - restore DisplayID modes (e.g. 4K120) on Snapdragon X external displays\n\n"
           "  qcedid list                       show external monitors and whether they are affected\n"
           "  qcedid apply [target] [options]   load a trimmed EDID override (elevated)\n"
           "      --bpc N          declare N bits per colour (default: 8 only when needed)\n"
           "      --display-id N   driver display id (default: target + 1)\n"
           "      --force          allow untested drivers / EDIDs without DisplayID\n"
           "  qcedid clear [target | --all]     remove the override\n"
           "  qcedid analyze <edid.bin> [--bpc N] [--out file]\n"
           "                                    show what apply would change, without applying\n\n"
           "Overrides last until reboot. Replug the monitor after apply or clear.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "list"))
        return cmd_list();
    if (!strcmp(argv[1], "apply"))
        return cmd_apply(argc - 2, argv + 2);
    if (!strcmp(argv[1], "clear"))
        return cmd_clear(argc - 2, argv + 2);
    if (!strcmp(argv[1], "analyze"))
        return cmd_analyze(argc - 2, argv + 2);
    usage();
    return 2;
}
