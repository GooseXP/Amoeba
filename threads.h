#ifndef AMOEBA_THREADS_H
#define AMOEBA_THREADS_H

#include <stdio.h>     /* fprintf for LOG_ACTIONS */
#include <pthread.h>
#include <signal.h>
#include <unistd.h>    /* usleep */
#include "config.h"
#include "model.h"     /* CommandSettings, ThreadData (already defined here) */
#include "trend.h"     /* LearningTrendTracker */

/* Exposed by exec.c; used to stop the tuner loop */
extern volatile sig_atomic_t termination_requested;

/* ------------ Existing API (unchanged) ------------ */
/* NOTE: ThreadData comes from model.h — don't redefine it here. */

int  init_thread_sem(unsigned int max_concurrency);
void destroy_thread_sem(void);
void* worker_thread(void *arg);

/* ------------ New: Trend tuner support ------------ */

typedef struct TunerArgs {
    CommandSettings *settings;           /* protected by settings->mutex */
    LearningTrendTracker *tracker;       /* trend source */
    int interval_ms;                     /* how often to adjust */
} TunerArgs;

/* Small helper: clamp and set length with mutex */
static inline void tuner_set_length_locked(CommandSettings *s, int new_len) {
    if (!s) return;
    if (new_len < CMDMIN) new_len = CMDMIN;
    if (new_len > CMDMAX) new_len = CMDMAX;
    s->length = new_len;
}

/* Inline implementation so you don’t need a new .c file */
static inline void* tuner_thread(void *arg) {
    TunerArgs *ta = (TunerArgs *)arg;
    if (!ta || !ta->settings || !ta->tracker) return NULL;

    /* optional warm-up read */
    (void)get_moving_average(ta->tracker);

    while (!termination_requested) {
        int adj = analyze_learning_trend(ta->tracker);   /* >0 up, <0 down, 0 flat */

        if (adj != 0) {
            pthread_mutex_lock(&ta->settings->mutex);
            int cur = ta->settings->length;
            tuner_set_length_locked(ta->settings, cur + (adj > 0 ? 1 : -1));
            #if LOG_ACTIONS
            fprintf(stdout, "[tuner] length %s to %d\n",
                    (adj > 0 ? "↑" : "↓"),
                    ta->settings->length);
            #endif
            pthread_mutex_unlock(&ta->settings->mutex);
        }

        if (ta->interval_ms <= 0) ta->interval_ms = 1500;
        usleep((useconds_t)ta->interval_ms * 1000);
    }

    #if LOG_ACTIONS
    fprintf(stdout, "[tuner] exiting\n");
    #endif
    return NULL;
}

#endif /* AMOEBA_THREADS_H */
