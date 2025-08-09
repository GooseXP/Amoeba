#ifndef AMOEBA_COMMAND_H
#define AMOEBA_COMMAND_H

/*
 * command.h — command construction API (sparse-assoc aware)
 *
 * Builds a command as an array of token indices into Words->token,
 * honoring CommandSettings (length/scope). The result is a -1
 * terminated array of ints (capacity CMDMAX+1).
 *
 * Thread-safety: Implementation locks Words and CommandSettings
 * mutexes internally while reading shared state.
 */

#include "model.h"   /* Words, CommandSettings */
#include "config.h"  /* CMDMAX, IDX_TERMINATOR */

#ifdef __cplusplus
extern "C" {
    #endif

    /**
     * construct_command
     * -----------------
     * Fills out_cmd with up to settings->length token indices chosen from the
     * vocabulary (Words). Selection respects settings->scope (percentage
     * of vocabulary sampled) and clamps length to [CMDMIN..CMDMAX].
     *
     * on success:
     *   - out_cmd[0..(argc-1)] are valid indices into words->token
     *   - out_cmd[argc] == IDX_TERMINATOR (-1)
     *
     * Parameters:
     *   words     : vocabulary/association store (read-only access)
     *   settings  : length/scope parameters (read-only access)
     *   out_cmd   : caller-provided buffer of size CMDMAX+1
     *
     * Returns:
     *   argc (number of arguments written, 0..CMDMAX)
     */
    int construct_command(const Words *words,
                          const CommandSettings *settings,
                          int out_cmd[CMDMAX + 1]);

    #ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMOEBA_COMMAND_H */
