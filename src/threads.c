// src/threads.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>
#include <semaphore.h>
#include <time.h>      // nanosleep, clock_gettime
#include <stdarg.h>    // va_list
#include <ctype.h>     // isprint

#include "config.h"
#include "model.h"
#include "threads.h"
#include "command.h"
#include "exec.h"      // termination_requested
#include "database.h"
#include "trend.h"

/* =========================
* Global semaphore
* ========================= */
sem_t thread_sem;

/* =========================
* Logging (optional)
* ========================= */

static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;

static void logf_safe(const char *fmt, ...) {
#if LOG_ACTIONS
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_mtx);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    pthread_mutex_unlock(&log_mtx);
    va_end(ap);
#else
    (void)fmt;
#endif
}

/* Render a compact, printable preview of output (first N bytes). */
static void preview_output(const char *in, size_t n, char *out, size_t outsz) {
#if LOG_ACTIONS
    if (!in || !out || outsz == 0) return;
    size_t i = 0, o = 0;
    for (; in[i] && i < n && o + 1 < outsz; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\n') {
            if (o + 2 < outsz) { out[o++]='\\'; out[o++]='n'; }
        } else if (c == '\r') {
            if (o + 2 < outsz) { out[o++]='\\'; out[o++]='r'; }
        } else if (isprint(c)) {
            out[o++] = (char)c;
        } else {
            if (o + 4 < outsz) {
                static const char HEX[] = "0123456789ABCDEF";
                out[o++]='\\'; out[o++]='x';
                out[o++]=HEX[c>>4]; out[o++]=HEX[c&0xF];
            }
        }
    }
    out[o] = '\0';
#else
    (void)in; (void)n; (void)out; (void)outsz;
#endif
}

/* =========================
* Internal helpers
* ========================= */

/* Build a shell command string from token indices.
* Returns a heap-allocated NUL-terminated string the caller must free().
* On failure or empty command, returns NULL.
*/
static char *build_command_line(const Words *words, const int cmd[CMDMAX + 1]) {
    if (!words || !cmd) return NULL;

    /* first pass: compute needed length */
    size_t total = 0;
    int argc = 0;

    /* Words is shared; lock for consistent reads. Cast away const only to lock. */
    pthread_mutex_lock((pthread_mutex_t *)&words->mutex);
    for (int i = 0; i < CMDMAX && cmd[i] != IDX_TERMINATOR; ++i) {
        int idx = cmd[i];
        if (idx < 0 || (size_t)idx >= words->numWords) continue;
        const char *w = words->token[idx];
        if (!w) continue;
        total += strlen(w);
        argc++;
        if (i) total += 1; /* space */
    }
    if (argc == 0) {
        pthread_mutex_unlock((pthread_mutex_t *)&words->mutex);
        return NULL;
    }

    char *line = (char *)malloc(total + 1);
    if (!line) {
        pthread_mutex_unlock((pthread_mutex_t *)&words->mutex);
        return NULL;
    }

    /* second pass: concatenate */
    size_t off = 0;
    int first = 1;
    for (int i = 0; i < CMDMAX && cmd[i] != IDX_TERMINATOR; ++i) {
        int idx = cmd[i];
        if (idx < 0 || (size_t)idx >= words->numWords) continue;
        const char *w = words->token[idx];
        if (!w) continue;

        if (!first) line[off++] = ' ';
        size_t len = strlen(w);
        memcpy(line + off, w, len);
        off += len;
        first = 0;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&words->mutex);

    line[off] = '\0';
    return line;
}

/* Interruptible semaphore wait: returns 0 on acquired, -1 on shutdown/error. */
static int sem_wait_interruptible(sem_t *s) {
    for (;;) {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            /* Fallback: blocking wait but still break on EINTR; check shutdown flag */
            if (sem_wait(s) == 0) return 0;
            if (errno == EINTR && termination_requested) return -1;
            if (errno == EINTR) continue;
            return -1;
        }
        /* wait ~200ms at a time */
        ts.tv_nsec += 200 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        if (sem_timedwait(s, &ts) == 0) return 0;
        if (errno == ETIMEDOUT) {
            if (termination_requested) return -1;
            continue;
        }
        if (errno == EINTR) {
            if (termination_requested) return -1;
            continue;
        }
        /* other error */
        return -1;
    }
}

/* =========================
* Public API
* ========================= */

int init_thread_sem(unsigned int max_concurrent) {
    if (sem_init(&thread_sem, 0, max_concurrent) != 0) {
        return -1;
    }
    return 0;
}

void destroy_thread_sem(void) {
    (void)sem_destroy(&thread_sem);
}

void *worker_thread(void *arg) {
    ThreadData *data = (ThreadData *)arg;
    if (!data || !data->words || !data->observations || !data->settings || !data->tracker) {
        return NULL;
    }

    /* Limit how many workers run their critical work simultaneously. */
    if (sem_wait_interruptible(&thread_sem) != 0) {
        return NULL;
    }

    logf_safe("[T%lu] worker started\n", (unsigned long)pthread_self());

    while (!termination_requested) {
        int cmd_indices[CMDMAX + 1];
        int argc = construct_command(data->words, data->settings, cmd_indices);
        if (argc <= 0) {
            /* Nothing to do yet; brief yield so we don't spin hot. */
            struct timespec ts = {0, 50 * 1000 * 1000}; // 50 ms
            nanosleep(&ts, NULL);
            continue;
        }

        char *cmdline = build_command_line(data->words, cmd_indices);
        if (!cmdline || cmdline[0] == '\0') {
            free(cmdline);
            continue;
        }

#if LOG_ACTIONS
        logf_safe("[T%lu] $ %s\n", (unsigned long)pthread_self(), cmdline);
#endif

        char *output = execute_command(cmdline);

#if LOG_ACTIONS
        if (!output) {
            logf_safe("[T%lu] ! exec failed (NULL output)\n", (unsigned long)pthread_self());
        }
#endif

        if (output) {
            int lrnval = update_database(data->words, data->observations, output, cmd_indices);
            update_trend_tracker(data->tracker, lrnval);

#if LOG_ACTIONS
            char prev[LOG_OUTPUT_PREVIEW + 8];
            preview_output(output, LOG_OUTPUT_PREVIEW, prev, sizeof prev);
            double ma = get_moving_average(data->tracker);
            logf_safe("[T%lu] -> lrn=%d, avg=%.2f, out=%zuB: \"%s\"\n",
                      (unsigned long)pthread_self(), lrnval, ma, strlen(output), prev);
#endif
            free(output);
        }

        free(cmdline);
    }

    logf_safe("[T%lu] worker stopping (signal)\n", (unsigned long)pthread_self());
    (void)sem_post(&thread_sem);
    return NULL;
}
