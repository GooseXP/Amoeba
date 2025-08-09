#ifndef AMOEBA_DATABASE_H
#define AMOEBA_DATABASE_H

/*
 * database.h — vocabulary/observations persistence & mutation API
 *
 * Responsibilities:
 *   - Initialize/teardown in-memory stores (Words, Observations)
 *   - Grow (reallocate) structures as new words/lines are learned
 *   - Update the database from command output (learning)
 *   - Load/save to simple text/CSV files
 *   - Seed vocabulary from executables in $PATH
 *
 * Implementation notes:
 *   - Words->assoc is a sparse hashmap of associations:
 *       key (i,pi,k,pk) -> value (int)
 *     Missing keys are interpreted as zero.
 *   - Public functions here lock/unlock the owned mutexes internally unless documented.
 */

#include "model.h"   /* Words, Observations */
#include "config.h"  /* file name defaults, limits */

#ifdef __cplusplus
extern "C" {
    #endif

    /* =========================
     * Lifecycle
     * ========================= */

    /**
     * Initialize an empty Words store.
     * - Sets numWords=0, token=NULL, assoc initialized, mutex initialized.
     */
    void init_words(Words *words);

    /**
     * Initialize an empty Observations store.
     * - Sets numObservations=0, entries=NULL, initializes mutex.
     */
    void init_observations(Observations *observations);

    /**
     * Free all heap allocations owned by Words and Observations and destroy mutexes.
     * Safe to call on partially initialized structures.
     */
    void cleanup_database(Words *words, Observations *observations);

    /* =========================
     * Growing / Reallocation
     * ========================= */

    /**
     * Extend Words to accommodate a newly discovered token of given length
     * (length is source token's char length; we allocate length+1 for NUL).
     *
     * Thread-safe: locks words->mutex internally.
     */
    void reallocate_words(Words *words, int wordLength);

    /**
     * Append a new observation line with capacity for `observationLength` tokens
     * plus a terminating IDX_TERMINATOR (-1). The caller should fill the new row.
     *
     * Thread-safe: locks observations->mutex internally.
     */
    void reallocate_observations(Observations *observations, int observationLength);

    /* =========================
     * Persistence
     * ========================= */

    /**
     * Load database files (tokens/values/observations). Existing contents are
     * cleared first. Any NULL path uses the defaults from config.h.
     *
     * Returns 0 on success, non-zero on error.
     */
    int load_database(Words *words,
                      Observations *observations,
                      const char *tokens_path,        /* e.g., TOKENS_FILE */
                      const char *values_path,        /* e.g., VALUES_FILE (sparse CSV) */
                      const char *observations_path); /* e.g., OBSERVATIONS_FILE */

    /**
     * Save database to disk. Any NULL path uses the defaults from config.h.
     * On error, prints a message to stderr.
     *
     * values.csv is written sparsely: one line per non-zero entry:
     *   i,pi,k,pk,val
     */
    void write_database(const Words *words,
                        const Observations *observations,
                        const char *tokens_path,
                        const char *values_path,
                        const char *observations_path);

    /* =========================
     * Learning / Update
     * ========================= */

    /**
     * Tokenize `output` (captured stdout/stderr from a command), update vocabulary
     * and observations, and accumulate a learning value (lrnval).
     *
     * - command_integers is a -1 terminated array of token indices that formed
     *   the executed command (positional length ≤ CMDMAX).
     * - For each completed output line, a proximity-based redundancy check is used
     *   (implemented in database.c via learning.h). New lines increase lrnval by
     *   REWARD; near-duplicates may incur PENALTY or scaled reward.
     * - Updates the sparse association map with assoc_add(...) using lrnval.
     *
     * Returns the lrnval accumulated for this update.
     */
    int update_database(Words *words,
                        Observations *observations,
                        char *output,
                        int *command_integers);

    /* =========================
     * Seeding
     * ========================= */

    /**
     * Populate Words->token from executables found on PATH (or override).
     * Returns number of tokens added (>=0), or -1 on fatal error.
     */
    int seed_vocabulary_from_path(Words *words, const char *path_env_override);

    #ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMOEBA_DATABASE_H */
