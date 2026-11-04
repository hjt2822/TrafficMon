/* C ABI bridge between the pure-C Win32 UI and the C++ collection/storage core.
 * Everything in this header is C99-compatible; the implementation is C++17.
 *
 * Threading contract: create/start/tick/history/stop/destroy and the path helper
 * must all be called from one thread (the UI thread). The ETW worker thread is
 * internal to the collector and never crosses this ABI.
 *
 * Measurement contract:
 *   - upPerSec/downPerSec, today*, month* and NIC rows are NIC-level counters.
 *   - apps[] are application-level rows from ETW. The two bases must never be
 *     added together; they do not cover the same bytes.
 *   - When ETW is unavailable, etw=0 and appBytesAvailable=0. No synthetic
 *     application rows or zero-filled app totals are produced.
 */
#ifndef TM_BRIDGE_H
#define TM_BRIDGE_H

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TM_MAX_NICS 32
#define TM_MAX_APPS 64
#define TM_MAX_WATCH 8
#define TM_MAX_BUCKETS 32
#define TM_SIGNAL_POINTS 60
#define TM_ALIAS_CHARS 128
#define TM_GUID_CHARS 64
#define TM_NAME_CHARS 128
#define TM_PATH_CHARS 512
#define TM_STATUS_CHARS 128
#define TM_ERROR_CHARS 256

typedef struct TmEngine TmEngine;

typedef struct {
    wchar_t alias[TM_ALIAS_CHARS];
    wchar_t guid[TM_GUID_CHARS];
    uint64_t upPerSec;
    uint64_t downPerSec;
    int64_t todayUp;
    int64_t todayDown;
} TmNicRow;

typedef struct {
    wchar_t name[TM_NAME_CHARS];
    wchar_t path[TM_PATH_CHARS];
    int64_t up;
    int64_t down;
    int64_t upLan;
    int64_t downLan;
    int lanKnown;
} TmAppRow;

typedef struct {
    wchar_t name[TM_NAME_CHARS];
    wchar_t path[TM_PATH_CHARS];
    uint64_t upPerSec;       /* application basis, this tick only; needs ETW */
    uint64_t downPerSec;
} TmWatchRow;

typedef struct {
    int running;             /* 1 after tm_start() succeeds, until tm_stop() */
    int etw;                 /* 1 while the ETW collector is running */
    int appBytesAvailable;   /* 1 when apps[] carries real application bytes */
    int nicCount;
    int appCount;
    uint64_t upPerSec;       /* NIC basis, normalized by real elapsed time */
    uint64_t downPerSec;
    int64_t todayUp;         /* NIC basis, local day */
    int64_t todayDown;
    int64_t monthUp;         /* NIC basis, local month */
    int64_t monthDown;
    int64_t appTotalUp;      /* application basis, local day */
    int64_t appTotalDown;
    int64_t sessionStartUtc;
    int signalCount;         /* valid points, oldest first at index 0 */
    uint64_t signalUp[TM_SIGNAL_POINTS];   /* one-second NIC rates */
    uint64_t signalDown[TM_SIGNAL_POINTS];
    wchar_t status[TM_STATUS_CHARS];
    wchar_t error[TM_ERROR_CHARS];
    wchar_t dbPath[TM_PATH_CHARS];
    TmNicRow nics[TM_MAX_NICS];
    TmAppRow apps[TM_MAX_APPS];
    int watchCount;
    TmWatchRow watch[TM_MAX_WATCH];
} TmSnapshot;

typedef struct {
    int monthMode;           /* 0 = one local day, 1 = one local month */
    int count;               /* number of buckets in use (<= TM_MAX_BUCKETS) */
    int appCount;
    int32_t label[TM_MAX_BUCKETS]; /* day view: local hour 0-23; month: day 1-31 */
    int64_t up[TM_MAX_BUCKETS];    /* NIC basis */
    int64_t down[TM_MAX_BUCKETS];
    int64_t totalUp;
    int64_t totalDown;
    int64_t appTotalUp;      /* application basis over the period */
    int64_t appTotalDown;
    TmAppRow apps[TM_MAX_APPS];
} TmHistory;

/* Resolve traffic.sqlite3 beside the executable. Returns 0 on success, -1 with a message in err on failure. */
int tm_default_db_path(wchar_t* out, int outChars, wchar_t* err, int errChars);

/* Create the engine and open/migrate the database. Returns NULL on failure. */
TmEngine* tm_create(const wchar_t* dbPath, int noEtw, wchar_t* err, int errChars);

/* Start the ETW session (degraded, not failed, without administrator rights)
 * and the collection session row. Returns 0 when started, -1 on fatal error;
 * detailed state is available from tm_tick()'s snapshot. */
int tm_start(TmEngine* engine);

/* Collect one tick, flush to SQLite every fifth tick, and fill a snapshot.
 * Safe to call before tm_start() (returns an empty snapshot). */
void tm_tick(TmEngine* engine, TmSnapshot* out);

/* Replace the watched program list. paths is a newline-separated list of full
 * executable paths (application identities). Returns the count accepted, or -1. */
int tm_set_watch(TmEngine* engine, const wchar_t* paths);

/* Query one local day or month. anchorDay is YYYYMMDD. Returns 0 on success. */
int tm_history(TmEngine* engine, int monthMode, int32_t anchorDay, TmHistory* out,
               wchar_t* err, int errChars);

/* Stop collection, flush pending deltas, close the session row. Idempotent. */
void tm_stop(TmEngine* engine);

/* Destroy the engine. Calls tm_stop() if needed. */
void tm_destroy(TmEngine* engine);

/* Calendar helpers mirroring include/local_time.h (local-day semantics). */
int32_t tm_today(void);
int32_t tm_month_start(int32_t day);
int32_t tm_next_month(int32_t day);
int32_t tm_next_day(int32_t day);
int32_t tm_prev_month(int32_t day);
int32_t tm_prev_day(int32_t day);

#ifdef __cplusplus
}
#endif

#endif /* TM_BRIDGE_H */