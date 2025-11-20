#define _POSIX_C_SOURCE 200809L
#include "nm_dir.h"
#include "nm_persist.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Simple hash map for filename->ssId using chaining
typedef struct node { char *k; int v; struct node *next; } node_t;

#define NBKT 257
static node_t *g_buckets[NBKT];

// Simple LRU cache for last 64 lookups
typedef struct lru_node { char *k; int v; struct lru_node *prev, *next; } lru_node_t;
static lru_node_t *g_lru_head = NULL, *g_lru_tail = NULL; // MRU at head
static size_t g_lru_size = 0;
static const size_t LRU_MAX = 64;

static unsigned hash_str(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; ++s) { h ^= (unsigned char)(*s); h *= 16777619u; }
    return h;
}

static void map_put(const char *k, int v) {
    unsigned h = hash_str(k) % NBKT;
    node_t *n = g_buckets[h];
    for (; n; n = n->next) {
        if (strcmp(n->k, k) == 0) { n->v = v; return; }
    }
    n = (node_t *)malloc(sizeof(node_t));
    n->k = strdup(k); n->v = v; n->next = g_buckets[h]; g_buckets[h] = n;
}

static int map_get(const char *k, int *out_v) {
    unsigned h = hash_str(k) % NBKT;
    node_t *n = g_buckets[h];
    for (; n; n = n->next) {
        if (strcmp(n->k, k) == 0) { if (out_v) *out_v = n->v; return 0; }
    }
    return -1;
}

static void lru_promote(lru_node_t *n) {
    if (!n || n == g_lru_head) return;
    // detach
    if (n->prev) n->prev->next = n->next;
    if (n->next) n->next->prev = n->prev; else g_lru_tail = n->prev;
    // insert at head
    n->prev = NULL; n->next = g_lru_head; if (g_lru_head) g_lru_head->prev = n; g_lru_head = n; if (!g_lru_tail) g_lru_tail = n;
}

static lru_node_t *lru_find(const char *k) {
    for (lru_node_t *n = g_lru_head; n; n = n->next) {
        if (strcmp(n->k, k) == 0) return n;
    }
    return NULL;
}

static void lru_insert(const char *k, int v) {
    lru_node_t *n = (lru_node_t *)malloc(sizeof(lru_node_t));
    n->k = strdup(k); n->v = v; n->prev = NULL; n->next = g_lru_head; if (g_lru_head) g_lru_head->prev = n; g_lru_head = n; if (!g_lru_tail) g_lru_tail = n;
    if (++g_lru_size > LRU_MAX) {
        // evict tail
        lru_node_t *t = g_lru_tail; if (t) {
            g_lru_tail = t->prev; if (g_lru_tail) g_lru_tail->next = NULL; else g_lru_head = NULL;
            free(t->k); free(t); g_lru_size--;
        }
    }
}

void nm_dir_init(void) {
    // Seed from persisted state
    char files[256][128]; int ss[256];
    size_t n = nm_state_get_dir(files, ss, 256);
    for (size_t i = 0; i < n; ++i) map_put(files[i], ss[i]);
}

int nm_dir_lookup(const char *file, int *out_ss_id) {
    // LRU first
    lru_node_t *n = lru_find(file);
    if (n) { if (out_ss_id) *out_ss_id = n->v; lru_promote(n); return 0; }
    // Map
    int v=0; if (map_get(file, &v) == 0) { lru_insert(file, v); if (out_ss_id) *out_ss_id = v; return 0; }
    return -1;
}

int nm_dir_set(const char *file, int ss_id) {
    int old=0; int existed = (map_get(file, &old) == 0);
    if (!existed || old != ss_id) {
        map_put(file, ss_id);
        nm_state_set_dir(file, ss_id);
        // update LRU
        lru_node_t *n = lru_find(file);
        if (n) { n->v = ss_id; lru_promote(n); }
        else lru_insert(file, ss_id);
        return 1;
    }
    return 0;
}

size_t nm_dir_build_view_json(char *dst, size_t dst_sz, int include_ss) {
    size_t written = 0;
    written += snprintf(dst + written, dst_sz - written, "{\"status\":\"OK\",\"files\":[");
    // Iterate through persistence snapshot to have stable listing
    char files[512][128]; int ss[512]; size_t n = nm_state_get_dir(files, ss, 512);
    for (size_t i = 0; i < n; ++i) {
        if (i) written += snprintf(dst + written, dst_sz - written, ",");
        if (include_ss) written += snprintf(dst + written, dst_sz - written, "{\"name\":\"%s\",\"ssId\":%d}", files[i], ss[i]);
        else written += snprintf(dst + written, dst_sz - written, "\"%s\"", files[i]);
        if (written >= dst_sz) break;
    }
    if (written < dst_sz) written += snprintf(dst + written, dst_sz - written, "]}");
    return written;
}

int nm_dir_del(const char *file) {
    // remove from map buckets
    unsigned h = hash_str(file) % NBKT;
    node_t *prev = NULL, *n = g_buckets[h];
    while (n) {
        if (strcmp(n->k, file) == 0) {
            if (prev) prev->next = n->next; else g_buckets[h] = n->next;
            free(n->k); free(n);
            break;
        }
        prev = n; n = n->next;
    }
    // remove from LRU
    lru_node_t *ln = g_lru_head;
    while (ln) {
        if (strcmp(ln->k, file) == 0) {
            if (ln->prev) ln->prev->next = ln->next; else g_lru_head = ln->next;
            if (ln->next) ln->next->prev = ln->prev; else g_lru_tail = ln->prev;
            free(ln->k); free(ln); g_lru_size--; break;
        }
        ln = ln->next;
    }
    // persistence
    return nm_state_del_dir(file);
}

int nm_dir_rename(const char *old_file, const char *new_file) {
    if (!old_file || !new_file || !*old_file || !*new_file) return 0;
    // Check existing mapping value
    int ssid = 0; if (map_get(old_file, &ssid) != 0) return 0;
    // Ensure destination not present
    int dummy = 0; if (map_get(new_file, &dummy) == 0) return 0;
    // Update persistence first
    if (!nm_state_rename_dir(old_file, new_file)) return 0;
    
    // Remove old from map (without touching persistence)
    unsigned h = hash_str(old_file) % NBKT;
    node_t *prev = NULL, *n = g_buckets[h];
    while (n) {
        if (strcmp(n->k, old_file) == 0) {
            if (prev) prev->next = n->next; else g_buckets[h] = n->next;
            free(n->k); free(n);
            break;
        }
        prev = n; n = n->next;
    }
    
    // Remove old from LRU (without touching persistence)
    lru_node_t *ln = g_lru_head;
    while (ln) {
        if (strcmp(ln->k, old_file) == 0) {
            if (ln->prev) ln->prev->next = ln->next; else g_lru_head = ln->next;
            if (ln->next) ln->next->prev = ln->prev; else g_lru_tail = ln->prev;
            free(ln->k); free(ln); g_lru_size--; break;
        }
        ln = ln->next;
    }
    
    // Add new mapping to map and LRU
    map_put(new_file, ssid);
    lru_insert(new_file, ssid);
    return 1;
}
