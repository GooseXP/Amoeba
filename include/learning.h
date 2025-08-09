#ifndef AMOEBA_LEARNING_H
#define AMOEBA_LEARNING_H

/*
 * learning.h — proximity-based similarity utilities
 *
 * Standalone helpers used by database.c to decide whether an observation
 * line is “new enough” to learn or effectively redundant.
 *
 * No heavy includes; header-only inline for tiny abs-diff helper.
 */

#include <stddef.h>   /* size_t */

#ifdef __cplusplus
extern "C" {
    #endif

    /* Tiny inline for hot loop usage */
    static inline int abs_diff_int(int a, int b) { return a > b ? a - b : b - a; }

    /**
     * array_similarity_proximity
     * --------------------------
     * Proximity-weighted similarity between two integer arrays.
     *
     * For each element in arr1, we find the closest matching value in arr2 by
     * positional distance and add 1/(1+distance) to the score. The maximum
     * per-element score is 1 (exact same index). Final result is normalized to
     * a percentage [0..100].
     *
     * Parameters:
     *   arr1, n1 : first array and its length (scoring is normalized by n1)
     *   arr2, n2 : second array and its length
     */
    float array_similarity_proximity(const int arr1[], int n1,
                                     const int arr2[], int n2);

    /**
     * line_similarity_proximity
     * -------------------------
     * Same as array_similarity_proximity, but each array may be terminated by -1.
     * max_len* bounds the scan to prevent overruns.
     *
     * Parameters:
     *   line1, max_len1 : first line buffer and its maximum usable length
     *   line2, max_len2 : second line buffer and its maximum usable length
     *
     * Returns:
     *   percentage [0..100]
     */
    float line_similarity_proximity(const int *line1, int max_len1,
                                    const int *line2, int max_len2);

    /**
     * is_redundant_line_proximity
     * ---------------------------
     * Compares a candidate tokenized line against an existing set of observation
     * lines and reports whether it’s redundant under a similarity threshold.
     *
     * Parameters:
     *   tokenized_line      : candidate line (not necessarily -1 terminated if you pass observationLength)
     *   observationLength   : cap for both candidate and existing lines when scoring
     *   entries             : array of existing lines (each int* is -1 terminated)
     *   numObservations     : number of existing lines
     *   threshold_percent   : redundancy threshold (e.g., 85.0f)
     *   out_best_index      : optional; receives index of best match (or -1)
     *   out_best_score      : optional; receives best similarity percentage
     *
     * Returns:
     *   1 if redundant (best_similarity >= threshold), else 0.
     */
    int is_redundant_line_proximity(const int *tokenized_line, int observationLength,
                                    int **entries, size_t numObservations,
                                    float threshold_percent,
                                    int *out_best_index, float *out_best_score);

    #ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMOEBA_LEARNING_H */
