// src/trend.c
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "model.h"
#include "trend.h"

/* =========================
* Internal helpers
* ========================= */

static double compute_average(const LearningTrendTracker *t) {
    if (!t || t->count <= 0) return 0.0;
    long sum = 0;
    for (int i = 0; i < t->count; ++i) sum += t->lrnvals[i];
    return (double)sum / (double)t->count;
}

/* Compute average over the last `k` samples (k <= t->count). */
static double compute_recent_avg(const LearningTrendTracker *t, int k) {
    if (!t || k <= 0 || t->count <= 0) return 0.0;
    long sum = 0;
    int used = 0;
    for (int i = 0; i < k; ++i) {
        int idx = t->index - 1 - i;
        while (idx < 0) idx += t->window_size;
        sum += t->lrnvals[idx];
        used++;
    }
    return (used > 0) ? (double)sum / (double)used : 0.0;
}

/* Compute average over the older samples (exclude the last `k_recent`). */
static double compute_prior_avg(const LearningTrendTracker *t, int k_recent) {
    if (!t) return 0.0;
    int n = t->count;
    int prior = n - k_recent;
    if (prior <= 0) return 0.0;
    long sum = 0;
    for (int i = k_recent; i < n; ++i) {
        int idx = t->index - 1 - i;
        while (idx < 0) idx += t->window_size;
        sum += t->lrnvals[idx];
    }
    return (double)sum / (double)prior;
}

/* =========================
* Public API
* ========================= */

void init_trend_tracker(LearningTrendTracker *tracker) {
    if (!tracker) return;
    tracker->window_size = TREND_WINDOW_SIZE;
    tracker->lrnvals = (int*)calloc((size_t)tracker->window_size, sizeof(int));
    tracker->index = 0;
    tracker->count = 0;
    tracker->moving_average = 0.0;
    pthread_mutex_init(&tracker->mutex, NULL);
}

void destroy_trend_tracker(LearningTrendTracker *tracker) {
    if (!tracker) return;
    if (tracker->lrnvals) {
        free(tracker->lrnvals);
        tracker->lrnvals = NULL;
    }
    tracker->window_size = 0;
    tracker->index = 0;
    tracker->count = 0;
    tracker->moving_average = 0.0;
    pthread_mutex_destroy(&tracker->mutex);
}

void update_trend_tracker(LearningTrendTracker *tracker, int lrnval) {
    if (!tracker || !tracker->lrnvals) return;

    pthread_mutex_lock(&tracker->mutex);

    tracker->lrnvals[tracker->index] = lrnval;
    tracker->index = (tracker->index + 1) % tracker->window_size;
    if (tracker->count < tracker->window_size) tracker->count++;

    tracker->moving_average = compute_average(tracker);

    pthread_mutex_unlock(&tracker->mutex);
}

double get_moving_average(const LearningTrendTracker *tracker) {
    if (!tracker) return 0.0;
    pthread_mutex_lock((pthread_mutex_t*)&tracker->mutex);
    double ma = tracker->moving_average;
    pthread_mutex_unlock((pthread_mutex_t*)&tracker->mutex);
    return ma;
}

int analyze_learning_trend(const LearningTrendTracker *tracker) {
    if (!tracker) return 0;

    pthread_mutex_lock((pthread_mutex_t*)&tracker->mutex);

    int n = tracker->count;
    if (n < 2) {
        pthread_mutex_unlock((pthread_mutex_t*)&tracker->mutex);
        return 0; /* not enough data */
    }

    /* Compare the most recent half-window vs the prior half-window. */
    int k_recent = n / 2;
    if (k_recent <= 0) k_recent = 1;

    double recent = compute_recent_avg(tracker, k_recent);
    double prior  = compute_prior_avg(tracker, k_recent);
    double delta  = recent - prior;

    pthread_mutex_unlock((pthread_mutex_t*)&tracker->mutex);

    /* Simple threshold to avoid flapping on tiny changes. */
    const double EPS = 0.5; /* tweak if you want a more/less sensitive trend */
    if (delta > EPS)  return +1;
    if (delta < -EPS) return -1;
    return 0;
}
