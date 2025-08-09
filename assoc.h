#ifndef AMOEBA_ASSOC_H
#define AMOEBA_ASSOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
    #endif

    typedef struct AssocEntry {
        int i, pi, k, pk;
        int val;
        struct AssocEntry *next;
    } AssocEntry;

    typedef struct {
        AssocEntry **buckets;  /* array of bucket heads */
        size_t       nbuckets; /* buckets length (power of two) */
        size_t       nentries; /* number of live entries with nonzero val */
    } Assoc;

    /* Initialize/teardown */
    int  assoc_init(Assoc *a, size_t nbuckets_hint);  /* hint can be 0 -> default */
    void assoc_free(Assoc *a);

    /* Add delta to a key. Creates entry if missing. Deletes entry if val becomes 0. */
    int  assoc_add(Assoc *a, int i, int pi, int k, int pk, int delta);

    /* Get current value for a key (0 if absent). */
    int  assoc_get(const Assoc *a, int i, int pi, int k, int pk);

    /* Iterator: walk all entries (order undefined). */
    typedef struct {
        const Assoc *a;
        size_t bucket;
        AssocEntry *e;
    } AssocIter;

    static inline void assoc_iter_init(const Assoc *a, AssocIter *it) {
        it->a = a; it->bucket = 0; it->e = NULL;
    }
    int assoc_iter_next(AssocIter *it, int *i, int *pi, int *k, int *pk, int *val);

    #ifdef __cplusplus
}
#endif

#endif /* AMOEBA_ASSOC_H */
