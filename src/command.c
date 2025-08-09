// src/command.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>   // uintptr_t
#include <limits.h>   // LONG_MIN

#include "config.h"
#include "model.h"
#include "command.h"
#include "assoc.h"

/* =========================
* RNG helpers
* ========================= */

static void ensure_seeded(void) {
    static int seeded = 0;
    if (!seeded) {
        unsigned s = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)&seeded;
        srand(s);
        seeded = 1;
    }
}

static int rand_between(int lo, int hi_inclusive) {
    if (hi_inclusive <= lo) return lo;
    int span = hi_inclusive - lo + 1;
    return lo + (int)(rand() % span);
}

/* Fisher–Yates (partial) to choose first k unique items */
static void partial_shuffle(int *arr, int n, int k) {
    for (int i = 0; i < k && i < n; ++i) {
        int j = rand_between(i, n - 1);
        int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

/* =========================
* Scoring helpers (sparse)
* ========================= */

/* Sum association strengths between candidate 'w' at position 'pos'
*   and already chosen arguments chosen[0..chosen_cnt-1] at their positions.
*   NOTE: Caller holds words->mutex for consistency. */
static long pair_score(const Words *words,
                       int w, int pos,
                       const int *chosen, int chosen_cnt) {
    long s = 0;
    for (int q = 0; q < chosen_cnt; ++q) {
        int wq = chosen[q];
        int pos_q = q; /* by construction, chosen[q] is placed at index q */
        if (wq < 0) continue;
        if ((size_t)w  >= words->numWords) continue;
        if ((size_t)wq >= words->numWords) continue;

        /* Include both directions; map may not be symmetric. */
        s += assoc_get(&words->assoc, w,  pos, wq, pos_q);
        s += assoc_get(&words->assoc, wq, pos_q, w,  pos);
    }
    return s;
}

/* Greedy pick: at position pos, select the candidate with max pair_score.
*   If all scores equal, pick random among them. */
static int greedy_pick(const Words *words,
                       const int *cands, int cand_cnt,
                       const int *chosen, int chosen_cnt,
                       int pos) {
    long best = LONG_MIN;
    int best_indices[LINEBUFFER]; /* generous bound */
    int best_count = 0;

    for (int i = 0; i < cand_cnt; ++i) {
        int w = cands[i];
        long s = pair_score(words, w, pos, chosen, chosen_cnt);
        if (s > best) {
            best = s;
            best_indices[0] = i;
            best_count = 1;
        } else if (s == best) {
            if (best_count < (int)(sizeof(best_indices)/sizeof(best_indices[0])))
                best_indices[best_count++] = i;
        }
    }

    if (best_count <= 0) {
        /* fallback: pick random index from [0..cand_cnt-1] */
        return (cand_cnt > 0) ? rand_between(0, cand_cnt - 1) : -1;
    }
    /* break ties randomly */
    int which = rand_between(0, best_count - 1);
    return best_indices[which];
}

/* =========================
* Public API
* ========================= */

int construct_command(const Words *words,
                      const CommandSettings *settings,
                      int out_cmd[CMDMAX + 1]) {
    ensure_seeded();

    if (!words || !settings || !out_cmd) {
        if (out_cmd) out_cmd[0] = IDX_TERMINATOR;
        return 0;
    }

    /* Snapshot settings while holding its mutex */
    int want_len, scope_pct;
    pthread_mutex_lock((pthread_mutex_t*)&settings->mutex);
    want_len  = settings->length;
    scope_pct = settings->scope;
    pthread_mutex_unlock((pthread_mutex_t*)&settings->mutex);

    want_len  = CLAMP(want_len,  CMDMIN, CMDMAX);

    /* Lock the vocabulary for consistent view during construction */
    pthread_mutex_lock((pthread_mutex_t*)&words->mutex);

    size_t N = words->numWords;
    if (N == 0) {
        pthread_mutex_unlock((pthread_mutex_t*)&words->mutex);
        out_cmd[0] = IDX_TERMINATOR;
        return 0;
    }
    if (want_len > (int)N) want_len = (int)N;

    int sample_size = (int)((double)N * (double)CLAMP(scope_pct, SRCHMIN, SRCHMAX) / 100.0 + 0.5);
    if (sample_size < 1) sample_size = 1;
    if (sample_size > (int)N) sample_size = (int)N;

    /* Build candidate index list [0..N-1] and sample 'sample_size' without replacement */
    int *candidates = (int*)malloc((size_t)N * sizeof(int));
    if (!candidates) {
        pthread_mutex_unlock((pthread_mutex_t*)&words->mutex);
        out_cmd[0] = IDX_TERMINATOR;
        return 0;
    }
    for (size_t i = 0; i < N; ++i) candidates[i] = (int)i;

    /* Partial shuffle to select K candidates */
    partial_shuffle(candidates, (int)N, sample_size);

    /* Greedy construction using sparse associations */
    int chosen[CMDMAX];
    int argc = 0;

    /* Start: pick one at random among K candidates to avoid O(N^2) */
    {
        int pick_i = rand_between(0, sample_size - 1);
        chosen[argc++] = candidates[pick_i];
        /* remove from pool by swapping with the last of the sample window */
        candidates[pick_i] = candidates[sample_size - 1];
        sample_size--;
    }

    /* Continue greedy picks */
    while (argc < want_len && sample_size > 0) {
        int best_idx_in_cands = greedy_pick(words, candidates, sample_size, chosen, argc, argc);
        if (best_idx_in_cands < 0) {
            /* fallback: random */
            best_idx_in_cands = rand_between(0, sample_size - 1);
        }
        int w = candidates[best_idx_in_cands];
        chosen[argc++] = w;

        /* remove selected from pool */
        candidates[best_idx_in_cands] = candidates[sample_size - 1];
        sample_size--;
    }

    /* Output */
    for (int i = 0; i < argc; ++i) out_cmd[i] = chosen[i];
    out_cmd[argc] = IDX_TERMINATOR;

    free(candidates);
    pthread_mutex_unlock((pthread_mutex_t*)&words->mutex);
    return argc;
}
