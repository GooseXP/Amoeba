// src/exec.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>

#include "config.h"
#include "exec.h"

/* ============ globals ============ */

volatile sig_atomic_t termination_requested = 0;

/* ============ helpers ============ */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static double now_monotonic_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Try to send a signal to the child's process group so any grandchildren die too. */
static void send_signal_tree(pid_t child_pid, int sig) {
    if (child_pid <= 0) return;
    /* negative pid targets the process group whose id is |child_pid| */
    (void)kill(-child_pid, sig);
}

/* ============ public API ============ */

void signal_handler(int signum) {
    (void)signum;
    termination_requested = 1;
}

/**
* check_child_status
* Return semantics:
*   0  -> child has exited (reaped)
*   1  -> child still running
*  -1  -> error (waitpid failure)
*/
int check_child_status(pid_t child_pid) {
    int status = 0;
    pid_t r = waitpid(child_pid, &status, WNOHANG);
    if (r == 0) {
        return 1; /* still running */
    } else if (r == child_pid) {
        return 0; /* finished */
    } else {
        /* r < 0 */
        if (errno == EINTR) return 1; /* treat as still running; caller will loop again */
        return -1;
    }
}

char *execute_command(char cmd[]) {
    if (!cmd) {
        errno = EINVAL;
        return NULL;
    }

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        /* fork failed */
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* ---- child ---- */
        /* Create a new process group so we can signal the whole tree from parent */
        (void)setpgid(0, 0);

        /* Redirect stdout & stderr to pipe write end */
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        /* Use /bin/sh -c to execute the command string */
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);

        /* If exec fails */
        _exit(127);
    }

    /* ---- parent ---- */
    close(pipefd[1]); /* we only read */
    set_nonblocking(pipefd[0]);

    /* dynamic buffer to store output */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        /* try to kill child if we can't buffer output */
        send_signal_tree(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return NULL;
    }

    double t_start = now_monotonic_s();
    int kill_stage = 0; /* 0: normal, 1: sent SIGTERM, 2+: sent SIGKILLs */
    int finished = 0;

    struct pollfd pfd;
    pfd.fd = pipefd[0];
    pfd.events = POLLIN | POLLERR | POLLHUP;

    char tmp[4096];

    for (;;) {
        /* Read anything available */
        int pr = poll(&pfd, 1, 100); /* 100ms tick */
        if (pr > 0 && (pfd.revents & (POLLIN | POLLERR | POLLHUP))) {
            for (;;) {
                ssize_t r = read(pipefd[0], tmp, sizeof(tmp));
                if (r > 0) {
                    if (len + (size_t)r + 1 > cap) {
                        size_t ncap = cap * 2;
                        while (ncap < len + (size_t)r + 1) ncap *= 2;
                        char *nbuf = (char*)realloc(buf, ncap);
                        if (!nbuf) {
                            /* OOM; bail out */
                            free(buf);
                            close(pipefd[0]);
                            send_signal_tree(pid, SIGKILL);
                            (void)waitpid(pid, NULL, 0);
                            return NULL;
                        }
                        buf = nbuf;
                        cap = ncap;
                    }
                    memcpy(buf + len, tmp, (size_t)r);
                    len += (size_t)r;
                    buf[len] = '\0';
                } else if (r == 0) {
                    /* EOF */
                    break;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    /* read error—ignore transient, break loop */
                    break;
                }
            }
        }

        /* Check child status */
        int cs = check_child_status(pid);
        if (cs == 0) {
            finished = 1;
        } else if (cs < 0) {
            /* waitpid failure—terminate */
            close(pipefd[0]);
            free(buf);
            return NULL;
        }

        double elapsed = now_monotonic_s() - t_start;
        int over_time = (elapsed >= (double)RUNTIME);

        if (!finished && (over_time || termination_requested)) {
            if (kill_stage == 0) {
                send_signal_tree(pid, SIGTERM);
                kill_stage = 1;
            } else if (kill_stage <= KILL_ATTEMPTS) {
                send_signal_tree(pid, SIGKILL);
                kill_stage++;
            } else {
                /* Give up */
                close(pipefd[0]);
                free(buf);
                return NULL;
            }
        }

        if (finished) {
            /* read until EOF and break */
            for (;;) {
                ssize_t r = read(pipefd[0], tmp, sizeof(tmp));
                if (r > 0) {
                    if (len + (size_t)r + 1 > cap) {
                        size_t ncap = cap * 2;
                        while (ncap < len + (size_t)r + 1) ncap *= 2;
                        char *nbuf = (char*)realloc(buf, ncap);
                        if (!nbuf) break; /* best-effort; drop remainder */
                        buf = nbuf;
                        cap = ncap;
                    }
                    memcpy(buf + len, tmp, (size_t)r);
                    len += (size_t)r;
                } else {
                    break;
                }
            }
            break;
        }
    }

    close(pipefd[0]);

    /* ensure NUL termination */
    if (len == 0) {
        /* return empty string instead of NULL on success */
        buf[0] = '\0';
    } else {
        buf[len] = '\0';
    }

    return buf;
}
