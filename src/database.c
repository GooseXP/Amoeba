// src/database.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

#include "config.h"
#include "model.h"
#include "assoc.h"
#include "learning.h"
#include "database.h"


/* ---------- local helpers ---------- */

static char *dupstr_local(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* linear lookup. caller must hold words->mutex if racing with writers */
static int find_token_index_unlocked(const Words *words, const char *tok) {
    if (!words || !tok) return -1;
    for (size_t i = 0; i < words->numWords; ++i) {
        const char *w = words->token[i];
        if (w && strcmp(w, tok) == 0) return (int)i;
    }
    return -1;
}

/* Tokenize a free-form line by whitespace into known token indices.
* Returns malloc'd int[] terminated by IDX_TERMINATOR, or NULL if none. */
static int *tokenize_to_indices(Words *words, const char *line) {
    if (!words || !line) return NULL;

    int count = 0;
    char *tmp = dupstr_local(line);
    if (!tmp) return NULL;

    /* count known tokens first */
    char *save = NULL;
    for (char *t = strtok_r(tmp, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
        pthread_mutex_lock(&words->mutex);
        int idx = find_token_index_unlocked(words, t);
        pthread_mutex_unlock(&words->mutex);
        if (idx >= 0) count++;
    }
    free(tmp);
    if (count == 0) return NULL;

    int *arr = (int *)malloc(((size_t)count + 1) * sizeof(int));
    if (!arr) return NULL;
    int pos = 0;
    char *tmp2 = dupstr_local(line);
    if (!tmp2) { free(arr); return NULL; }
    char *save2 = NULL;
    for (char *t = strtok_r(tmp2, " \t\r\n", &save2); t; t = strtok_r(NULL, " \t\r\n", &save2)) {
        pthread_mutex_lock(&words->mutex);
        int idx = find_token_index_unlocked(words, t);
        pthread_mutex_unlock(&words->mutex);
        if (idx >= 0) arr[pos++] = idx;
    }
    free(tmp2);
    arr[pos] = IDX_TERMINATOR;
    return arr;
}

/* ensure parent directory of a file path exists (mkdir -p style, best-effort) */
static int ensure_parent_dir(const char *filepath) {
    if (!filepath || !*filepath) return 0;
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", filepath);

    char *slash = strrchr(buf, '/');
    if (!slash) return 0; /* current directory */
    *slash = '\0';
    if (!*buf) return 0;

    if (mkdir(buf, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;

    /* progressively create parents */
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    (void)mkdir(buf, 0755);
    return 0;
}

/* ---------- public API (matches your database.h) ---------- */

void init_words(Words *w) {
    if (!w) return;
    w->token = NULL;
    w->numWords = 0;
    pthread_mutex_init(&w->mutex, NULL);
    (void)assoc_init(&w->assoc, 0);  /* 0 = default bucket hint */
}

void free_words(Words *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    for (size_t i = 0; i < w->numWords; ++i) free(w->token[i]);
    free(w->token);
    w->token = NULL;
    w->numWords = 0;
    pthread_mutex_unlock(&w->mutex);
 
    /* free assoc before destroying the mutex (no dependency either way here) */
    assoc_free(&w->assoc);
    pthread_mutex_destroy(&w->mutex);
}

/* NOTE: declared public in your header; caller must hold words->mutex.
* Semantics: if growth succeeds, we ALWAYS append a new slot and increment numWords.
* If the string malloc fails, we append a NULL slot; the CALLER MAY shrink back by
* doing `if (!words->token[words->numWords - 1]) words->numWords--;` */
void reallocate_words(Words *words, int wordLength) {
    if (!words || wordLength < 0) return;

    size_t newCount = words->numWords + 1;

    /* grow the token pointer array by 1 */
    char **grown = (char **)realloc(words->token, newCount * sizeof(*grown));
    if (!grown) return; /* keep old on failure */

    words->token = grown;
    char *slot = (char *)malloc((size_t)wordLength + 1);
    words->token[words->numWords] = slot; /* may be NULL */
    words->numWords = newCount;
}

void init_observations(Observations *o) {
    if (!o) return;
    o->entries = NULL;
    o->numObservations = 0;
    pthread_mutex_init(&o->mutex, NULL);
}

void free_observations(Observations *o) {
    if (!o) return;
    pthread_mutex_lock(&o->mutex);
    for (size_t i = 0; i < o->numObservations; ++i) free(o->entries[i]);
    free(o->entries);
    o->entries = NULL;
    o->numObservations = 0;
    pthread_mutex_unlock(&o->mutex);
    pthread_mutex_destroy(&o->mutex);
}

void cleanup_database(Words *words, Observations *obs) {
    if (obs) {
        if (obs->entries) {
            for (size_t i = 0; i < obs->numObservations; ++i) {
                free(obs->entries[i]);   /* each is an int* (tokenized line) */
            }
            free(obs->entries);
            obs->entries = NULL;
        }
        obs->numObservations = 0;
        pthread_mutex_destroy(&obs->mutex);
    }
    if (words) {
        if (words->token) {
            for (size_t i = 0; i < words->numWords; ++i) {
                free(words->token[i]);
            }
            free(words->token);
            words->token = NULL;
        }
        words->numWords = 0;
        assoc_free(&words->assoc);
        pthread_mutex_destroy(&words->mutex);
    }
}

/* ---------- persistence (paths provided explicitly) ---------- */

static int load_tokens(Words *w, const char *tokens_path) {
    if (!tokens_path) return -1;
    FILE *fp = fopen(tokens_path, "r");
    if (!fp) return (errno == ENOENT) ? 0 : -1;

    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t n = strlen(buf);
        while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
        if (!*buf) continue;

        pthread_mutex_lock(&w->mutex);
        if (find_token_index_unlocked(w, buf) < 0) {
            reallocate_words(w, (int)strlen(buf));
            if (w->token[w->numWords - 1]) {
                strcpy(w->token[w->numWords - 1], buf);
            } else {
                /* slot alloc failed -> remove the NULL slot */
                if (w->numWords > 0) w->numWords--;
            }
        }
        pthread_mutex_unlock(&w->mutex);
    }
    fclose(fp);
    return 0;
}

/* format on disk: i\tpi\tk\tpk\tvalue\n */
static int load_values(Words *w, const char *assoc_path) {
    if (!assoc_path) return -1;
    FILE *fp = fopen(assoc_path, "r");
    if (!fp) return (errno == ENOENT) ? 0 : -1;

    int i, pi, k, pk, v;
    while (fscanf(fp, "%d\t%d\t%d\t%d\t%d", &i, &pi, &k, &pk, &v) == 5) {
        assoc_add(&w->assoc, i, pi, k, pk, v);
        int c; while ((c = fgetc(fp)) != '\n' && c != EOF) {}
    }

    fclose(fp);
    return 0;
}

static int load_observations(Observations *o, const char *obs_path) {
    if (!obs_path) return -1;
    FILE *fp = fopen(obs_path, "r");
    if (!fp) return (errno == ENOENT) ? 0 : -1;

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1) {
        if (len <= 0) continue;

        /* count ints */
        int count = 0;
        char *tmp = dupstr_local(line);
        if (!tmp) continue;
        char *save = NULL;
        for (char *t = strtok_r(tmp, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
            count++;
        }
        free(tmp);
        if (count == 0) continue;

        int *arr = (int *)malloc(((size_t)count + 1) * sizeof(int));
        if (!arr) continue;

        int pos = 0;
        char *tmp2 = dupstr_local(line);
        if (!tmp2) { free(arr); continue; }
        char *save2 = NULL;
        for (char *t = strtok_r(tmp2, " \t\r\n", &save2); t; t = strtok_r(NULL, " \t\r\n", &save2)) {
            arr[pos++] = atoi(t);
        }
        free(tmp2);
        if (pos == 0 || arr[pos-1] != IDX_TERMINATOR) arr[pos++] = IDX_TERMINATOR;

        pthread_mutex_lock(&o->mutex);
        int **newv = (int **)realloc(o->entries, (o->numObservations + 1) * sizeof(*newv));
        if (newv) {
            o->entries = newv;
            o->entries[o->numObservations++] = arr;
            arr = NULL;
        }
        pthread_mutex_unlock(&o->mutex);
        free(arr);
    }
    free(line);
    fclose(fp);
    return 0;
}

/* database.h: int load_database(Words*, Observations*, const char*, const char*, const char*) */
int load_database(Words *w, Observations *o, const char *tokens_path, const char *assoc_path, const char *obs_path) {
    if (!w || !o) return -1;
    if (tokens_path && *tokens_path) if (load_tokens(w, tokens_path) != 0) return -1;
    if (assoc_path && *assoc_path)   if (load_values(w, assoc_path) != 0) return -1;
    if (obs_path && *obs_path)       if (load_observations(o, obs_path) != 0) return -1;
    return 0;
}

/* ---------- writing (robust, with mkdir -p and logs) ---------- */

static int write_tokens_file(const Words *w, const char *tokens_path) {
    if (!tokens_path || !*tokens_path) return 0;
    ensure_parent_dir(tokens_path);
    FILE *fp = fopen(tokens_path, "w");
    if (!fp) { perror("fopen tokens"); return -1; }

    size_t n = 0;
    for (size_t i = 0; i < w->numWords; ++i) {
        const char *t = w->token[i];
        if (t) { fputs(t, fp); fputc('\n', fp); ++n; }
    }
    fflush(fp);
    fclose(fp);

#if LOG_ACTIONS
    fprintf(stdout, "[persist] wrote %zu tokens -> %s\n", n, tokens_path);
#endif
    return 0;
}

static int write_assoc_file(const Words *w, const char *assoc_path) {
    if (!assoc_path || !*assoc_path) return 0;
    ensure_parent_dir(assoc_path);
    FILE *fp = fopen(assoc_path, "w");
    if (!fp) { perror("fopen values"); return -1; }

    AssocIter it;
    assoc_iter_init(&w->assoc, &it);
    int i, pi, k, pk, v;
    size_t rows = 0;
    while (assoc_iter_next(&it, &i, &pi, &k, &pk, &v)) {
        fprintf(fp, "%d\t%d\t%d\t%d\t%d\n", i, pi, k, pk, v);
        ++rows;
    }
    fflush(fp);
    fclose(fp);

#if LOG_ACTIONS
    fprintf(stdout, "[persist] wrote %zu assoc rows -> %s\n", rows, assoc_path);
#endif
    return 0;
}

static int write_obs_file(const Observations *o, const char *obs_path) {
    if (!obs_path || !*obs_path) return 0;
    ensure_parent_dir(obs_path);
    FILE *fp = fopen(obs_path, "w");
    if (!fp) { perror("fopen observations"); return -1; }

    size_t rows = 0;
    for (size_t li = 0; li < o->numObservations; ++li) {
        const int *row = o->entries[li];
        if (!row) continue;
        for (int j = 0; row[j] != IDX_TERMINATOR; ++j) {
            if (j) fputc(' ', fp);
            fprintf(fp, "%d", row[j]);
        }
        fprintf(fp, " %d\n", IDX_TERMINATOR);
        ++rows;
    }

    fflush(fp);
    fclose(fp);

#if LOG_ACTIONS
    fprintf(stdout, "[persist] wrote %zu observations -> %s\n", rows, obs_path);
#endif
    return 0;
}

/* database.h: void write_database(...) */
void write_database(const Words *w, const Observations *o, const char *tokens_path, const char *assoc_path, const char *obs_path) {
    if (!w || !o) return;
#if LOG_ACTIONS
    fprintf(stdout, "[persist] writing database…\n");
#endif
    (void)write_tokens_file(w, tokens_path);
    (void)write_assoc_file(w, assoc_path);
    (void)write_obs_file(o, obs_path);
#if LOG_ACTIONS
    fprintf(stdout, "[persist] done.\n");
#endif
}

/* ---------- PATH seeding ---------- */

int seed_vocabulary_from_path(Words *words, const char *path_env_override) {
    if (!words) return -1;

    const char *envp = path_env_override ? path_env_override : getenv("PATH");
    if (!envp || !*envp) {
        envp = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }

    char *paths = dupstr_local(envp);
    if (!paths) return -1;

    int total_added = 0;
    char *saveptr = NULL;

    for (char *dir = strtok_r(paths, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
#if LOG_SEEDING
        fprintf(stdout, "[seed] scanning %s ...\n", dir);
#endif
        DIR *dh = opendir(dir);
        if (!dh) {
#if LOG_SEEDING
            fprintf(stdout, "[seed]   (skip: cannot open)\n");
#endif
            continue;
        }

        time_t t0 = time(NULL);
        int processed = 0, added_this_dir = 0;

        struct dirent *ent;
        while ((ent = readdir(dh))) {
            if (ent->d_name[0] == '.') continue;

            if (DIR_SCAN_TIMEOUT_SEC > 0 && (time(NULL) - t0) >= DIR_SCAN_TIMEOUT_SEC) {
#if LOG_SEEDING
                fprintf(stdout, "[seed]   %s: timed out after %d s, moving on\n", dir, DIR_SCAN_TIMEOUT_SEC);
#endif
                break;
            }

            int is_reg = 0, is_lnk = 0;
#ifdef _DIRENT_HAVE_D_TYPE
            if (ent->d_type == DT_REG)      is_reg = 1;
            else if (ent->d_type == DT_LNK) is_lnk = 1;
#endif

            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

            struct stat st;
            if (!is_reg && !is_lnk) {
                if (lstat(full, &st) != 0) continue;
                is_reg = S_ISREG(st.st_mode);
                is_lnk = S_ISLNK(st.st_mode);
            } else if (is_reg) {
                if (stat(full, &st) != 0) continue; /* need mode bits */
            }

#if SKIP_SYMLINKS
            if (is_lnk) continue;
#endif
            if (!is_reg) continue;
            if ((st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) == 0) continue;

            pthread_mutex_lock(&words->mutex);
            int already = find_token_index_unlocked(words, ent->d_name);
            if (already < 0) {
                size_t len = strlen(ent->d_name);
                reallocate_words(words, (int)len);
                if (words->token[words->numWords - 1]) {
                    memcpy(words->token[words->numWords - 1], ent->d_name, len + 1);
                    added_this_dir++;
                    total_added++;
                } else {
                    /* remove NULL slot on failure */
                    if (words->numWords > 0) words->numWords--;
                }
            }
            pthread_mutex_unlock(&words->mutex);

            processed++;
#if LOG_SEEDING
            if (processed % SEED_LOG_EVERY == 0) {
                fprintf(stdout, "[seed]   %s: processed %d (+%d)\n", dir, processed, added_this_dir);
            }
#endif
            if (MAX_SEED_PER_DIR > 0 && added_this_dir >= MAX_SEED_PER_DIR) {
#if LOG_SEEDING
                fprintf(stdout, "[seed]   %s: hit cap %d, moving on\n", dir, MAX_SEED_PER_DIR);
#endif
                break;
            }
        }

        closedir(dh);
#if LOG_SEEDING
        fprintf(stdout, "[seed]   %s: added %d\n", dir, added_this_dir);
#endif
    }

    free(paths);
#if LOG_SEEDING
    fprintf(stdout, "[seed] total added: %d\n", total_added);
#endif
    return total_added;
}

/* ---------- learning update ---------- */

int update_database(Words *words, Observations *obs, char *output, int *cmd_indices) {
    if (!words || !obs || !output || !cmd_indices) return 0;

    /* Tokenize the command output into known token indices (may be NULL). */
    int *line = tokenize_to_indices(words, output);

    int redundant = 0;
    int reward = 1; /* default: positive reward */

    if (line) {
        /* compute effective length of the tokenized output */
        int nline = 0;
        while (line[nline] != IDX_TERMINATOR && nline < CMDMAX * 4) nline++;

        /* Check redundancy against all observations; learning.c can internally
         * *           prefer more recent ones if desired. */
        pthread_mutex_lock(&obs->mutex);
        int best_index = -1;
        float best_score = 0.0f;
        redundant = is_redundant_line_proximity(
            line, nline,
            obs->entries, obs->numObservations,
            REDUNDANCY_THRESHOLD,
            &best_index, &best_score);

#if VERBOSE_LOG
        if (redundant) {
            fprintf(stdout, "[learn] redundant vs obs[%d], score=%.1f%%\n", best_index, best_score);
        }
#endif

        /* Append observation if desired */
        if (!redundant || STORE_REDUNDANT) {
            int **newv = (int **)realloc(obs->entries, (obs->numObservations + 1) * sizeof(*newv));
            if (newv) {
                obs->entries = newv;
                obs->entries[obs->numObservations++] = line;
                line = NULL; /* ownership transferred */
            }
        }
        pthread_mutex_unlock(&obs->mutex);

        if (line) free(line); /* only free if we did NOT append */
        reward = redundant ? -PENALTY : REWARD;   // from config.h
    }

    /* Update sparse association map with pairwise co-occurrences */
    int vals[CMDMAX];
    int pos[CMDMAX];
    int argc = 0;
    for (int i = 0; i < CMDMAX && cmd_indices[i] != IDX_TERMINATOR; ++i) {
        vals[argc] = cmd_indices[i];
        pos[argc]  = i;
        argc++;
    }

    if (argc > 0) {
        pthread_mutex_lock(&words->mutex);
        for (int a = 0; a < argc; ++a) {
            for (int b = 0; b < argc; ++b) {
                if (a == b) continue;
                assoc_add(&words->assoc, vals[a], pos[a], vals[b], pos[b], reward);
            }
        }
        pthread_mutex_unlock(&words->mutex);
    }

    return reward;
}
