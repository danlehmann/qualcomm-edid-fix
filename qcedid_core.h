// Shared EDID analysis and driver access for the qcedid command line tool and UI.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_BLOCKS 4         // edidoverride.xml parser accepts at most 4 blocks
#define MODE_LIMIT 32        // KMD mode list capacity per display
#define LINK_KBPS 25920000LL // 4-lane HBR3 payload bandwidth
#define MAX_ROWS 256
// Slots auto trim leaves free after the last DisplayID mode by default, since the slot
// model is inferred from the driver and may miss modes it adds (for example from a
// variable refresh range) or skips.
#define TRIM_MARGIN 4

typedef struct {
    BYTE b[MAX_BLOCKS * 128];
    int blocks;
} Edid;

typedef enum {
    SRC_ESTABLISHED,   // established timing bit
    SRC_STANDARD,      // standard timing (base block or 0xFA descriptor)
    SRC_DESCRIPTOR,    // established timings III / CVT 3-byte code descriptor
    SRC_DETAILED,      // base block detailed timing descriptor
    SRC_CTA_VIC,       // CTA-861 short video descriptor
    SRC_CTA_Y420,      // CTA-861 YCbCr 4:2:0 video data block entry
    SRC_CTA_DETAILED,  // CTA-861 detailed timing descriptor
    SRC_DISPLAYID,     // DisplayID type I / VII timing
} RowSource;

typedef enum {
    ST_LISTED,   // takes a slot within the mode list limit
    ST_DROPPED,  // would take a slot past the limit
    ST_REJECTED, // exceeds the link bandwidth, so the driver skips it without a slot
    ST_DUPLICATE, // same timing as an earlier row, so the driver skips it without a slot
    ST_REMOVED,  // removed from the override
} RowStatus;

// One mode candidate, in the order the driver parses them.
typedef struct {
    RowSource source;
    int offset;    // EDID byte offset of the bit, timing, descriptor or SVD
    int bit;       // bit within the byte (established timings)
    int index;     // ordinal among rows of this source in its CTA block
    int w, h, interlaced;
    double hz;
    int khz;       // pixel clock; estimated from the resolution when khz_estimated
    int khz_estimated;
    int vic;       // CTA-861 VIC, 0 for other sources
    int native;
    int weight;    // number of modes the row stands for
    int locked;    // preferred timing or DisplayID timing: never removed
    int keep;
    int slot;      // last mode list slot the row takes, 0 if it takes none
    RowStatus status;
} Row;

typedef struct {
    Row rows[MAX_ROWS];
    int n;
    int declared_bpc; // 0 if the EDID does not declare one
} ModeTable;

void table_from_edid(const Edid *e, ModeTable *t);
// Bits per colour the override declares: bpc_choice if non-zero, else 8 when the
// fastest DisplayID timing would not fit 4-lane HBR3 at the declared depth.
int table_effective_bpc(const ModeTable *t, int bpc_choice);
void table_compute(ModeTable *t, int bpc);
// Number of DisplayID rows that would be dropped by the mode list limit.
int table_displayid_dropped(const ModeTable *t);
int table_count(const ModeTable *t, RowSource source);
void table_reset(ModeTable *t);
// Unticks the rows of t (built from an original EDID) that have no counterpart in active
// (built from an override made from it), so t shows the override's selection.
void table_match(ModeTable *t, const ModeTable *active);
// Removes the least valuable rows until every DisplayID row has a slot at least margin
// below the limit (TRIM_MARGIN is the default). log (may be NULL)
// receives one line per change. Returns 1 if all DisplayID rows fit.
int table_auto_trim(ModeTable *t, int bpc, int margin, void (*log)(void *ctx, const char *line), void *ctx);
void edid_build(const Edid *orig, const ModeTable *t, int bpc, Edid *out);
const char *row_source_name(const Row *r, char *buf, size_t len);
const char *row_status_name(const Row *r);

int edid_read_file(const WCHAR *path, Edid *e);
int edid_write_file(const WCHAR *path, const Edid *e);

typedef struct {
    LUID adapter;
    UINT target;
    WCHAR name[64];
    WCHAR instance[256]; // DISPLAY\<id>\<instance>
    Edid edid;           // EDID as Windows reports it (overridden while an override is active)
    int have_edid;
} Monitor;

int list_monitors(Monitor *mons, int max);
// The monitor's unmodified EDID. from_cache is set when the current EDID is an
// active override and the original came from the cache.
void original_edid(const Monitor *m, Edid *out, int *from_cache);
int save_original(const Monitor *m, const Edid *e);

int driver_version(char *out, size_t len); // 0 if no Adreno adapter
int driver_tested(const char *version);
extern const char *const tested_driver_version; // tested versions, for messages

int write_override_xml(const Edid *e);
// Sends the EDID override command: enable = 1 loads edidoverride.xml, 0 clears.
// Returns the command status (0 = ok), or -1 when the escape itself failed.
int send_override(LUID adapter, UINT display, UINT enable);
// Clears the override on one KMD display id, or on every external output when display is
// 0 (the mapping from Windows targets to KMD display ids is not known), then loads
// edidoverride.xml on them when enable is set. outputs (may be NULL) receives the number
// of outputs affected. Returns 0 on success, -1 on failure.
int set_override(LUID adapter, UINT display, UINT enable, int *outputs);

// Description of the last failure of a function above.
const char *core_error(void);
