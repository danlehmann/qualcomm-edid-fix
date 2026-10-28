// qcedid UI: shows every mode an external monitor's EDID gives the Snapdragon X display
// driver, where the 32-mode limit cuts the list off, and lets the user remove modes so
// the high refresh DisplayID modes fit, then loads the result as the driver's override.
#define UNICODE
#define _UNICODE
#include "qcedid_core.h"
#include <commctrl.h>
#include <commdlg.h>
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
};

#define WM_APP_RECHECK (WM_APP + 1)

static const int bpc_values[] = { 0, 6, 8, 10, 12 };

static struct {
    HWND wnd, monitor, open, refresh, list, info, summary, legend, bpc_label, bpc, id_label, display_id, auto_trim,
        reset, save, clear, apply;
    HFONT font, bold;
    UINT dpi;

    Monitor monitors[8];
    int nmonitors;
    int source;          // index into monitors, or -1 for a file
    WCHAR file[MAX_PATH];
    Edid orig;
    int from_cache;
    int have_edid;
    ModeTable table;
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

// ---------------------------------------------------------------------------
// Mode list
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
        set_text(g.display_id, L"%u", display_id_for(m));
        swprintf(desc, 512, L"%ls, Windows target %u, %ls%ls", m->name, m->target, m->instance,
                 g.from_cache ? L"\nAn override is active; showing the monitor's original EDID." : L"");
    } else if (g.file[0]) {
        if (edid_read_file(g.file, &g.orig))
            g.have_edid = 1;
        swprintf(desc, 512, L"%ls", g.have_edid ? g.file : L"");
        if (!g.have_edid)
            MessageBoxA(g.wnd, core_error(), "qcedid", MB_ICONERROR);
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
}

static void load_monitors(void)
{
    g.nmonitors = list_monitors(g.monitors, 8);
    SendMessageW(g.monitor, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g.nmonitors; i++) {
        WCHAR label[128];
        swprintf(label, 128, L"%ls (target %u)", g.monitors[i].name, g.monitors[i].target);
        SendMessageW(g.monitor, CB_ADDSTRING, 0, (LPARAM)label);
    }
    if (g.file[0])
        SendMessageW(g.monitor, CB_ADDSTRING, 0, (LPARAM)g.file);
    if (g.nmonitors) {
        g.source = 0;
        SendMessageW(g.monitor, CB_SETCURSEL, 0, 0);
    } else {
        g.source = -1;
        if (g.file[0])
            SendMessageW(g.monitor, CB_SETCURSEL, 0, 0);
    }
    load_source();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static void build_override(Edid *out)
{
    edid_build(&g.orig, &g.table, current_bpc(), out);
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
    SendMessageW(g.monitor, CB_RESETCONTENT, 0, 0);
    load_monitors();
    g.source = -1;
    SendMessageW(g.monitor, CB_SETCURSEL, g.nmonitors, 0);
    load_source();
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
    build_override(&e);
    if (!edid_write_file(path, &e))
        MessageBoxA(g.wnd, core_error(), "qcedid", MB_ICONERROR);
}

static UINT selected_display_id(void)
{
    WCHAR buf[16];
    GetWindowTextW(g.display_id, buf, 16);
    return (UINT)_wtoi(buf);
}

static void on_apply(void)
{
    Monitor *m = &g.monitors[g.source];
    UINT id = selected_display_id();
    if (id < 1 || id > 10) {
        MessageBoxW(g.wnd, L"The driver display id must be between 1 and 10.", L"qcedid", MB_ICONERROR);
        return;
    }
    if (!driver_tested(g.driver)) {
        WCHAR msg[512];
        swprintf(msg, 512,
                 L"Driver %hs is untested. The driver interface was reverse engineered from %hs and may differ.\n\n"
                 L"Continue anyway?", g.driver, tested_driver_version);
        if (MessageBoxW(g.wnd, msg, L"qcedid", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            return;
    }
    if (g.orig.blocks > MAX_BLOCKS) {
        MessageBoxW(g.wnd, L"This EDID has more than 4 blocks, which the driver's override file does not support.",
                    L"qcedid", MB_ICONERROR);
        return;
    }
    if (table_displayid_dropped(&g.table) &&
        MessageBoxW(g.wnd, L"Some DisplayID modes are still past the 32-mode limit. Apply anyway?", L"qcedid",
                    MB_ICONWARNING | MB_YESNO) != IDYES)
        return;
    if (!g.from_cache && !save_original(m, &g.orig)) {
        MessageBoxA(g.wnd, core_error(), "qcedid", MB_ICONERROR);
        return;
    }
    Edid e;
    build_override(&e);
    if (!write_override_xml(&e) || send_override(m->adapter, id, 0) != 0 || send_override(m->adapter, id, 1) != 0) {
        MessageBoxA(g.wnd, core_error(), "qcedid", MB_ICONERROR);
        return;
    }
    MessageBoxW(g.wnd,
                L"The override is loaded. Unplug the monitor and plug it back in, then choose the refresh rate in "
                L"Settings > Display > Advanced display.\n\nThe override lasts until the next reboot.",
                L"qcedid", MB_ICONINFORMATION);
}

static void on_clear(void)
{
    Monitor *m = &g.monitors[g.source];
    UINT id = selected_display_id();
    if (send_override(m->adapter, id, 0) != 0) {
        MessageBoxA(g.wnd, core_error(), "qcedid", MB_ICONERROR);
        return;
    }
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
    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    g.bold = CreateFontIndirectW(&ncm.lfMessageFont);
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
    MoveWindow(g.bpc_label, x, bottom + px(4), px(80), h, TRUE);
    x += px(80);
    MoveWindow(g.bpc, x, bottom, px(120), px(200), TRUE);
    x += px(120) + gap;
    MoveWindow(g.id_label, x, bottom + px(4), px(105), h, TRUE);
    x += px(105);
    MoveWindow(g.display_id, x, bottom, px(40), h, TRUE);
    x += px(40) + gap * 2;
    int bw2 = px(96);
    MoveWindow(g.auto_trim, x, bottom, bw2, h, TRUE);
    x += bw2 + gap;
    MoveWindow(g.reset, x, bottom, bw2, h, TRUE);
    x += bw2 + gap;
    MoveWindow(g.save, x, bottom, bw2, h, TRUE);
    MoveWindow(g.apply, m + w - bw2, bottom, bw2, h, TRUE);
    MoveWindow(g.clear, m + w - 2 * bw2 - gap, bottom, bw2, h, TRUE);
    RECT lr;
    GetClientRect(g.list, &lr);
    int rest = lr.right;
    for (int i = 0; i < 5; i++)
        rest -= ListView_GetColumnWidth(g.list, i);
    ListView_SetColumnWidth(g.list, 5, rest > px(120) ? rest : px(120));
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
    g.id_label = control(WC_STATICW, L"Driver display id:", 0, 0);
    g.display_id = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 0,
                                   0, 0, 0, g.wnd, (HMENU)ID_DISPLAY_ID, NULL, NULL);
    g.auto_trim = control(WC_BUTTONW, L"Auto trim", WS_TABSTOP, ID_AUTO_TRIM);
    g.reset = control(WC_BUTTONW, L"Reset", WS_TABSTOP, ID_RESET);
    g.save = control(WC_BUTTONW, L"Save EDID...", WS_TABSTOP, ID_SAVE);
    g.clear = control(WC_BUTTONW, L"Clear override", WS_TABSTOP, ID_CLEAR);
    g.apply = control(WC_BUTTONW, L"Apply", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_APPLY);
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

static void trim_log(void *ctx, const char *line)
{
    (void)ctx;
    (void)line;
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
        SetWindowPos(wnd, NULL, 0, 0, px(1000), px(740), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (!driver_version(g.driver, sizeof g.driver))
            strcpy(g.driver, "not found");
        if (__argc > 1)
            wcsncpy(g.file, __wargv[1], MAX_PATH - 1);
        load_monitors();
        if (g.file[0]) {
            g.source = -1;
            SendMessageW(g.monitor, CB_SETCURSEL, g.nmonitors, 0);
            load_source();
        }
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = px(900);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = px(480);
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
                load_source();
            }
            return 0;
        case ID_BPC:
            if (HIWORD(wp) == CBN_SELCHANGE)
                update_rows();
            return 0;
        case ID_OPEN: on_open(); return 0;
        case ID_REFRESH: load_monitors(); return 0;
        case ID_AUTO_TRIM:
            table_auto_trim(&g.table, current_bpc(), trim_log, NULL);
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
                if (r->locked && !checked)
                    PostMessageW(wnd, WM_APP_RECHECK, 0, 0);
                else if (checked != r->keep) {
                    r->keep = checked;
                    PostMessageW(wnd, WM_APP_RECHECK, 0, 0);
                }
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

    HWND wnd = CreateWindowExW(0, L"qcedid", L"qcedid - Snapdragon X external display modes", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1000, 740, NULL, NULL, inst, NULL);
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
