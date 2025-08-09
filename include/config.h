#ifndef AMOEBA_CONFIG_H
#define AMOEBA_CONFIG_H

/*
 * config.h — central build-time configuration for Amoeba
 *
 * Only macros and light compile-time options here.
 * No heavy system headers and no function prototypes.
 */

/* =========================
 * Storage & parsing limits
 * ========================= */

/* Max tokens stored for a single observation line. */
#ifndef LINEBUFFER
#define LINEBUFFER 100
#endif

/* Max characters per token (must be > 1 to allow NUL). */
#ifndef WRDBUFFER
#define WRDBUFFER 100
#endif

/* Command argument bounds (number of tokens in a generated command). */
#ifndef CMDMAX
#define CMDMAX 10
#endif
#ifndef CMDMIN
#define CMDMIN 1
#endif

/* Database search “scope” (% of words to sample when constructing commands). */
#ifndef SRCHMIN
#define SRCHMIN 1   /* percent */
#endif
#ifndef SRCHMAX
#define SRCHMAX 100 /* percent */
#endif

/* Sentinel used to terminate token index arrays (observation lines & command arrays). */
#ifndef IDX_TERMINATOR
#define IDX_TERMINATOR (-1)
#endif

/* =========================
 * PATH seeding controls
 * ========================= */

#ifndef LOG_SEEDING
#define LOG_SEEDING 1
#endif

/* Limit how many entries we add per directory during PATH scan (0 = unlimited). */
#ifndef MAX_SEED_PER_DIR
#define MAX_SEED_PER_DIR 5000
#endif

/* Print a progress line every N files. */
#ifndef SEED_LOG_EVERY
#define SEED_LOG_EVERY 200
#endif

/* Bail out of a single directory after N seconds (0 = no timeout). */
#ifndef DIR_SCAN_TIMEOUT_SEC
#define DIR_SCAN_TIMEOUT_SEC 8
#endif

/* Skip symlinks while seeding executables from PATH. */
#ifndef SKIP_SYMLINKS
#define SKIP_SYMLINKS 1
#endif

/* =========================
 * Learning & scoring
 * ========================= */

/* Reward/penalty values used by association updates. */
#ifndef REWARD
#define REWARD 10
#endif
#ifndef PENALTY
#define PENALTY 1
#endif

/* Moving average window for learning values (lrnval). */
#ifndef TREND_WINDOW_SIZE
#define TREND_WINDOW_SIZE 10
#endif

/* % similarity at/above which a line is considered redundant. */
#ifndef REDUNDANCY_THRESHOLD
#define REDUNDANCY_THRESHOLD 75.0f
#endif

/* Optional knobs for windowed redundancy checks (currently unused by learning.h). */
#ifndef REDUNDANCY_WINDOW
#define REDUNDANCY_WINDOW 10         /* compare against last N observations */
#endif
#ifndef REDUNDANCY_MIN_OVERLAP
#define REDUNDANCY_MIN_OVERLAP 1     /* require at least this many token matches */
#endif

/* Store redundant observations too? (1=yes, 0=no). */
#ifndef STORE_REDUNDANT
#define STORE_REDUNDANT 1
#endif

/* =========================
 * Execution & runtime
 * ========================= */

#ifndef RUNTIME
#define RUNTIME 10            /* child process allowed runtime (seconds) */
#endif
#ifndef KILL_ATTEMPTS
#define KILL_ATTEMPTS 3       /* escalation attempts (e.g., SIGTERM → SIGKILL) */
#endif

/* =========================
 * Concurrency
 * ========================= */

#ifndef MAX_THREADS
#define MAX_THREADS 8
#endif
#ifndef COMMANDS_PER_THREAD
#define COMMANDS_PER_THREAD 2
#endif

/* =========================
 * Persistence (file names)
 * ========================= */

#ifndef TOKENS_FILE
#define TOKENS_FILE "tokens.txt"
#endif
#ifndef VALUES_FILE
#define VALUES_FILE "values.csv"
#endif
#ifndef OBSERVATIONS_FILE
#define OBSERVATIONS_FILE "observations.csv"
#endif

/* =========================
 * Logging
 * ========================= */

#ifndef LOG_ACTIONS
#define LOG_ACTIONS 1         /* 1 = print agent actions, 0 = quiet */
#endif

#ifndef LOG_OUTPUT_PREVIEW
#define LOG_OUTPUT_PREVIEW 200 /* max bytes of command output to preview in logs */
#endif

/* Extra learning logs (e.g., redundancy decisions). */
#ifndef VERBOSE_LOG
#define VERBOSE_LOG 0
#endif

/* =========================
 * Utility macros
 * ========================= */

#ifndef MIN
#define MIN(a,b) (( (a) < (b) ) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a,b) (( (a) > (b) ) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(x,lo,hi) ( ((x) < (lo)) ? (lo) : ( ((x) > (hi)) ? (hi) : (x) ) )
#endif

/* =========================
 * Persistence (file names)
 * ========================= */

#ifndef DB_DIR
#define DB_DIR "data"   /* change to "." for current dir, or ".amoeba" if you prefer hidden */
#endif

#ifndef TOKENS_FILE
#define TOKENS_FILE        DB_DIR "/tokens.txt"
#endif
#ifndef VALUES_FILE
#define VALUES_FILE        DB_DIR "/values.csv"
#endif
#ifndef OBSERVATIONS_FILE
#define OBSERVATIONS_FILE  DB_DIR "/observations.csv"
#endif

/* =========================
 * Sanity checks
 * ========================= */

#if (CMDMAX) <= 0
# error "CMDMAX must be > 0"
#endif

#if (CMDMIN) <= 0
# error "CMDMIN must be > 0"
#endif

#if (CMDMIN) > (CMDMAX)
# error "CMDMIN must be <= CMDMAX"
#endif

#if (LINEBUFFER) <= 0
# error "LINEBUFFER must be > 0"
#endif

#if (WRDBUFFER) <= 1
# error "WRDBUFFER must be > 1 (needs room for NUL)"
#endif

#if (SRCHMIN) < 0 || (SRCHMAX) > 100 || (SRCHMIN) > (SRCHMAX)
# error "SRCHMIN/SRCHMAX must satisfy 0 <= SRCHMIN <= SRCHMAX <= 100"
#endif

#if (MAX_THREADS) <= 0
# error "MAX_THREADS must be > 0"
#endif

#if (COMMANDS_PER_THREAD) <= 0
# error "COMMANDS_PER_THREAD must be > 0"
#endif

#if __STDC_VERSION__ >= 201112L
_Static_assert(CMDMIN <= CMDMAX, "CMDMIN must be <= CMDMAX");
_Static_assert(LINEBUFFER > 0, "LINEBUFFER must be > 0");
_Static_assert(WRDBUFFER > 1, "WRDBUFFER must be > 1");
#endif

#endif /* AMOEBA_CONFIG_H */
