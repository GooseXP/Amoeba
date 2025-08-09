// src/learning.c
#include <stddef.h>  /* size_t */
#include "learning.h"

/* =========================
* Internal helpers
* ========================= */

static int eff_len_terminated(const int *a, int max_len) {
    if (!a || max_len <= 0) return 0;
    int n = 0;
    while (n < max_len && a[n] != -1) n++;
    return n;
}

/* =========================
* Public API
* ========================= */

float array_similarity_proximity(const int arr1[], int n1, const int arr2[], int n2) {
    if (!arr1 || !arr2 || n1 <= 0 || n2 <= 0) return 0.0f;

    float total_score = 0.0f;
    float max_score   = (float)n1; /* normalize by arr1 length */

    for (int i = 0; i < n1; i++) {
        int best_match_index = -1;
        int min_distance = n2; /* worst case */

        int a = arr1[i];
        for (int j = 0; j < n2; j++) {
            if (a == arr2[j]) {
                int d = abs_diff_int(i, j);
                if (d < min_distance) {
                    min_distance = d;
                    best_match_index = j;
                }
            }
        }

        if (best_match_index != -1) {
            float proximity_score = 1.0f / (1.0f + (float)min_distance);
            total_score += proximity_score;
        }
    }

    return (total_score / max_score) * 100.0f;
}

float line_similarity_proximity(const int *line1, int max_len1, const int *line2, int max_len2) {
    int n1 = eff_len_terminated(line1, max_len1);
    int n2 = eff_len_terminated(line2, max_len2);
    if (n1 <= 0 || n2 <= 0) return 0.0f;
    return array_similarity_proximity(line1, n1, line2, n2);
}

int is_redundant_line_proximity(const int *tokenized_line, int observationLength, int **entries, size_t numObservations, float threshold_percent, int *out_best_index, float *out_best_score) {
    if (!tokenized_line || observationLength <= 0) {
        if (out_best_index) *out_best_index = -1;
        if (out_best_score) *out_best_score = 0.0f;
        return 0;
    }

    float best = 0.0f;
    int best_idx = -1;

    for (size_t i = 0; i < numObservations; i++) {
        int *row = entries ? entries[i] : NULL;
        if (!row) continue;

        float s = line_similarity_proximity(tokenized_line, observationLength, row, observationLength /* cap comparison */);
        if (s > best) {
            best = s;
            best_idx = (int)i;
        }
        if (best >= threshold_percent) break; /* early exit */
    }

    if (out_best_index) *out_best_index = best_idx;
    if (out_best_score) *out_best_score = best;

    return (best >= threshold_percent) ? 1 : 0;
}
