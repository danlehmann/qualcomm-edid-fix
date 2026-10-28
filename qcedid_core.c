// EDID mode table, override building and Qualcomm KMD access. See NOTES.md for the
// reverse engineered driver behaviour this models.
#include "qcedid_core.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Driver versions whose escape layout, override file format and mode list limit were
// checked against the disassembly.
static const char *const tested_versions[] = { "31.0.152.1", "31.0.170.0" };
const char *const tested_driver_version = "31.0.152.1 and 31.0.170.0";

static char g_error[512];

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

const char *core_error(void) { return g_error; }

// ---------------------------------------------------------------------------
// Timing tables
// ---------------------------------------------------------------------------

typedef struct {
    short w, h;
    unsigned char hz, interlaced;
} Format;

// CTA-861 VICs 1..127 (aspect ratio variants share a resolution).
static const Format vic_table[128] = {
    [1] = { 640, 480, 60, 0 },     [2] = { 720, 480, 60, 0 },     [3] = { 720, 480, 60, 0 },
    [4] = { 1280, 720, 60, 0 },    [5] = { 1920, 1080, 60, 1 },   [6] = { 1440, 480, 60, 1 },
    [7] = { 1440, 480, 60, 1 },    [8] = { 1440, 240, 60, 0 },    [9] = { 1440, 240, 60, 0 },
    [10] = { 2880, 480, 60, 1 },   [11] = { 2880, 480, 60, 1 },   [12] = { 2880, 240, 60, 0 },
    [13] = { 2880, 240, 60, 0 },   [14] = { 1440, 480, 60, 0 },   [15] = { 1440, 480, 60, 0 },
    [16] = { 1920, 1080, 60, 0 },  [17] = { 720, 576, 50, 0 },    [18] = { 720, 576, 50, 0 },
    [19] = { 1280, 720, 50, 0 },   [20] = { 1920, 1080, 50, 1 },  [21] = { 1440, 576, 50, 1 },
    [22] = { 1440, 576, 50, 1 },   [23] = { 1440, 288, 50, 0 },   [24] = { 1440, 288, 50, 0 },
    [25] = { 2880, 576, 50, 1 },   [26] = { 2880, 576, 50, 1 },   [27] = { 2880, 288, 50, 0 },
    [28] = { 2880, 288, 50, 0 },   [29] = { 1440, 576, 50, 0 },   [30] = { 1440, 576, 50, 0 },
    [31] = { 1920, 1080, 50, 0 },  [32] = { 1920, 1080, 24, 0 },  [33] = { 1920, 1080, 25, 0 },
    [34] = { 1920, 1080, 30, 0 },  [35] = { 2880, 480, 60, 0 },   [36] = { 2880, 480, 60, 0 },
    [37] = { 2880, 576, 50, 0 },   [38] = { 2880, 576, 50, 0 },   [39] = { 1920, 1080, 50, 1 },
    [40] = { 1920, 1080, 100, 1 }, [41] = { 1280, 720, 100, 0 },  [42] = { 720, 576, 100, 0 },
    [43] = { 720, 576, 100, 0 },   [44] = { 1440, 576, 100, 1 },  [45] = { 1440, 576, 100, 1 },
    [46] = { 1920, 1080, 120, 1 }, [47] = { 1280, 720, 120, 0 },  [48] = { 720, 480, 120, 0 },
    [49] = { 720, 480, 120, 0 },   [50] = { 1440, 480, 120, 1 },  [51] = { 1440, 480, 120, 1 },
    [52] = { 720, 576, 200, 0 },   [53] = { 720, 576, 200, 0 },   [54] = { 1440, 576, 200, 1 },
    [55] = { 1440, 576, 200, 1 },  [56] = { 720, 480, 240, 0 },   [57] = { 720, 480, 240, 0 },
    [58] = { 1440, 480, 240, 1 },  [59] = { 1440, 480, 240, 1 },  [60] = { 1280, 720, 24, 0 },
    [61] = { 1280, 720, 25, 0 },   [62] = { 1280, 720, 30, 0 },   [63] = { 1920, 1080, 120, 0 },
    [64] = { 1920, 1080, 100, 0 }, [65] = { 1280, 720, 24, 0 },   [66] = { 1280, 720, 25, 0 },
    [67] = { 1280, 720, 30, 0 },   [68] = { 1280, 720, 50, 0 },   [69] = { 1280, 720, 60, 0 },
    [70] = { 1280, 720, 100, 0 },  [71] = { 1280, 720, 120, 0 },  [72] = { 1920, 1080, 24, 0 },
    [73] = { 1920, 1080, 25, 0 },  [74] = { 1920, 1080, 30, 0 },  [75] = { 1920, 1080, 50, 0 },
    [76] = { 1920, 1080, 60, 0 },  [77] = { 1920, 1080, 100, 0 }, [78] = { 1920, 1080, 120, 0 },
    [79] = { 1680, 720, 24, 0 },   [80] = { 1680, 720, 25, 0 },   [81] = { 1680, 720, 30, 0 },
    [82] = { 1680, 720, 50, 0 },   [83] = { 1680, 720, 60, 0 },   [84] = { 1680, 720, 100, 0 },
    [85] = { 1680, 720, 120, 0 },  [86] = { 2560, 1080, 24, 0 },  [87] = { 2560, 1080, 25, 0 },
    [88] = { 2560, 1080, 30, 0 },  [89] = { 2560, 1080, 50, 0 },  [90] = { 2560, 1080, 60, 0 },
    [91] = { 2560, 1080, 100, 0 }, [92] = { 2560, 1080, 120, 0 }, [93] = { 3840, 2160, 24, 0 },
    [94] = { 3840, 2160, 25, 0 },  [95] = { 3840, 2160, 30, 0 },  [96] = { 3840, 2160, 50, 0 },
    [97] = { 3840, 2160, 60, 0 },  [98] = { 4096, 2160, 24, 0 },  [99] = { 4096, 2160, 25, 0 },
    [100] = { 4096, 2160, 30, 0 }, [101] = { 4096, 2160, 50, 0 }, [102] = { 4096, 2160, 60, 0 },
    [103] = { 3840, 2160, 24, 0 }, [104] = { 3840, 2160, 25, 0 }, [105] = { 3840, 2160, 30, 0 },
    [106] = { 3840, 2160, 50, 0 }, [107] = { 3840, 2160, 60, 0 }, [108] = { 1280, 720, 48, 0 },
    [109] = { 1280, 720, 48, 0 },  [110] = { 1680, 720, 48, 0 },  [111] = { 1920, 1080, 48, 0 },
    [112] = { 1920, 1080, 48, 0 }, [113] = { 2560, 1080, 48, 0 }, [114] = { 3840, 2160, 48, 0 },
    [115] = { 4096, 2160, 48, 0 }, [116] = { 3840, 2160, 48, 0 }, [117] = { 3840, 2160, 100, 0 },
    [118] = { 3840, 2160, 120, 0 }, [119] = { 3840, 2160, 100, 0 }, [120] = { 3840, 2160, 120, 0 },
    [121] = { 5120, 2160, 24, 0 }, [122] = { 5120, 2160, 25, 0 }, [123] = { 5120, 2160, 30, 0 },
    [124] = { 5120, 2160, 48, 0 }, [125] = { 5120, 2160, 50, 0 }, [126] = { 5120, 2160, 60, 0 },
    [127] = { 5120, 2160, 100, 0 },
};

static Format vic_format(int vic)
{
    static const unsigned char rates[8] = { 24, 25, 30, 48, 50, 60, 100, 120 };
    Format f = { 0 };
    if (vic >= 1 && vic <= 127)
        f = vic_table[vic];
    else if (vic == 193)
        f = (Format){ 5120, 2160, 120, 0 };
    else if (vic >= 194 && vic <= 209)
        f = (Format){ 7680, 4320, rates[(vic - 194) % 8], 0 };
    else if (vic >= 210 && vic <= 217)
        f = (Format){ 10240, 4320, rates[vic - 210], 0 };
    else if (vic == 218 || vic == 219)
        f = (Format){ 4096, 2160, vic == 218 ? 100 : 120, 0 };
    return f;
}

// EDID established timings I/II, byte 35 bit 7 first.
static const Format established_table[17] = {
    { 720, 400, 70, 0 },  { 720, 400, 88, 0 },  { 640, 480, 60, 0 },   { 640, 480, 67, 0 },
    { 640, 480, 72, 0 },  { 640, 480, 75, 0 },  { 800, 600, 56, 0 },   { 800, 600, 60, 0 },
    { 800, 600, 72, 0 },  { 800, 600, 75, 0 },  { 832, 624, 75, 0 },   { 1024, 768, 87, 1 },
    { 1024, 768, 60, 0 }, { 1024, 768, 70, 0 }, { 1024, 768, 75, 0 },  { 1280, 1024, 75, 0 },
    { 1152, 870, 75, 0 },
};

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

// ---------------------------------------------------------------------------
// Mode table
// ---------------------------------------------------------------------------

static Row *add_row(ModeTable *t, RowSource source, int offset)
{
    if (t->n >= MAX_ROWS)
        return NULL;
    Row *r = &t->rows[t->n++];
    memset(r, 0, sizeof *r);
    r->source = source;
    r->offset = offset;
    r->weight = 1;
    r->keep = 1;
    return r;
}

static void set_format(Row *r, Format f)
{
    r->w = f.w;
    r->h = f.h;
    r->hz = f.hz;
    r->interlaced = f.interlaced;
    // Typical blanking overhead; only used to judge whether the link can carry it.
    r->khz = (int)((double)f.w * f.h * f.hz * (f.interlaced ? 0.5 : 1.0) * 1.2 / 1000);
    r->khz_estimated = 1;
}

static void add_standard(ModeTable *t, const Edid *e, int o)
{
    const BYTE *b = e->b;
    if ((b[o] == 1 && b[o + 1] == 1) || (b[o] == 0 && b[o + 1] == 0))
        return;
    Row *r = add_row(t, SRC_STANDARD, o);
    if (!r)
        return;
    int w = (b[o] + 31) * 8, h;
    switch (b[o + 1] >> 6) {
    case 0: h = b[19] >= 3 ? w * 10 / 16 : w; break;
    case 1: h = w * 3 / 4; break;
    case 2: h = w * 4 / 5; break;
    default: h = w * 9 / 16; break;
    }
    set_format(r, (Format){ (short)w, (short)h, (unsigned char)((b[o + 1] & 63) + 60), 0 });
}

static void add_detailed(ModeTable *t, RowSource source, const BYTE *d, int offset, int index)
{
    Row *r = add_row(t, source, offset);
    if (!r)
        return;
    int hb = d[3] | (d[4] & 0x0f) << 8, vb = d[6] | (d[7] & 0x0f) << 8;
    r->index = index;
    r->khz = (d[0] | d[1] << 8) * 10;
    r->w = d[2] | (d[4] >> 4) << 8;
    r->h = d[5] | (d[7] >> 4) << 8;
    r->interlaced = d[17] >> 7;
    int frame = (r->w + hb) * (r->h + vb);
    r->hz = frame ? r->khz * 1000.0 / frame : 0;
    if (r->interlaced)
        r->h *= 2;
}

static void add_cta(ModeTable *t, const Edid *e, int k)
{
    const BYTE *x = e->b + k * 128;
    int base = k * 128, d = x[2], svd = 0, y420 = 0;
    for (int i = 4; i < d && i < 127;) {
        int tag = x[i] >> 5, len = x[i] & 31;
        if (tag == 2) {
            for (int j = 0; j < len; j++) {
                BYTE v = x[i + 1 + j];
                Row *r = add_row(t, SRC_CTA_VIC, base + i + 1 + j);
                if (!r)
                    return;
                r->native = v >= 129 && v <= 192;
                r->vic = r->native ? v & 0x7f : v;
                r->index = svd++;
                set_format(r, vic_format(r->vic));
            }
        } else if (tag == 7 && len >= 1 && x[i + 1] == 0x0E) {
            for (int j = 1; j < len; j++) {
                Row *r = add_row(t, SRC_CTA_Y420, base + i + 1 + j);
                if (!r)
                    return;
                r->vic = x[i + 1 + j];
                r->index = y420++;
                set_format(r, vic_format(r->vic));
            }
        }
        i += len + 1;
    }
    int idx = 0;
    for (int j = d; d >= 4 && j + 18 <= 127 && (x[j] | x[j + 1]); j += 18)
        add_detailed(t, SRC_CTA_DETAILED, x + j, base + j, idx++);
}

static void add_displayid(ModeTable *t, const Edid *e, int k)
{
    const BYTE *b = e->b + k * 128;
    BYTE version = b[1];
    int end = 5 + b[2];
    if (end > 126)
        end = 126;
    for (int i = 5; i + 3 <= end;) {
        BYTE tag = b[i];
        int len = b[i + 2];
        if (tag == 0 && len == 0)
            break;
        if (tag == 0x03 || tag == 0x22) {
            for (int o = 0; o + 20 <= len && i + 3 + o + 20 <= end; o += 20) {
                const BYTE *d = b + i + 3 + o;
                Row *r = add_row(t, SRC_DISPLAYID, k * 128 + i + 3 + o);
                if (!r)
                    return;
                int raw = d[0] | d[1] << 8 | d[2] << 16;
                r->khz = version >= 0x20 ? raw + 1 : (raw + 1) * 10;
                r->w = (d[4] | d[5] << 8) + 1;
                r->h = (d[12] | d[13] << 8) + 1;
                int hb = (d[6] | d[7] << 8) + 1, vb = (d[14] | d[15] << 8) + 1;
                r->hz = r->khz * 1000.0 / ((double)(r->w + hb) * (r->h + vb));
                r->interlaced = (d[3] >> 4) & 1;
                r->locked = 1;
            }
        }
        i += 3 + len;
    }
}

void table_from_edid(const Edid *e, ModeTable *t)
{
    const BYTE *b = e->b;
    memset(t, 0, sizeof *t);

    BYTE in = b[20];
    int depth = (in >> 4) & 7;
    if ((in & 0x80) && b[18] == 1 && b[19] >= 4 && depth >= 1 && depth <= 6)
        t->declared_bpc = 4 + 2 * depth;

    // Driver parse order: CTA-861 blocks, the four base block descriptors, standard
    // timings, established timings, and DisplayID last.
    for (int k = 1; k < e->blocks; k++)
        if (b[k * 128] == 0x02)
            add_cta(t, e, k);
    for (int o = 54; o <= 108; o += 18) {
        const BYTE *d = b + o;
        if (d[0] | d[1]) {
            add_detailed(t, SRC_DETAILED, d, o, 0);
            if (o == 54)
                t->rows[t->n - 1].locked = 1;
        } else if (d[3] == 0xFA) {
            for (int s = o + 5; s < o + 17; s += 2)
                add_standard(t, e, s);
        } else if (d[3] == 0xF7 || d[3] == 0xF8) {
            int count = 0;
            if (d[3] == 0xF7)
                for (int i = 6; i < 12; i++)
                    count += popcount8(d[i]);
            else
                for (int i = 6; i < 18; i += 3)
                    if (d[i] | d[i + 1] | d[i + 2])
                        count += popcount8(d[i + 2] & 0x1f);
            if (count) {
                Row *r = add_row(t, SRC_DESCRIPTOR, o);
                if (r)
                    r->weight = count;
            }
        }
    }
    for (int o = 38; o < 54; o += 2)
        add_standard(t, e, o);
    for (int i = 0; i < 17; i++) {
        int byte = 35 + i / 8, bit = 7 - i % 8;
        if ((b[byte] >> bit) & 1) {
            Row *r = add_row(t, SRC_ESTABLISHED, byte);
            if (r) {
                r->bit = bit;
                set_format(r, established_table[i]);
            }
        }
    }
    for (int k = 1; k < e->blocks; k++)
        if (b[k * 128] == 0x70)
            add_displayid(t, e, k);
}

int table_effective_bpc(const ModeTable *t, int bpc_choice)
{
    if (bpc_choice)
        return bpc_choice;
    int bpc = t->declared_bpc ? t->declared_bpc : 8;
    int max_khz = 0;
    for (int i = 0; i < t->n; i++)
        if (t->rows[i].source == SRC_DISPLAYID && t->rows[i].khz > max_khz)
            max_khz = t->rows[i].khz;
    if (bpc > 8 && (long long)max_khz * 3 * bpc > LINK_KBPS)
        return 8;
    return bpc;
}

// Established, standard and CTA rows map onto entries of the driver's timing table, so
// rows with the same format resolve to the same timing and the driver skips the repeat.
static int table_timing(const Row *r)
{
    return r->w && (r->source == SRC_ESTABLISHED || r->source == SRC_STANDARD || r->source == SRC_CTA_VIC ||
                    r->source == SRC_CTA_Y420);
}

static int same_format(const Row *a, const Row *b)
{
    return a->w == b->w && a->h == b->h && a->interlaced == b->interlaced && (int)(a->hz + 0.5) == (int)(b->hz + 0.5);
}

void table_compute(ModeTable *t, int bpc)
{
    int slot = 0;
    for (int i = 0; i < t->n; i++) {
        Row *r = &t->rows[i];
        r->slot = 0;
        int duplicate = 0;
        if (table_timing(r))
            for (int j = 0; j < i && !duplicate; j++)
                duplicate = t->rows[j].slot && table_timing(&t->rows[j]) && same_format(r, &t->rows[j]);
        if (!r->keep) {
            r->status = ST_REMOVED;
        } else if ((long long)r->khz * 3 * bpc > LINK_KBPS) {
            r->status = ST_REJECTED;
        } else if (duplicate) {
            r->status = ST_DUPLICATE;
        } else {
            slot += r->weight;
            r->slot = slot;
            r->status = slot > MODE_LIMIT ? ST_DROPPED : ST_LISTED;
        }
    }
}

int table_displayid_dropped(const ModeTable *t)
{
    int n = 0;
    for (int i = 0; i < t->n; i++)
        if (t->rows[i].source == SRC_DISPLAYID && t->rows[i].status == ST_DROPPED)
            n++;
    return n;
}

int table_count(const ModeTable *t, RowSource source)
{
    int n = 0;
    for (int i = 0; i < t->n; i++)
        if (t->rows[i].source == source && t->rows[i].keep)
            n += t->rows[i].weight;
    return n;
}

void table_reset(ModeTable *t)
{
    for (int i = 0; i < t->n; i++)
        t->rows[i].keep = 1;
}

static int same_row(const Row *a, const Row *b)
{
    return a->source == b->source && a->vic == b->vic && a->w == b->w && a->h == b->h &&
           a->interlaced == b->interlaced && a->weight == b->weight && fabs(a->hz - b->hz) < 0.5;
}

void table_match(ModeTable *t, const ModeTable *active)
{
    BYTE used[MAX_ROWS] = { 0 };
    for (int i = 0; i < t->n; i++) {
        Row *r = &t->rows[i];
        int found = 0;
        for (int j = 0; j < active->n && !found; j++)
            if (!used[j] && same_row(r, &active->rows[j]))
                used[j] = found = 1;
        r->keep = found || r->locked;
    }
}

static void logf_(void (*log)(void *, const char *), void *ctx, const char *fmt, ...)
{
    if (!log)
        return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log(ctx, line);
}

// Removal order within the CTA and detailed stages: interlaced first, then lowest
// resolution, then lowest refresh rate.
static double drop_score(const Row *r)
{
    if (!r->w)
        return -1;
    return (r->interlaced ? 0 : 1e9) + (double)r->h * r->w + r->hz / 1000;
}

static int displayid_over(const ModeTable *t, int limit)
{
    for (int i = 0; i < t->n; i++)
        if (t->rows[i].source == SRC_DISPLAYID && t->rows[i].slot > limit)
            return 1;
    return 0;
}

int table_auto_trim(ModeTable *t, int bpc, int margin, void (*log)(void *, const char *), void *ctx)
{
    table_compute(t, bpc);

    // Stages in order of least value: legacy VGA-era timings, 4:2:0-only modes, CTA short
    // video descriptors, then detailed timings. Within a stage, rows that take a slot are
    // removed one at a time, lowest first.
    static const unsigned stages[] = {
        1u << SRC_ESTABLISHED | 1u << SRC_STANDARD | 1u << SRC_DESCRIPTOR,
        1u << SRC_CTA_Y420,
        1u << SRC_CTA_VIC,
        1u << SRC_CTA_DETAILED | 1u << SRC_DETAILED,
    };
    int limit = MODE_LIMIT - margin;
    for (size_t s = 0; s < sizeof stages / sizeof *stages && displayid_over(t, limit); s++) {
        while (displayid_over(t, limit)) {
            Row *best = NULL;
            for (int i = 0; i < t->n; i++) {
                Row *r = &t->rows[i];
                if (r->slot && !r->locked && !r->native && (stages[s] >> r->source & 1) &&
                    (!best || drop_score(r) < drop_score(best)))
                    best = r;
            }
            if (!best)
                break;
            best->keep = 0;
            char name[64];
            logf_(log, ctx, "removed %s %dx%d%s @ %.0f Hz", row_source_name(best, name, sizeof name), best->w,
                  best->h, best->interlaced ? "i" : "", best->hz);
            table_compute(t, bpc);
        }
    }
    return !table_displayid_dropped(t);
}

const char *row_source_name(const Row *r, char *buf, size_t len)
{
    switch (r->source) {
    case SRC_ESTABLISHED: snprintf(buf, len, "Established"); break;
    case SRC_STANDARD: snprintf(buf, len, "Standard"); break;
    case SRC_DESCRIPTOR: snprintf(buf, len, "Descriptor (%d modes)", r->weight); break;
    case SRC_DETAILED: snprintf(buf, len, r->locked ? "Detailed (preferred)" : "Detailed"); break;
    case SRC_CTA_VIC: snprintf(buf, len, "CTA VIC %d%s", r->vic, r->native ? " (native)" : ""); break;
    case SRC_CTA_Y420: snprintf(buf, len, "CTA 4:2:0 VIC %d", r->vic); break;
    case SRC_CTA_DETAILED: snprintf(buf, len, "CTA detailed"); break;
    case SRC_DISPLAYID: snprintf(buf, len, "DisplayID"); break;
    }
    return buf;
}

const char *row_status_name(const Row *r)
{
    switch (r->status) {
    case ST_LISTED: return "In mode list";
    case ST_DROPPED: return "Dropped (over 32)";
    case ST_REJECTED: return "Exceeds link bandwidth";
    case ST_DUPLICATE: return "Duplicate (no slot)";
    case ST_REMOVED: return "Removed";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Building the override EDID
// ---------------------------------------------------------------------------

// Rewrites a CTA-861 extension block keeping the SVDs, 4:2:0 VDB entries and detailed
// timings whose keep flags are set (indexed in block order). Empty data blocks are
// dropped and the YCbCr 4:2:0 capability map is remapped to the remaining SVDs.
static void rebuild_cta(BYTE *x, const BYTE *svd_keep, const BYTE *y420_keep, const BYTE *dtd_keep)
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
                if (svd_keep[sidx])
                    kept_old[nkept++] = sidx;
        i += len + 1;
    }

    sidx = 0;
    int yidx = 0;
    for (int i = 4; i < d;) {
        int tag = x[i] >> 5, len = x[i] & 31;
        const BYTE *p = x + i + 1;
        if (tag == 2) {
            BYTE v[31];
            int n = 0;
            for (int j = 0; j < len; j++, sidx++)
                if (svd_keep[sidx])
                    v[n++] = p[j];
            if (n) {
                out[o++] = (BYTE)(2 << 5 | n);
                memcpy(out + o, v, n);
                o += n;
            }
        } else if (tag == 7 && len >= 1 && p[0] == 0x0E) {
            BYTE v[31];
            int n = 0;
            for (int j = 1; j < len; j++, yidx++)
                if (y420_keep[yidx])
                    v[n++] = p[j];
            if (n) {
                out[o++] = (BYTE)(7 << 5 | (n + 1));
                out[o++] = 0x0E;
                memcpy(out + o, v, n);
                o += n;
            }
        } else if (tag == 7 && len > 1 && p[0] == 0x0F) {
            int nb = nkept ? (nkept + 7) / 8 : 1;
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
    int idx = 0;
    for (int j = d; j + 18 <= 127 && (x[j] | x[j + 1]); j += 18, idx++)
        if (dtd_keep[idx]) {
            memcpy(out + o, x + j, 18);
            o += 18;
        }
    memcpy(x, out, 127);
    set_checksum(x);
}

void edid_build(const Edid *orig, const ModeTable *t, int bpc, Edid *out)
{
    *out = *orig;
    BYTE *b = out->b;
    for (int i = 0; i < t->n; i++) {
        const Row *r = &t->rows[i];
        if (r->keep)
            continue;
        switch (r->source) {
        case SRC_ESTABLISHED:
            b[r->offset] &= (BYTE) ~(1 << r->bit);
            break;
        case SRC_STANDARD:
            b[r->offset] = b[r->offset + 1] = 1;
            break;
        case SRC_DESCRIPTOR:
        case SRC_DETAILED:
            memset(b + r->offset, 0, 18);
            b[r->offset + 3] = 0x10; // dummy descriptor
            break;
        default:
            break;
        }
    }
    for (int k = 1; k < out->blocks; k++) {
        if (b[k * 128] != 0x02)
            continue;
        BYTE svd_keep[128], y420_keep[128], dtd_keep[8];
        memset(svd_keep, 1, sizeof svd_keep);
        memset(y420_keep, 1, sizeof y420_keep);
        memset(dtd_keep, 1, sizeof dtd_keep);
        int changed = 0;
        for (int i = 0; i < t->n; i++) {
            const Row *r = &t->rows[i];
            if (r->keep || r->offset / 128 != k)
                continue;
            if (r->source == SRC_CTA_VIC)
                svd_keep[r->index] = 0, changed = 1;
            else if (r->source == SRC_CTA_Y420)
                y420_keep[r->index] = 0, changed = 1;
            else if (r->source == SRC_CTA_DETAILED && r->index < 8)
                dtd_keep[r->index] = 0, changed = 1;
        }
        if (changed)
            rebuild_cta(b + k * 128, svd_keep, y420_keep, dtd_keep);
    }
    if (bpc && bpc != t->declared_bpc && (b[20] & 0x80) && b[18] == 1 && b[19] >= 4)
        b[20] = (BYTE)((b[20] & 0x8f) | (((bpc - 4) / 2) << 4));
    set_checksum(b);
}

int edid_read_file(const WCHAR *path, Edid *e)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) {
        set_error("Cannot open %ls.", path);
        return 0;
    }
    size_t n = fread(e->b, 1, sizeof e->b, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (n < 128 || n % 128 || more) {
        set_error("%ls is not a binary EDID of 1 to %d blocks.", path, MAX_BLOCKS);
        return 0;
    }
    e->blocks = (int)(n / 128);
    return 1;
}

int edid_write_file(const WCHAR *path, const Edid *e)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f || fwrite(e->b, 1, e->blocks * 128, f) != (size_t)e->blocks * 128) {
        if (f)
            fclose(f);
        set_error("Cannot write %ls.", path);
        return 0;
    }
    fclose(f);
    return 1;
}

// ---------------------------------------------------------------------------
// Monitors and the original EDID cache
// ---------------------------------------------------------------------------

static int read_registry_edid(const WCHAR *instance, Edid *e)
{
    WCHAR key[400];
    swprintf(key, 400, L"SYSTEM\\CurrentControlSet\\Enum\\%ls\\Device Parameters", instance);
    BYTE tmp[8 * 128];
    DWORD size = sizeof tmp;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"EDID", RRF_RT_REG_BINARY, NULL, tmp, &size) != ERROR_SUCCESS)
        return 0;
    if (size < 128 || size % 128 || size > sizeof e->b)
        return 0;
    memcpy(e->b, tmp, size);
    e->blocks = (int)(size / 128);
    return 1;
}

int list_monitors(Monitor *mons, int max)
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
            int dup = 0;
            for (int j = 0; j < n; j++)
                if (mons[j].target == t->id && !memcmp(&mons[j].adapter, &t->adapterId, sizeof(LUID)))
                    dup = 1;
            if (dup)
                continue;
            DISPLAYCONFIG_TARGET_DEVICE_NAME name = { 0 };
            name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            name.header.size = sizeof name;
            name.header.adapterId = t->adapterId;
            name.header.id = t->id;
            if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
                continue;
            Monitor *m = &mons[n++];
            memset(m, 0, sizeof *m);
            m->adapter = t->adapterId;
            m->target = t->id;
            wcsncpy(m->name, name.monitorFriendlyDeviceName[0] ? name.monitorFriendlyDeviceName : L"(unnamed)", 63);
            // \\?\DISPLAY#PHLC249#3&a0c4df9&0&UID3#{guid} -> DISPLAY\PHLC249\3&a0c4df9&0&UID3
            const WCHAR *p = name.monitorDevicePath;
            if (!wcsncmp(p, L"\\\\?\\", 4))
                p += 4;
            int k = 0;
            for (; *p && k < 255 && wcsncmp(p, L"#{", 2); p++)
                m->instance[k++] = *p == L'#' ? L'\\' : *p;
            m->instance[k] = 0;
            m->have_edid = read_registry_edid(m->instance, &m->edid);
        }
    }
    free(paths);
    free(modes);
    return n;
}


// %ProgramData%\qcedid\<manufacturer, product and serial>.edid holds the unmodified EDID of
// a monitor. The override keeps EDID bytes 8..17, so the name is the same while it is
// active, and it does not depend on the port the monitor is connected to.
static void cache_path(const Edid *e, WCHAR *out, size_t len)
{
    WCHAR base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy(base, L"C:\\ProgramData");
    const BYTE *b = e->b;
    WCHAR vendor[4] = { (WCHAR)('@' + ((b[8] >> 2) & 31)), (WCHAR)('@' + (((b[8] & 3) << 3) | (b[9] >> 5))),
                        (WCHAR)('@' + (b[9] & 31)), 0 };
    swprintf(out, len, L"%ls\\qcedid\\%ls%02X%02X_%02X%02X%02X%02X.edid", base, vendor, b[11], b[10], b[15], b[14],
             b[13], b[12]);
}

void original_edid(const Monitor *m, Edid *out, int *from_cache)
{
    WCHAR path[MAX_PATH];
    Edid cached;
    *from_cache = 0;
    *out = m->edid;
    cache_path(&m->edid, path, MAX_PATH);
    if (!edid_read_file(path, &cached))
        return;
    if (cached.blocks == m->edid.blocks && !memcmp(cached.b, m->edid.b, cached.blocks * 128))
        return;
    if (memcmp(cached.b + 8, m->edid.b + 8, 10))
        return;
    // An active override is the original with modes removed or a lower declared depth.
    ModeTable *cur = malloc(sizeof *cur), *orig = malloc(sizeof *orig);
    table_from_edid(&m->edid, cur);
    table_from_edid(&cached, orig);
    int overridden = table_count(cur, SRC_DISPLAYID) == table_count(orig, SRC_DISPLAYID) &&
                     (cur->n < orig->n || cur->declared_bpc != orig->declared_bpc);
    free(cur);
    free(orig);
    if (overridden) {
        *out = cached;
        *from_cache = 1;
    }
}

int save_original(const Monitor *m, const Edid *e)
{
    (void)m;
    WCHAR path[MAX_PATH], dir[MAX_PATH];
    cache_path(e, path, MAX_PATH);
    wcscpy(dir, path);
    *wcsrchr(dir, L'\\') = 0;
    CreateDirectoryW(dir, NULL);
    if (!edid_write_file(path, e)) {
        set_error("Cannot save the original EDID to %ls; administrator rights are required.", path);
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

int driver_version(char *out, size_t len)
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

int driver_tested(const char *version)
{
    for (size_t i = 0; i < sizeof tested_versions / sizeof *tested_versions; i++)
        if (!strcmp(version, tested_versions[i]))
            return 1;
    return 0;
}

int write_override_xml(const Edid *e)
{
    WCHAR path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\edidoverride.xml");
    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        set_error("Cannot write %ls; administrator rights are required.", path);
        return 0;
    }
    // One line per 128-byte block: "<BLOCKn>" followed by 256 hex digits.
    for (int k = 0; k < e->blocks; k++) {
        fprintf(f, "<BLOCK%d>", k);
        for (int i = 0; i < 128; i++)
            fprintf(f, "%02X", e->b[k * 128 + i]);
        fprintf(f, "</BLOCK%d>\n", k);
    }
    fclose(f);
    return 1;
}

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

// Escape buffer (qcdxkm8380.sys 31.0.152.1): exactly 0x4854 bytes; escape code
// 0x20000100 at +0x0c, result at +0x14, command size at +0x88, command at +0x8c.
// Command: {0x106, 2 (EDID override), display id 1..10, enable}, status at +0x1c.
#define ESCAPE_SIZE 0x4854
#define CMD_OFFSET 0x8c

int send_override(LUID adapter, UINT display, UINT enable)
{
    D3DKMT_OPENADAPTERFROMLUID open = { adapter, 0 };
    NTSTATUS st = D3DKMTOpenAdapterFromLuid(&open);
    if (st) {
        set_error("D3DKMTOpenAdapterFromLuid failed: 0x%08lx", st);
        return -1;
    }
    D3DKMT_CREATEDEVICE cd = { 0 };
    cd.hAdapter = open.hAdapter;
    int status = -1;
    st = D3DKMTCreateDevice(&cd);
    if (st) {
        set_error("D3DKMTCreateDevice failed: 0x%08lx", st);
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
        if (st) {
            set_error("D3DKMTEscape failed: 0x%08lx", st);
        } else {
            status = (int)cmd[7];
            if (status)
                set_error("The driver returned status %d for display id %u.", status, display);
        }
        free(buf);
        D3DKMT_DESTROYDEVICE dd = { cd.hDevice };
        D3DKMTDestroyDevice(&dd);
    }
    D3DKMT_CLOSEADAPTER ca = { open.hAdapter };
    D3DKMTCloseAdapter(&ca);
    return status;
}

int set_override(LUID adapter, UINT display, UINT enable, int *outputs)
{
    UINT ids[10];
    int n = 0;
    if (display) {
        if (send_override(adapter, display, 0) != 0)
            return -1;
        ids[n++] = display;
    } else {
        // Display id 1 is the internal panel; ids without display state report status 3.
        for (UINT id = 2; id <= 10; id++)
            if (send_override(adapter, id, 0) == 0)
                ids[n++] = id;
        if (!n) {
            set_error("The driver reported no external display outputs.");
            return -1;
        }
    }
    for (int i = 0; enable && i < n; i++)
        if (send_override(adapter, ids[i], 1) != 0)
            return -1;
    if (outputs)
        *outputs = n;
    return 0;
}
