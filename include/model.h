#ifndef AMOEBA_MODEL_H
#define AMOEBA_MODEL_H

/*
 * model.h — core data types (no function prototypes here)
 *
 * Shared structs used across modules:
 *   - LearningTrendTracker: moving average of learning value (lrnval)
 *   - Words:        vocabulary + sparse association map (Assoc)
 *   - Observations: learned output lines as token indices
 *   - CommandSettings: command-generation parameters
 *   - ThreadData:   bundle passed to worker threads
 */

#include <stddef.h>     // size_t
#include <pthread.h>    // pthread_mutex_t
#include "config.h"     // limits like CMDMAX
#include "assoc.h"      // sparse (i,pi,k,pk) -> int map

#ifdef __cplusplus
extern "C" {
    #endif

    /* =========================
     * Learning trend tracker
     * ========================= */
    typedef struct {
        int              window_size;     /* number of recent lrnval entries to consider */
        int             *lrnvals;         /* circular buffer of recent lrnval values (length = window_size) */
        int              index;           /* current write index in circular buffer */
        int              count;           /* how many entries have been written (<= window_size) */
        double           moving_average;  /* cached moving average */
        pthread_mutex_t  mutex;           /* protects the structure */
    } LearningTrendTracker;

    /* =========================
     * Words database (sparse)
     * =========================
     * token: array of C-strings; token[i] is the ith known word
     * assoc: sparse association map for (i,pi,k,pk) -> value
     */
    typedef struct {
        char           **token;     /* length = numWords; each token[i] is malloc’d string */
        size_t          numWords;   /* current vocabulary size */
        Assoc           assoc;      /* sparse association storage */
        pthread_mutex_t mutex;      /* protects token/numWords/assoc */
    } Words;

    /* =========================
     * Observations store
     * =========================
     * entries: array of lines; each entries[line] is an int* of token indices
     *          terminated by IDX_TERMINATOR (-1).
     */
    typedef struct {
        int            **entries;          /* length = numObservations */
        size_t           numObservations;  /* number of stored lines */
        pthread_mutex_t  mutex;            /* protects entries/numObservations */
    } Observations;

    /* =========================
     * Command generation settings
     * =========================
     * length: desired number of args in constructed command (bounded by [CMDMIN..CMDMAX])
     * scope:  percent of vocabulary to sample when building commands ([SRCHMIN..SRCHMAX])
     */
    typedef struct {
        int              length;
        int              scope;
        pthread_mutex_t  mutex;
    } CommandSettings;

    /* =========================
     * Thread payload
     * =========================
     * Bundle of pointers provided to worker threads.
     */
    typedef struct {
        Words                *words;
        Observations         *observations;
        CommandSettings      *settings;
        LearningTrendTracker *tracker;
    } ThreadData;

    #ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMOEBA_MODEL_H */
