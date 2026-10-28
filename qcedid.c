// qcedid command line tool. See README.md.
#include "qcedid_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void usage(void)
{
    printf("qcedid - restore DisplayID modes (e.g. 4K120) on Snapdragon X external displays\n\n"
           "  qcedid list                       show external monitors and whether they are affected\n"
           "  qcedid apply [target] [options]   load a trimmed EDID override (elevated)\n"
           "      --bpc N          declare N bits per colour (default: 8 only when needed)\n"
           "      --margin N       mode list slots to keep free after DisplayID (default: 4)\n"
           "      --display-id N   only this driver display id (default: all external outputs)\n"
           "      --force          allow untested drivers / EDIDs without DisplayID\n"
           "  qcedid clear [--display-id N]     remove the override\n"
           "  qcedid analyze <edid.bin> [--bpc N] [--margin N] [--out file]\n"
           "                                    show the mode table and what apply would change\n\n"
           "Overrides last until reboot. Replug the monitor after apply or clear.\n");
}

static const wchar_t *opt_value(int argc, wchar_t **argv, const wchar_t *name)
{
    for (int i = 0; i < argc - 1; i++)
        if (!wcscmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

static int has_flag(int argc, wchar_t **argv, const wchar_t *name)
{
    for (int i = 0; i < argc; i++)
        if (!wcscmp(argv[i], name))
            return 1;
    return 0;
}

// First argument that is neither an option nor an option's value.
static const wchar_t *positional(int argc, wchar_t **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!wcscmp(argv[i], L"--bpc") || !wcscmp(argv[i], L"--display-id") || !wcscmp(argv[i], L"--out") ||
            !wcscmp(argv[i], L"--margin")) {
            i++;
            continue;
        }
        if (argv[i][0] != L'-')
            return argv[i];
    }
    return NULL;
}

static int parse_bpc(int argc, wchar_t **argv, int *bpc)
{
    const wchar_t *v = opt_value(argc, argv, L"--bpc");
    *bpc = v ? _wtoi(v) : 0;
    if (v && (*bpc < 6 || *bpc > 16 || *bpc % 2)) {
        fprintf(stderr, "--bpc must be 6, 8, 10, 12, 14 or 16.\n");
        return 0;
    }
    return 1;
}

// Value of --margin, or TRIM_MARGIN.
static int parse_margin(int argc, wchar_t **argv)
{
    const wchar_t *v = opt_value(argc, argv, L"--margin");
    int margin = v ? _wtoi(v) : TRIM_MARGIN;
    return margin < 0 ? 0 : margin > MODE_LIMIT - 1 ? MODE_LIMIT - 1 : margin;
}

static void print_log(void *ctx, const char *line)
{
    (void)ctx;
    printf("  - %s\n", line);
}

static void print_table(const ModeTable *t)
{
    printf("  %-5s %-24s %-12s %10s %13s  %s\n", "Slot", "Source", "Resolution", "Refresh", "Pixel clock", "Status");
    for (int i = 0; i < t->n; i++) {
        const Row *r = &t->rows[i];
        char name[64], res[32] = "", slot[8] = "-", clk[24] = "", hz[16] = "";
        row_source_name(r, name, sizeof name);
        if (r->w)
            snprintf(res, sizeof res, "%dx%d%s", r->w, r->h, r->interlaced ? "i" : "");
        if (r->hz)
            snprintf(hz, sizeof hz, "%.2f Hz", r->hz);
        if (r->khz)
            snprintf(clk, sizeof clk, "%s%.2f MHz", r->khz_estimated ? "~" : "", r->khz / 1000.0);
        if (r->slot)
            snprintf(slot, sizeof slot, "%d", r->slot);
        printf("  %-5s %-24s %-12s %10s %13s  %s\n", slot, name, res, hz, clk, row_status_name(r));
    }
}

// Builds the override from orig into out, printing each change; t holds the result.
static int build(const Edid *orig, int bpc_choice, int margin, Edid *out, ModeTable *t)
{
    table_from_edid(orig, t);
    int bpc = table_effective_bpc(t, bpc_choice);
    if (bpc != t->declared_bpc)
        printf("  - declaring %d bpc (was %d)\n", bpc, t->declared_bpc);
    int ok = table_auto_trim(t, bpc, margin, print_log, NULL);
    table_compute(t, bpc);
    edid_build(orig, t, bpc, out);
    if (!ok)
        printf("  Warning: DisplayID timings still exceed the mode list.\n");
    return ok;
}

static int cmd_list(void)
{
    char ver[64] = "not found";
    driver_version(ver, sizeof ver);
    printf("Adreno driver %s\n\n", ver);
    Monitor mons[8];
    int n = list_monitors(mons, 8);
    if (!n)
        printf("No external monitors connected.\n");
    ModeTable *t = malloc(sizeof *t);
    for (int i = 0; i < n; i++) {
        Monitor *m = &mons[i];
        printf("[target %u] %ls  (%ls)\n", m->target, m->name, m->instance);
        if (!m->have_edid) {
            printf("    EDID not available\n\n");
            continue;
        }
        Edid orig;
        int from_cache;
        original_edid(m, &orig, &from_cache);
        table_from_edid(&orig, t);
        table_compute(t, table_effective_bpc(t, 0));
        int candidates = 0;
        for (int j = 0; j < t->n; j++)
            if (t->rows[j].source != SRC_DISPLAYID && t->rows[j].slot)
                candidates = t->rows[j].slot;
        printf("    EDID: %d blocks, %d mode list slots used before DisplayID (limit %d)\n", orig.blocks,
               candidates, MODE_LIMIT);
        for (int j = 0; j < t->n; j++) {
            Row *r = &t->rows[j];
            if (r->source == SRC_DISPLAYID)
                printf("      DisplayID %dx%d @ %.2f Hz (%.2f MHz): %s\n", r->w, r->h, r->hz, r->khz / 1000.0,
                       row_status_name(r));
        }
        if (from_cache)
            printf("    Override active (original EDID from cache).\n");
        else if (table_displayid_dropped(t))
            printf("    Affected. Fix: qcedid apply %u   (elevated), then replug the monitor\n", m->target);
        else
            printf("    Not affected by the mode list limit.\n");
        printf("\n");
    }
    free(t);
    return 0;
}

static int check_driver(int force)
{
    char ver[64];
    if (!driver_version(ver, sizeof ver)) {
        fprintf(stderr, "No Qualcomm Adreno display adapter found.\n");
        return 0;
    }
    if (driver_tested(ver))
        return 1;
    if (force) {
        printf("Driver %s is untested; continuing because of --force.\n", ver);
        return 1;
    }
    fprintf(stderr,
            "Driver %s is untested. The driver interface was checked against versions %s and may differ.\n"
            "Use --force to try anyway.\n",
            ver, tested_driver_version);
    return 0;
}

static Monitor *pick(Monitor *mons, int n, const wchar_t *arg)
{
    if (arg) {
        UINT target = (UINT)_wtoi(arg);
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

static int cmd_apply(int argc, wchar_t **argv)
{
    int force = has_flag(argc, argv, L"--force"), bpc;
    if (!parse_bpc(argc, argv, &bpc) || !check_driver(force))
        return 1;
    Monitor mons[8];
    Monitor *m = pick(mons, list_monitors(mons, 8), positional(argc, argv));
    if (!m)
        return 1;
    if (!m->have_edid) {
        fprintf(stderr, "Cannot read the EDID of target %u.\n", m->target);
        return 1;
    }
    Edid orig, e;
    int from_cache;
    original_edid(m, &orig, &from_cache);
    if (orig.blocks > MAX_BLOCKS) {
        fprintf(stderr, "EDID has %d blocks; the driver's override file supports %d.\n", orig.blocks, MAX_BLOCKS);
        return 1;
    }
    ModeTable *t = malloc(sizeof *t);
    table_from_edid(&orig, t);
    if (!table_count(t, SRC_DISPLAYID) && !force) {
        fprintf(stderr, "This EDID has no DisplayID timings, so the mode list limit is not what hides its modes.\n");
        return 1;
    }
    if (!from_cache && !save_original(m, &orig)) {
        fprintf(stderr, "%s\n", core_error());
        return 1;
    }
    printf("Building override for %ls (target %u):\n", m->name, m->target);
    build(&orig, bpc, parse_margin(argc, argv), &e, t);
    free(t);
    if (!write_override_xml(&e)) {
        fprintf(stderr, "%s\n", core_error());
        return 1;
    }
    const wchar_t *idv = opt_value(argc, argv, L"--display-id");
    int outputs;
    if (set_override(m->adapter, idv ? (UINT)_wtoi(idv) : 0, 1, &outputs) != 0) {
        fprintf(stderr, "%s\n", core_error());
        return 1;
    }
    printf("Override loaded on %d external output%s. Unplug and replug the monitor.\n"
           "It lasts until reboot. Until then, other monitors plugged into this computer get this EDID too;\n"
           "run qcedid clear before connecting a different monitor.\n", outputs, outputs == 1 ? "" : "s");
    return 0;
}

static int cmd_clear(int argc, wchar_t **argv)
{
    if (!check_driver(has_flag(argc, argv, L"--force")))
        return 1;
    Monitor mons[8];
    if (!list_monitors(mons, 8)) {
        fprintf(stderr, "No external monitor connected to find the display adapter; connect one and retry.\n");
        return 1;
    }
    const wchar_t *idv = opt_value(argc, argv, L"--display-id");
    int outputs;
    if (set_override(mons[0].adapter, idv ? (UINT)_wtoi(idv) : 0, 0, &outputs) != 0) {
        fprintf(stderr, "%s\n", core_error());
        return 1;
    }
    printf("Cleared the override on %d output%s. Replug the monitor.\n", outputs, outputs == 1 ? "" : "s");
    return 0;
}

static int cmd_analyze(int argc, wchar_t **argv)
{
    const wchar_t *path = positional(argc, argv);
    int bpc;
    if (!path) {
        usage();
        return 2;
    }
    if (!parse_bpc(argc, argv, &bpc))
        return 1;
    Edid orig, e;
    if (!edid_read_file(path, &orig)) {
        fprintf(stderr, "%s\n", core_error());
        return 1;
    }
    ModeTable *t = malloc(sizeof *t);
    table_from_edid(&orig, t);
    table_compute(t, t->declared_bpc ? t->declared_bpc : 8);
    printf("%ls: %d blocks, declared %d bpc\n\nAs the driver sees it:\n", path, orig.blocks, t->declared_bpc);
    print_table(t);
    printf("\nOverride:\n");
    build(&orig, bpc, parse_margin(argc, argv), &e, t);
    printf("\n");
    print_table(t);
    free(t);
    const wchar_t *out = opt_value(argc, argv, L"--out");
    if (out) {
        if (!edid_write_file(out, &e)) {
            fprintf(stderr, "%s\n", core_error());
            return 1;
        }
        printf("\nWrote %ls.\n", out);
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!wcscmp(argv[1], L"list"))
        return cmd_list();
    if (!wcscmp(argv[1], L"apply"))
        return cmd_apply(argc - 2, argv + 2);
    if (!wcscmp(argv[1], L"clear"))
        return cmd_clear(argc - 2, argv + 2);
    if (!wcscmp(argv[1], L"analyze"))
        return cmd_analyze(argc - 2, argv + 2);
    usage();
    return 2;
}
