// src/main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>

#include "config.h"
#include "model.h"
#include "database.h"
#include "trend.h"
#include "threads.h"
#include "exec.h"     // signal_handler, termination_requested

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--threads N] [--length N] [--scope P]\n"
            "  --threads N   Number of worker threads (1..%d) [default: %d]\n"
            "  --length  N   Command arg length (%d..%d) [default: %d]\n"
            "  --scope   P   Vocabulary sampling scope (percent %d..%d) [default: %d]\n",
            prog, MAX_THREADS, MAX_THREADS,
            CMDMIN, CMDMAX, 1,
            SRCHMIN, SRCHMAX, 50);
}

static void install_handlers(void) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;                // set SA_RESTART if you prefer auto-restarting syscalls
    if (sigaction(SIGINT,  &sa, NULL) == -1) perror("sigaction(SIGINT)");
    if (sigaction(SIGTERM, &sa, NULL) == -1) perror("sigaction(SIGTERM)");
    // Optional: ignore SIGPIPE so writes to closed pipes don't kill the process
    signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char **argv) {
    int num_threads = MAX_THREADS;
    int want_length = 1;   // start simple: executable only
    int want_scope  = 50;

    // --- CLI parsing
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--length") && i + 1 < argc) {
            want_length = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--scope") && i + 1 < argc) {
            want_scope = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    // Clamp inputs
    if (num_threads < 1) num_threads = 1;
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
    if (want_length < CMDMIN) want_length = CMDMIN;
    if (want_length > CMDMAX) want_length = CMDMAX;
    if (want_scope < SRCHMIN) want_scope = SRCHMIN;
    if (want_scope > SRCHMAX) want_scope = SRCHMAX;

    // --- Signals first
    install_handlers();

    // --- Core models
    Words words;
    Observations observations;
    CommandSettings settings;
    LearningTrendTracker tracker;

    init_words(&words);
    init_observations(&observations);

    // Load DB if present
    if (load_database(&words, &observations, NULL, NULL, NULL) != 0) {
        fprintf(stderr, "[warn] load_database failed; starting with empty DB.\n");
    }

    // Seed from PATH if still empty
    if (words.numWords == 0) {
        int seeded = seed_vocabulary_from_path(&words, NULL);
        printf("Seeded %d executable names from PATH.\n", seeded);
    }
    printf("Vocabulary size: %zu token(s).\n", words.numWords);

    // Settings
    settings.length = want_length;
    settings.scope  = want_scope;
    if (pthread_mutex_init(&settings.mutex, NULL) != 0) {
        fprintf(stderr, "Failed to init settings.mutex\n");
        cleanup_database(&words, &observations);
        return 1;
    }

    // Trend tracker
    init_trend_tracker(&tracker);

    // Concurrency gate
    if (init_thread_sem((unsigned int)num_threads) != 0) {
        fprintf(stderr, "Failed to initialize thread semaphore\n");
        destroy_trend_tracker(&tracker);
        pthread_mutex_destroy(&settings.mutex);
        cleanup_database(&words, &observations);
        return 1;
    }

    // --- Spawn workers (run until Ctrl-C)
    pthread_t  tids[MAX_THREADS];
    ThreadData payloads[MAX_THREADS];

    printf("Launching %d worker thread(s) (length=%d, scope=%d%%)\n",
           num_threads, want_length, want_scope);
    printf("Press Ctrl-C to stop.\n");

    for (int i = 0; i < num_threads; ++i) {
        payloads[i].words        = &words;
        payloads[i].observations = &observations;
        payloads[i].settings     = &settings;
        payloads[i].tracker      = &tracker;

        int rc = pthread_create(&tids[i], NULL, worker_thread, &payloads[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed on worker %d (rc=%d)\n", i, rc);
            num_threads = i; // only join the ones that started
            break;
        }
    }

    // --- Spawn tuner (periodically adjusts settings->length)
    pthread_t tuner_tid;
    TunerArgs tuner_args = {
        .settings    = &settings,
        .tracker     = &tracker,
        .interval_ms = 1500,    // tweak if you want faster/slower adjustments
    };
    if (pthread_create(&tuner_tid, NULL, tuner_thread, &tuner_args) != 0) {
        fprintf(stderr, "[warn] failed to start tuner thread; continuing without tuning\n");
        tuner_tid = 0;
    }

    // Wait for workers (they exit on SIGINT/SIGTERM)
    for (int i = 0; i < num_threads; ++i) {
        (void)pthread_join(tids[i], NULL);
    }

    // Stop & join tuner if it started
    if (tuner_tid) {
        (void)pthread_join(tuner_tid, NULL);
    }

    if (termination_requested) {
        printf("Received signal, shutting down…\n");
    }

    // Persist DB
    write_database(&words, &observations, TOKENS_FILE, VALUES_FILE, OBSERVATIONS_FILE);

    // Trend summary
    double ma = get_moving_average(&tracker);
    int trend = analyze_learning_trend(&tracker);
    const char *tstr = (trend > 0) ? "up" : (trend < 0) ? "down" : "flat";
    printf("Learning moving average: %.2f  (trend: %s)\n", ma, tstr);

    // Teardown
    destroy_thread_sem();
    destroy_trend_tracker(&tracker);
    pthread_mutex_destroy(&settings.mutex);
    cleanup_database(&words, &observations);

    printf("Shutdown complete.\n");
    return 0;
}
