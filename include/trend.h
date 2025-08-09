#ifndef AMOEBA_TREND_H
#define AMOEBA_TREND_H

/*
 * trend.h — moving-average tracking of learning value (lrnval)
 *
 * Utilities to maintain a circular buffer of recent lrnval samples,
 * compute a moving average, and provide a coarse trend indication.
 *
 * The underlying struct (LearningTrendTracker) is defined in model.h.
 */

#include "model.h"  /* LearningTrendTracker */

#ifdef __cplusplus
extern "C" {
    #endif

    /**
     * Initialize a LearningTrendTracker.
     * - Allocates the circular buffer (size = TREND_WINDOW_SIZE, set in config.h).
     * - Zeros indices/counters and initializes the mutex.
     */
    void init_trend_tracker(LearningTrendTracker *tracker);

    /**
     * Free resources associated with a LearningTrendTracker and destroy its mutex.
     * Safe to call on a partially initialized tracker.
     */
    void destroy_trend_tracker(LearningTrendTracker *tracker);

    /**
     * Push a new lrnval sample into the tracker and update the cached moving average.trend.h
     * Thread-safe: locks tracker->mutex internally.
     */
    void update_trend_tracker(LearningTrendTracker *tracker, int lrnval);

    /**
     * Read the current moving average (double). If there are no samples yet,
     * returns 0.0. Thread-safe.
     */
    double get_moving_average(const LearningTrendTracker *tracker);

    /**
     * Provide a coarse trend signal based on the moving average and recent samples.
     *
     * Return value convention:
     *   +1 : improving trend
     *    0 : flat/indeterminate
     *   -1 : declining trend
     *
     * Exact heuristic is implemented in trend.c.
     * Thread-safe.
     */
    int analyze_learning_trend(const LearningTrendTracker *tracker);

    #ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMOEBA_TREND_H */
