// qcedid UI. The simple view says what the connected monitor could do, what the driver
// currently offers, and fixes it with one button. The advanced view shows every mode the
// monitor's EDID gives the Snapdragon X display driver, where the 32-mode limit cuts the
// list off, and lets the user choose which modes to remove before loading the override.
#define UNICODE
#define _UNICODE
#include "qcedid_core.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

enum {
    ID_MONITOR = 100,
    ID_OPEN,
    ID_REFRESH,
    ID_LIST,
    ID_BPC,
    ID_DISPLAY_ID,
    ID_AUTO_TRIM,
    ID_RESET,
    ID_SAVE,
    ID_CLEAR,
    ID_APPLY,
    ID_FIX,
    ID_MODE,
};

#define WM_APP_RECHECK (WM_APP + 1)
#define TIMER_RELOAD 1

static const int bpc_values[] = { 0, 6, 8, 10, 12 };

// What the simple view offers for the selected monitor.
typedef enum {
    S_NONE,      // no monitor or no readable EDID
    S_FIXABLE,   // the fix makes a better mode available
    S_ACTIVE,    // an override is active
    S_FINE,      // all modes the link can carry are available
    S_FILE,      // an EDID file is selected
} SimpleState;

typedef enum {
    PENDING_NONE,
    PENDING_FIX,   // fix loaded, waiting for the monitor to be replugged
    PENDING_UNDO,  // fix cleared, waiting for the monitor to be replugged
} Pending;

static struct {
    HWND wnd, monitor, open, refresh, list, info, summary, legend, bpc_label, bpc, id_label, display_id, auto_trim,
        reset, save, clear, apply, message, fix, mode;
    HFONT font, bold, big;
    UINT dpi;
    int expert;

    Monitor monitors[8];
    int nmonitors;
    int source;          // index into monitors, or -1 for a file
    WCHAR file[MAX_PATH];
    Edid orig;
    int from_cache;
    int have_edid;
    ModeTable table;     // advanced view: the user's selection
    ModeTable fixed;     // simple view: the auto trimmed table
    int fixed_bpc;
    SimpleState state;
    Pending pending;
    int populating;
    char driver[64];
} g;

static int px(int v) { return MulDiv(v, (int)g.dpi, 96); }

static int current_bpc(void)
{
    int sel = (int)SendMessageW(g.bpc, CB_GETCURSEL, 0, 0);
    return table_effective_bpc(&g.table, sel > 0 ? bpc_values[sel] : 0);
}

static void set_text(HWND w, const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    SetWindowTextW(w, buf);
}

static void error_box(void) { MessageBoxA(g.wnd, core_error(), "qcedid", MB_ICONERROR); }

// ---------------------------------------------------------------------------
// Simple view
// ---------------------------------------------------------------------------

// Best mode of a table: highest resolution, then highest refresh rate. With listed_only,
// only modes the driver keeps count.
static const Row *best_mode(const ModeTable *t, int listed_only)
{
    const Row *best = NULL;
    for (int i = 0; i < t->n; i++) {
        const Row *r = &t->rows[i];
        if (!r->w || r->interlaced || (listed_only && r->status != ST_LISTED))
            continue;
        long long area = (long long)r->w * r->h, best_area = best ? (long long)best->w * best->h : 0;
        if (!best || area > best_area || (area == best_area && r->hz > best->hz + 0.5))
            best = r;
    }
    return best;
}

static int better(const Row *a, const Row *b)
{
    if (!a)
        return 0;
    if (!b)
        return 1;
    long long aa = (long long)a->w * a->h, ba = (long long)b->w * b->h;
    return aa > ba || (aa == ba && a->hz > b->hz + 0.5);
}

static void mode_name(const Row *r, WCHAR *out, size_t len)
{
    static const struct { int w, h; const WCHAR *name; } names[] = {
        { 1920, 1080, L"1080p" }, { 2560, 1440, L"1440p" }, { 3440, 1440, L"UWQHD" }, { 3840, 2160, L"4K" },
        { 4096, 2160, L"4K" },    { 5120, 1440, L"DQHD" },  { 5120, 2160, L"5K2K" },  { 5120, 2880, L"5K" },
        { 7680, 4320, L"8K" },
    };
    const WCHAR *name = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (names[i].w == r->w && names[i].h == r->h)
            name = names[i].name;
    if (name)
        swprintf(out, len, L"%ls (%d × %d) at %.0f Hz", name, r->w, r->h, r->hz);
    else
        swprintf(out, len, L"%d × %d at %.0f Hz", r->w, r->h, r->hz);
}

static void evaluate_simple(void)
{
    WCHAR msg[1024], now_s[64], fix_s[64], top_s[64];
    const WCHAR *name = g.source >= 0 ? g.monitors[g.source].name : L"";

    if (g.pending == PENDING_FIX && g.from_cache)
        g.pending = PENDING_NONE;
    if (g.pending == PENDING_UNDO && !g.from_cache)
        g.pending = PENDING_NONE;

    if (!driver_version(g.driver, sizeof g.driver)) {
        g.state = S_NONE;
        set_text(g.message, L"No Qualcomm Adreno display driver was found. This tool is for Snapdragon X laptops.");
        goto done;
    }
    if (g.source < 0 && !g.file[0]) {
        g.state = S_NONE;
        set_text(g.message, L"No external monitor is connected.\n\nConnect your monitor; this window updates by itself.");
        goto done;
    }
    if (!g.have_edid) {
        g.state = S_NONE;
        set_text(g.message, L"The description (EDID) of %ls could not be read.", name);
        goto done;
    }

    ModeTable *now = malloc(sizeof *now);
    table_from_edid(&g.orig, now);
    table_compute(now, now->declared_bpc ? now->declared_bpc : 8);
    table_from_edid(&g.orig, &g.fixed);
    g.fixed_bpc = table_effective_bpc(&g.fixed, 0);
    table_auto_trim(&g.fixed, g.fixed_bpc, NULL, NULL);
    table_compute(&g.fixed, g.fixed_bpc);

    const Row *now_best = best_mode(now, 1), *fix_best = best_mode(&g.fixed, 1), *top = best_mode(&g.fixed, 0);
    now_s[0] = fix_s[0] = top_s[0] = 0;
    if (now_best)
        mode_name(now_best, now_s, 64);
    if (fix_best)
        mode_name(fix_best, fix_s, 64);
    if (top)
        mode_name(top, top_s, 64);
    WCHAR too_fast[256] = L"";
    if (better(top, fix_best))
        swprintf(too_fast, 256, L"\n\nIts %ls mode needs more bandwidth than this connection can carry.", top_s);

    if (g.source < 0) {
        g.state = S_FILE;
        if (better(fix_best, now_best))
            swprintf(msg, 1024, L"This monitor supports %ls, but the Qualcomm driver only offers up to %ls. "
                                L"Fixing it needs the monitor connected.%ls", fix_s, now_s, too_fast);
        else
            swprintf(msg, 1024, L"This monitor is not affected: the driver offers up to %ls.%ls", now_s, too_fast);
    } else if (g.pending == PENDING_FIX) {
        g.state = S_FIXABLE;
        swprintf(msg, 1024, L"Almost done: unplug %ls and plug it back in.\n\nThen choose %ls in Settings > Display "
                            L"> Advanced display.", name, fix_s);
    } else if (g.pending == PENDING_UNDO) {
        g.state = S_ACTIVE;
        swprintf(msg, 1024, L"The fix is removed. Unplug %ls and plug it back in to return to the original modes.",
                 name);
    } else if (g.from_cache) {
        g.state = S_ACTIVE;
        swprintf(msg, 1024, L"The fix is active: %ls can now use %ls.\n\nPick it in Settings > Display > Advanced "
                            L"display. The fix lasts until you restart the computer; press Undo fix before "
                            L"connecting a different monitor.%ls", name, fix_s, too_fast);
    } else if (better(fix_best, now_best)) {
        g.state = S_FIXABLE;
        swprintf(msg, 1024, L"Your %ls supports %ls, but because of a bug in the Qualcomm display driver, only up to "
                            L"%ls is available.%ls", name, fix_s, now_s, too_fast);
    } else {
        g.state = S_FINE;
        swprintf(msg, 1024, L"Your %ls is not affected by the Qualcomm driver bug: up to %ls is available.%ls", name,
                 now_s, too_fast);
    }
    free(now);
    SetWindowTextW(g.message, msg);

done:
    SetWindowTextW(g.fix, g.state == S_ACTIVE ? L"Undo fix" : L"Fix");
    EnableWindow(g.fix, (g.state == S_FIXABLE && g.pending == PENDING_NONE) ||
                            (g.state == S_ACTIVE && g.pending == PENDING_NONE));
}

// ---------------------------------------------------------------------------
// Advanced view: mode list
// ---------------------------------------------------------------------------

static void update_rows(void)
{
    int bpc = current_bpc();
    table_compute(&g.table, bpc);
    g.populating = 1;
    for (int i = 0; i < g.table.n; i++) {
        const Row *r = &g.table.rows[i];
        WCHAR slot[16] = L"", status[64];
        if (r->slot)
            swprintf(slot, 16, L"%d", r->slot);
        swprintf(status, 64, L"%hs", row_status_name(r));
        ListView_SetItemText(g.list, i, 0, slot);
        ListView_SetItemText(g.list, i, 5, status);
        ListView_SetCheckState(g.list, i, r->keep);
    }
    g.populating = 0;
    InvalidateRect(g.list, NULL, FALSE);

    int used = 0, did_listed = 0, did_dropped = 0, did_rejected = 0;
    WCHAR best[64] = L"";
    for (int i = 0; i < g.table.n; i++) {
        const Row *r = &g.table.rows[i];
        if (r->slot > used)
            used = r->slot;
        if (r->source != SRC_DISPLAYID)
            continue;
        if (r->status == ST_LISTED) {
            did_listed++;
            swprintf(best, 64, L"%dx%d @ %.0f Hz", r->w, r->h, r->hz);
        } else if (r->status == ST_DROPPED) {
            did_dropped++;
        } else if (r->status == ST_REJECTED) {
            did_rejected++;
        }
    }
    int did = did_listed + did_dropped + did_rejected;
    if (!g.have_edid)
        set_text(g.summary, L"");
    else if (!did)
        set_text(g.summary, L"%d modes for %d slots. This EDID has no DisplayID modes, so the limit is not what hides "
                            L"its modes.", used, MODE_LIMIT);
    else if (did_dropped)
        set_text(g.summary, L"%d modes for %d slots: %d DisplayID mode%ls dropped. Untick lower modes or use Auto trim.",
                 used, MODE_LIMIT, did_dropped, did_dropped == 1 ? L" is" : L"s are");
    else if (did_rejected && !did_listed)
        set_text(g.summary, L"%d modes for %d slots, but the DisplayID modes exceed the link bandwidth at %d-bit. "
                            L"Choose a lower colour depth.", used, MODE_LIMIT, bpc);
    else
        set_text(g.summary, L"%d modes for %d slots: all DisplayID modes fit (%ls at %d-bit).", used, MODE_LIMIT, best,
                 bpc);

    int auto_bpc = table_effective_bpc(&g.table, 0);
    WCHAR label[32];
    swprintf(label, 32, L"Auto (%d-bit)", auto_bpc);
    int sel = (int)SendMessageW(g.bpc, CB_GETCURSEL, 0, 0);
    SendMessageW(g.bpc, CB_DELETESTRING, 0, 0);
    SendMessageW(g.bpc, CB_INSERTSTRING, 0, (LPARAM)label);
    SendMessageW(g.bpc, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
}

static void fill_list(void)
{
    g.populating = 1;
    ListView_DeleteAllItems(g.list);
    for (int i = 0; i < g.table.n; i++) {
        const Row *r = &g.table.rows[i];
        char name[64];
        WCHAR wname[64], res[32] = L"", hz[32] = L"", clk[32] = L"";
        row_source_name(r, name, sizeof name);
        swprintf(wname, 64, L"%hs", name);
        if (r->w)
            swprintf(res, 32, L"%dx%d%ls", r->w, r->h, r->interlaced ? L"i" : L"");
        if (r->hz)
            swprintf(hz, 32, L"%.2f Hz", r->hz);
        if (r->khz)
            swprintf(clk, 32, L"%ls%.2f MHz", r->khz_estimated ? L"~" : L"", r->khz / 1000.0);
        LVITEMW it = { LVIF_TEXT | LVIF_PARAM };
        it.iItem = i;
        it.pszText = L"";
        it.lParam = i;
        ListView_InsertItem(g.list, &it);
        ListView_SetItemText(g.list, i, 1, wname);
        ListView_SetItemText(g.list, i, 2, res);
        ListView_SetItemText(g.list, i, 3, hz);
        ListView_SetItemText(g.list, i, 4, clk);
    }
    g.populating = 0;
    update_rows();
}

static void update_buttons(void)
{
    int monitor = g.source >= 0 && g.have_edid;
    EnableWindow(g.apply, monitor);
    EnableWindow(g.clear, g.source >= 0);
    EnableWindow(g.display_id, g.source >= 0);
    EnableWindow(g.save, g.have_edid);
    EnableWindow(g.auto_trim, g.have_edid);
    EnableWindow(g.reset, g.have_edid);
}

static void load_source(void)
{
    g.have_edid = 0;
    g.from_cache = 0;
    memset(&g.table, 0, sizeof g.table);
    WCHAR desc[512] = L"";
    if (g.source >= 0) {
        Monitor *m = &g.monitors[g.source];
        if (m->have_edid) {
            original_edid(m, &g.orig, &g.from_cache);
            g.have_edid = 1;
        }
        swprintf(desc, 512, L"%ls, Windows target %u, %ls%ls", m->name, m->target, m->instance,
                 g.from_cache ? L"\nAn override is active; showing the monitor's original EDID." : L"");
    } else if (g.file[0]) {
        if (edid_read_file(g.file, &g.orig))
            g.have_edid = 1;
        swprintf(desc, 512, L"%ls", g.have_edid ? g.file : L"");
        if (!g.have_edid)
            error_box();
    }
    if (g.have_edid) {
        table_from_edid(&g.orig, &g.table);
        WCHAR more[160];
        swprintf(more, 160, L"\nAdreno driver %hs%ls. EDID: %d blocks, declares %d-bit colour.", g.driver,
                 driver_tested(g.driver) ? L" (tested)" : L" (untested)", g.orig.blocks, g.table.declared_bpc);
        wcscat(desc, more);
    } else if (g.source >= 0) {
        wcscat(desc, L"\nThe EDID of this monitor could not be read.");
    }
    SetWindowTextW(g.info, desc);
    fill_list();
    update_buttons();
    evaluate_simple();
}

// Reloads the monitor list, keeping the selected monitor if it is still connected.
static void load_monitors(void)
{
    WCHAR selected[256] = L"";
    if (g.source >= 0 && g.source < g.nmonitors)
        wcscpy(selected, g.monitors[g.source].instance);
    int was_file = g.source < 0 && g.file[0];

    g.nmonitors = list_monitors(g.monitors, 8);
    SendMessageW(g.monitor, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g.nmonitors; i++) {
        WCHAR label[128];
        swprintf(label, 128, L"%ls", g.monitors[i].name);
        SendMessageW(g.monitor, CB_ADDSTRING, 0, (LPARAM)label);
    }
    if (g.file[0])
        SendMessageW(g.monitor, CB_ADDSTRING, 0, (LPARAM)g.file);

    g.source = g.nmonitors ? 0 : -1;
    for (int i = 0; i < g.nmonitors; i++)
        if (!wcscmp(g.monitors[i].instance, selected))
            g.source = i;
    if (was_file || (!g.nmonitors && g.file[0]))
        g.source = -1;
    SendMessageW(g.monitor, CB_SETCURSEL, g.source >= 0 ? g.source : g.file[0] ? g.nmonitors : -1, 0);
    load_source();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static int confirm_driver(void)
{
    if (driver_tested(g.driver))
        return 1;
    WCHAR msg[512];
    swprintf(msg, 512,
             L"Driver %hs is untested. This tool talks to the driver through an interface that was reverse "
             L"engineered from versions %hs and may differ.\n\nContinue anyway?",
             g.driver, tested_driver_version);
    return MessageBoxW(g.wnd, msg, L"qcedid", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

// Saves the original EDID, writes the override built from t and loads it for display id,
// or for all external outputs when id is 0.
static int load_override(const ModeTable *t, int bpc, UINT id)
{
    Monitor *m = &g.monitors[g.source];
    if (g.orig.blocks > MAX_BLOCKS) {
        MessageBoxW(g.wnd, L"This monitor's EDID has more than 4 blocks, which the driver's override does not "
                           L"support.", L"qcedid", MB_ICONERROR);
        return 0;
    }
    if (!g.from_cache && !save_original(m, &g.orig)) {
        error_box();
        return 0;
    }
    Edid e;
    edid_build(&g.orig, t, bpc, &e);
    if (!write_override_xml(&e) || set_override(m->adapter, id, 1, NULL) != 0) {
        error_box();
        return 0;
    }
    return 1;
}

static void on_fix(void)
{
    Monitor *m = &g.monitors[g.source];
    if (g.state == S_ACTIVE) {
        if (set_override(m->adapter, 0, 0, NULL) != 0) {
            error_box();
            return;
        }
        g.pending = PENDING_UNDO;
        evaluate_simple();
        MessageBoxW(g.wnd, L"The fix is removed. Unplug the monitor and plug it back in.", L"qcedid",
                    MB_ICONINFORMATION);
        return;
    }
    if (!confirm_driver() || !load_override(&g.fixed, g.fixed_bpc, 0))
        return;
    g.pending = PENDING_FIX;
    evaluate_simple();
    MessageBoxW(g.wnd,
                L"Now unplug the monitor and plug it back in.\n\nThe fix lasts until you restart the computer. Until "
                L"then it applies to every USB-C display output, so press Undo fix before connecting a different "
                L"monitor.",
                L"qcedid", MB_ICONINFORMATION);
}

static void on_open(void)
{
    WCHAR path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"EDID files (*.bin;*.edid;*.dat)\0*.bin;*.edid;*.dat\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;
    wcscpy(g.file, path);
    g.source = -1;
    load_monitors();
}

static void on_save(void)
{
    WCHAR path[MAX_PATH] = L"override.bin";
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"EDID files (*.bin)\0*.bin\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bin";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn))
        return;
    Edid e;
    edid_build(&g.orig, &g.table, current_bpc(), &e);
    if (!edid_write_file(path, &e))
        error_box();
}

// The driver display id typed in the advanced view, 0 (all external outputs) when empty,
// or -1 when invalid.
static int selected_display_id(void)
{
    WCHAR buf[16];
    GetWindowTextW(g.display_id, buf, 16);
    if (!buf[0])
        return 0;
    int id = _wtoi(buf);
    if (id < 1 || id > 10) {
        MessageBoxW(g.wnd, L"The driver display id must be between 1 and 10, or empty for all external outputs.",
                    L"qcedid", MB_ICONERROR);
        return -1;
    }
    return id;
}

static void on_apply(void)
{
    int id = selected_display_id();
    if (id < 0)
        return;
    if (!confirm_driver())
        return;
    if (table_displayid_dropped(&g.table) &&
        MessageBoxW(g.wnd, L"Some DisplayID modes are still past the 32-mode limit. Apply anyway?", L"qcedid",
                    MB_ICONWARNING | MB_YESNO) != IDYES)
        return;
    if (!load_override(&g.table, current_bpc(), (UINT)id))
        return;
    g.pending = PENDING_FIX;
    evaluate_simple();
    MessageBoxW(g.wnd,
                L"The override is loaded. Unplug the monitor and plug it back in, then choose the refresh rate in "
                L"Settings > Display > Advanced display.\n\nThe override lasts until the next reboot.",
                L"qcedid", MB_ICONINFORMATION);
}

static void on_clear(void)
{
    Monitor *m = &g.monitors[g.source];
    int id = selected_display_id();
    if (id < 0)
        return;
    if (set_override(m->adapter, (UINT)id, 0, NULL) != 0) {
        error_box();
        return;
    }
    g.pending = PENDING_UNDO;
    evaluate_simple();
    MessageBoxW(g.wnd, L"The override is cleared. Unplug the monitor and plug it back in.", L"qcedid",
                MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void make_fonts(void)
{
    if (g.font)
        DeleteObject(g.font);
    if (g.bold)
        DeleteObject(g.bold);
    if (g.big)
        DeleteObject(g.big);
    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    g.bold = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = lf.lfHeight * 4 / 3;
    g.big = CreateFontIndirectW(&lf);
}

static BOOL CALLBACK set_font(HWND child, LPARAM font)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static void apply_fonts(void)
{
    EnumChildWindows(g.wnd, set_font, (LPARAM)g.font);
    SendMessageW(g.summary, WM_SETFONT, (WPARAM)g.bold, TRUE);
    SendMessageW(g.message, WM_SETFONT, (WPARAM)g.big, TRUE);
    SendMessageW(g.fix, WM_SETFONT, (WPARAM)g.big, TRUE);
    static const int widths[] = { 48, 170, 110, 90, 105, 170 };
    for (int i = 0; i < 6; i++)
        ListView_SetColumnWidth(g.list, i, px(widths[i]));
}

static void layout(void)
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    int m = px(12), h = px(26), gap = px(8), w = rc.right - 2 * m, y = m;
    int bw = px(110);

    if (!g.expert) {
        MoveWindow(g.monitor, m, y, w - bw - gap, px(300), TRUE);
        MoveWindow(g.refresh, m + w - bw, y, bw, h, TRUE);
        y += h + px(20);
        int fh = px(40), bottom = rc.bottom - m - fh;
        MoveWindow(g.message, m, y, w, bottom - px(12) - y, TRUE);
        MoveWindow(g.fix, m + w - px(160), bottom, px(160), fh, TRUE);
        MoveWindow(g.mode, m, bottom + (fh - h) / 2, px(120), h, TRUE);
        return;
    }

    MoveWindow(g.monitor, m, y, w - 2 * (bw + gap), px(300), TRUE);
    MoveWindow(g.open, m + w - 2 * bw - gap, y, bw, h, TRUE);
    MoveWindow(g.refresh, m + w - bw, y, bw, h, TRUE);
    y += h + gap;
    MoveWindow(g.info, m, y, w, px(36), TRUE);
    y += px(36) + gap;
    MoveWindow(g.summary, m, y, w, px(20), TRUE);
    y += px(20) + gap / 2;
    MoveWindow(g.legend, m, y, w, px(20), TRUE);
    y += px(20) + gap / 2;

    int bottom = rc.bottom - m - h;
    MoveWindow(g.list, m, y, w, bottom - gap - y, TRUE);

    int x = m;
    MoveWindow(g.mode, x, bottom, px(80), h, TRUE);
    x += px(80) + gap * 2;
    MoveWindow(g.bpc_label, x, bottom + px(4), px(80), h, TRUE);
    x += px(80);
    MoveWindow(g.bpc, x, bottom, px(110), px(200), TRUE);
    x += px(110) + gap;
    MoveWindow(g.id_label, x, bottom + px(4), px(150), h, TRUE);
    x += px(150);
    MoveWindow(g.display_id, x, bottom, px(36), h, TRUE);
    x += px(36) + gap * 2;
    int bw2 = px(90);
    MoveWindow(g.auto_trim, x, bottom, bw2, h, TRUE);
    x += bw2 + gap;
    MoveWindow(g.reset, x, bottom, bw2, h, TRUE);
    x += bw2 + gap;
    MoveWindow(g.save, x, bottom, bw2, h, TRUE);
    MoveWindow(g.apply, m + w - bw2, bottom, bw2, h, TRUE);
    MoveWindow(g.clear, m + w - 2 * bw2 - gap, bottom, bw2 + px(10), h, TRUE);
    RECT lr;
    GetClientRect(g.list, &lr);
    int rest = lr.right;
    for (int i = 0; i < 5; i++)
        rest -= ListView_GetColumnWidth(g.list, i);
    ListView_SetColumnWidth(g.list, 5, rest > px(120) ? rest : px(120));
}

// Shows the controls of the current view and sizes the window for it.
static void show_view(void)
{
    HWND expert_only[] = { g.open, g.info, g.summary, g.legend, g.list, g.bpc_label, g.bpc, g.id_label,
                           g.display_id, g.auto_trim, g.reset, g.save, g.clear, g.apply };
    for (size_t i = 0; i < sizeof expert_only / sizeof *expert_only; i++)
        ShowWindow(expert_only[i], g.expert ? SW_SHOW : SW_HIDE);
    ShowWindow(g.message, g.expert ? SW_HIDE : SW_SHOW);
    ShowWindow(g.fix, g.expert ? SW_HIDE : SW_SHOW);
    SetWindowTextW(g.mode, g.expert ? L"← Simple" : L"Advanced →");
    SetWindowPos(g.wnd, NULL, 0, 0, px(g.expert ? 1060 : 640), px(g.expert ? 740 : 340),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
}

static HWND control(const WCHAR *cls, const WCHAR *text, DWORD style, int id)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g.wnd, (HMENU)(INT_PTR)id, NULL,
                           NULL);
}

static void create_controls(void)
{
    g.monitor = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, ID_MONITOR);
    g.open = control(WC_BUTTONW, L"Open EDID file...", WS_TABSTOP, ID_OPEN);
    g.refresh = control(WC_BUTTONW, L"Refresh", WS_TABSTOP, ID_REFRESH);
    g.message = control(WC_STATICW, L"", SS_NOPREFIX, 0);
    g.fix = control(WC_BUTTONW, L"Fix", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_FIX);
    g.mode = control(WC_BUTTONW, L"", WS_TABSTOP, ID_MODE);
    g.info = control(WC_STATICW, L"", SS_NOPREFIX, 0);
    g.summary = control(WC_STATICW, L"", SS_NOPREFIX, 0);
    g.legend = control(WC_STATICW,
                       L"Green: DisplayID mode in the list.   Red: past the 32-mode limit, dropped by the driver.   "
                       L"Yellow: exceeds the link bandwidth.   Untick a row to remove it.",
                       SS_NOPREFIX, 0);
    g.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL, 0, 0,
                             0, 0, g.wnd, (HMENU)ID_LIST, NULL, NULL);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    static const WCHAR *cols[] = { L"Slot", L"Source", L"Resolution", L"Refresh", L"Pixel clock", L"Status" };
    for (int i = 0; i < 6; i++) {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_SUBITEM | LVCF_FMT };
        c.fmt = i == 0 || i == 3 || i == 4 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.pszText = (WCHAR *)cols[i];
        c.iSubItem = i;
        ListView_InsertColumn(g.list, i, &c);
    }
    g.bpc_label = control(WC_STATICW, L"Colour depth:", 0, 0);
    g.bpc = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_BPC);
    static const WCHAR *bpcs[] = { L"Auto", L"6-bit", L"8-bit", L"10-bit", L"12-bit" };
    for (int i = 0; i < 5; i++)
        SendMessageW(g.bpc, CB_ADDSTRING, 0, (LPARAM)bpcs[i]);
    SendMessageW(g.bpc, CB_SETCURSEL, 0, 0);
    g.id_label = control(WC_STATICW, L"Display id (empty = all):", 0, 0);
    g.display_id = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 0,
                                   0, 0, 0, g.wnd, (HMENU)ID_DISPLAY_ID, NULL, NULL);
    g.auto_trim = control(WC_BUTTONW, L"Auto trim", WS_TABSTOP, ID_AUTO_TRIM);
    g.reset = control(WC_BUTTONW, L"Reset", WS_TABSTOP, ID_RESET);
    g.save = control(WC_BUTTONW, L"Save EDID...", WS_TABSTOP, ID_SAVE);
    g.clear = control(WC_BUTTONW, L"Clear override", WS_TABSTOP, ID_CLEAR);
    g.apply = control(WC_BUTTONW, L"Apply", WS_TABSTOP, ID_APPLY);
}

static LRESULT custom_draw(NMLVCUSTOMDRAW *cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        int i = (int)cd->nmcd.dwItemSpec;
        if (i < 0 || i >= g.table.n)
            return CDRF_DODEFAULT;
        const Row *r = &g.table.rows[i];
        int did = r->source == SRC_DISPLAYID;
        switch (r->status) {
        case ST_LISTED:
            if (did) {
                cd->clrTextBk = RGB(198, 239, 206);
                cd->clrText = RGB(0, 97, 0);
            }
            break;
        case ST_DROPPED:
            cd->clrTextBk = did ? RGB(255, 180, 190) : RGB(255, 228, 230);
            cd->clrText = RGB(156, 0, 6);
            break;
        case ST_REJECTED:
            cd->clrTextBk = RGB(255, 242, 204);
            cd->clrText = RGB(140, 90, 0);
            break;
        case ST_DUPLICATE:
        case ST_REMOVED:
            cd->clrText = RGB(150, 150, 150);
            break;
        }
        if (did)
            SelectObject(cd->nmcd.hdc, g.bold);
        return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
    }
    case CDDS_ITEMPOSTPAINT: {
        // Line under the row that fills the last slot.
        int i = (int)cd->nmcd.dwItemSpec;
        if (i < 0 || i >= g.table.n)
            return CDRF_DODEFAULT;
        const Row *r = &g.table.rows[i];
        if (r->slot >= MODE_LIMIT && r->slot - r->weight < MODE_LIMIT) {
            RECT rc;
            ListView_GetItemRect(g.list, i, &rc, LVIR_BOUNDS);
            HPEN pen = CreatePen(PS_SOLID, px(2), RGB(200, 0, 0));
            HGDIOBJ old = SelectObject(cd->nmcd.hdc, pen);
            MoveToEx(cd->nmcd.hdc, rc.left, rc.bottom - px(1), NULL);
            LineTo(cd->nmcd.hdc, rc.right, rc.bottom - px(1));
            SelectObject(cd->nmcd.hdc, old);
            DeleteObject(pen);
        }
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g.wnd = wnd;
        g.dpi = GetDpiForWindow(wnd);
        create_controls();
        make_fonts();
        apply_fonts();
        if (!driver_version(g.driver, sizeof g.driver))
            strcpy(g.driver, "not found");
        if (__argc > 1) {
            wcsncpy(g.file, __wargv[1], MAX_PATH - 1);
            g.source = -1;
        }
        load_monitors();
        show_view();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = px(g.expert ? 960 : 480);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = px(g.expert ? 480 : 280);
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        make_fonts();
        apply_fonts();
        RECT *rc = (RECT *)lp;
        SetWindowPos(wnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        // A monitor was plugged or unplugged; Windows updates its EDID shortly after.
        SetTimer(wnd, TIMER_RELOAD, 1500, NULL);
        return msg == WM_DEVICECHANGE ? TRUE : 0;
    case WM_TIMER:
        if (wp == TIMER_RELOAD) {
            KillTimer(wnd, TIMER_RELOAD);
            load_monitors();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == g.summary)
            SetTextColor((HDC)wp, table_displayid_dropped(&g.table) ? RGB(170, 0, 0) : RGB(0, 110, 0));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MONITOR:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(g.monitor, CB_GETCURSEL, 0, 0);
                g.source = sel < g.nmonitors ? sel : -1;
                g.pending = PENDING_NONE;
                load_source();
            }
            return 0;
        case ID_BPC:
            if (HIWORD(wp) == CBN_SELCHANGE)
                update_rows();
            return 0;
        case ID_MODE:
            g.expert = !g.expert;
            show_view();
            return 0;
        case ID_FIX: on_fix(); return 0;
        case ID_OPEN: on_open(); return 0;
        case ID_REFRESH: load_monitors(); return 0;
        case ID_AUTO_TRIM:
            table_auto_trim(&g.table, current_bpc(), NULL, NULL);
            update_rows();
            return 0;
        case ID_RESET:
            table_reset(&g.table);
            update_rows();
            return 0;
        case ID_SAVE: on_save(); return 0;
        case ID_APPLY: on_apply(); return 0;
        case ID_CLEAR: on_clear(); return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->idFrom != ID_LIST)
            break;
        if (h->code == NM_CUSTOMDRAW)
            return custom_draw((NMLVCUSTOMDRAW *)lp);
        if (h->code == LVN_ITEMCHANGED && !g.populating) {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) &&
                nm->iItem >= 0 && nm->iItem < g.table.n) {
                Row *r = &g.table.rows[nm->iItem];
                int checked = ListView_GetCheckState(g.list, nm->iItem) != 0;
                if (!r->locked || checked)
                    r->keep = checked;
                PostMessageW(wnd, WM_APP_RECHECK, 0, 0);
            }
        }
        break;
    }
    case WM_APP_RECHECK:
        update_rows();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"qcedid";
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(0, L"qcedid", L"Snapdragon X monitor fix", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 640, 340, NULL, NULL, inst, NULL);
    ShowWindow(wnd, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
