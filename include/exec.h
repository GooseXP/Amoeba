#ifndef AMOEBA_EXEC_H
#define AMOEBA_EXEC_H

/*
 * exec.h — process execution & signal handling
 *
 * This module runs a shell command, captures combined stdout/stderr,
 * and enforces a runtime limit. It also exposes a simple SIGINT/SIGTERM
 * handler that flips a global flag other modules can poll.
 *
 * Conventions:
 *  - execute_command(cmd) returns a heap-allocated NUL-terminated buffer
 *    with the captured output, or NULL on error. Caller must free().
 *  - check_child_status(pid) is used internally by execute_command, but
 *    is exposed for reuse if needed. Returns 0 on success; non-zero on
 *    timeout/kill/error.
 *  - termination_requested is set to non-zero by signal_handler() when
 *    SIGINT/SIGTERM is received.
 */

#include <signal.h>    /* sig_atomic_t */
#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
    #endif

    /* Global flag set by signal_handler on SIGINT/SIGTERM. */
    extern volatile sig_atomic_t termination_requested;

    /**
     * signal_handler
     * --------------
     * Minimal handler: sets termination_requested when SIGINT/SIGTERM arrives.
     * Install with:
     *   struct sigaction sa = {0};
     *   sa.sa_handler = signal_handler;
     *   sigaction(SIGINT, &sa, NULL);
     *   sigaction(SIGTERM, &sa, NULL);
     */
    void signal_handler(int signum);

    /**
     * execute_command
     * ---------------
     * Runs the given command string in a child process, captures combined
     * stdout/stderr via a pipe, and enforces a time limit (see config.h:RUNTIME).
     *
     * Parameters:
     *   cmd : NUL-terminated shell command (e.g., "ls -la")
     *
     * Returns:
     *   Heap-allocated buffer containing the output (may be empty but not NULL-terminated? -> It IS NUL-terminated).
     *   NULL on error or timeout/kill. Caller must free() on success.
     */
    char *execute_command(char cmd[]);

    /**
     * check_child_status
     * ------------------
     * Helper that waits non-blockingly for the child to finish, enforcing
     * the runtime budget and escalating signals (e.g., SIGTERM → SIGKILL).
     *
     * Parameters:
     *   child_pid : PID returned by fork()
     *
     * Returns:
     *   0 if the child exited/was signaled normally within the budget,
     *   non-zero on waitpid/kill errors or if the timeout path failed.
     */
    int check_child_status(pid_t child_pid);

    #ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMOEBA_EXEC_H */
