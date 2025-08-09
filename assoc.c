#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "assoc.h"

static size_t round_up_pow2(size_t x) {
    size_t p = 1; while (p < x) p <<= 1; return p ? p : 1;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

/* Combine the 4-tuple into a 64-bit hash */
static size_t hkey(int i, int pi, int k, int pk) {
    uint64_t x = 0x9e3779b97f4a7c15ULL;
    x ^= (uint32_t)i;   x = mix64(x);
    x ^= (uint32_t)pi;  x = mix64(x);
    x ^= (uint32_t)k;   x = mix64(x);
    x ^= (uint32_t)pk;  x = mix64(x);
    return (size_t)x;
}

static int keys_equal(const AssocEntry *e, int i, int pi, int k, int pk) {
    return e->i==i && e->pi==pi && e->k==k && e->pk==pk;
}

static int resize(Assoc *a, size_t newcap) {
    AssocEntry **nb = (AssocEntry**)calloc(newcap, sizeof(*nb));
    if (!nb) return -1;
    for (size_t b = 0; b < a->nbuckets; ++b) {
        AssocEntry *e = a->buckets[b];
        while (e) {
            AssocEntry *next = e->next;
            size_t idx = hkey(e->i, e->pi, e->k, e->pk) & (newcap - 1);
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    free(a->buckets);
    a->buckets = nb;
    a->nbuckets = newcap;
    return 0;
}

int assoc_init(Assoc *a, size_t nbuckets_hint) {
    if (!a) return -1;
    size_t cap = round_up_pow2(nbuckets_hint ? nbuckets_hint : 1024);
    a->buckets = (AssocEntry**)calloc(cap, sizeof(*a->buckets));
    if (!a->buckets) return -1;
    a->nbuckets = cap;
    a->nentries = 0;
    return 0;
}

void assoc_free(Assoc *a) {
    if (!a || !a->buckets) return;
    for (size_t b = 0; b < a->nbuckets; ++b) {
        AssocEntry *e = a->buckets[b];
        while (e) { AssocEntry *n = e->next; free(e); e = n; }
    }
    free(a->buckets);
    a->buckets = NULL; a->nbuckets = 0; a->nentries = 0;
}

int assoc_add(Assoc *a, int i, int pi, int k, int pk, int delta) {
    if (!a || !a->buckets || delta == 0) return 0;

    /* resize if load factor > 0.75 */
    if ((a->nentries + 1) * 4 > a->nbuckets * 3) {
        if (resize(a, a->nbuckets ? a->nbuckets * 2 : 1024) != 0) return -1;
    }
    size_t idx = hkey(i,pi,k,pk) & (a->nbuckets - 1);
    AssocEntry *e = a->buckets[idx], *prev = NULL;
    while (e) {
        if (keys_equal(e, i,pi,k,pk)) {
            e->val += delta;
            if (e->val == 0) {
                /* delete */
                if (prev) prev->next = e->next; else a->buckets[idx] = e->next;
                free(e); a->nentries--;
            }
            return 0;
        }
        prev = e; e = e->next;
    }
    /* create new entry if nonzero */
    AssocEntry *ne = (AssocEntry*)malloc(sizeof(*ne));
    if (!ne) return -1;
    ne->i=i; ne->pi=pi; ne->k=k; ne->pk=pk; ne->val=delta;
    ne->next = a->buckets[idx];
    a->buckets[idx] = ne;
    a->nentries++;
    return 0;
}

int assoc_get(const Assoc *a, int i, int pi, int k, int pk) {
    if (!a || !a->buckets) return 0;
    size_t idx = hkey(i,pi,k,pk) & (a->nbuckets - 1);
    AssocEntry *e = a->buckets[idx];
    while (e) {
        if (keys_equal(e, i,pi,k,pk)) return e->val;
        e = e->next;
    }
    return 0;
}

int assoc_iter_next(AssocIter *it, int *i, int *pi, int *k, int *pk, int *val) {
    if (!it || !it->a || !it->a->buckets) return 0;
    if (it->e) {
        it->e = it->e->next;
        if (it->e) goto have;
        it->bucket++;
    }
    while (it->bucket < it->a->nbuckets) {
        it->e = it->a->buckets[it->bucket];
        if (it->e) goto have;
        it->bucket++;
    }
    return 0;
have:
    if (i) *i = it->e->i;
    if (pi) *pi = it->e->pi;
    if (k) *k = it->e->k;
    if (pk) *pk = it->e->pk;
    if (val) *val = it->e->val;
    return 1;
}
