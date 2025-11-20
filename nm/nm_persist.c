#define _POSIX_C_SOURCE 200809L
#include "nm_persist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Hash table implementation for O(1) lookups
#define HASH_BUCKETS 256

static unsigned hash_djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % HASH_BUCKETS;
}

struct acl_user { char *user; int perm; };

// Hash map structures for O(1) lookups
typedef struct user_hash_node {
    char *user;
    int is_active;
    struct user_hash_node *next;
} user_hash_node_t;

typedef struct acl_hash_node {
    char *file;
    size_t acl_index; // index into g_state.acls array
    struct acl_hash_node *next;
} acl_hash_node_t;

typedef struct folder_hash_node {
    char *path;
    size_t folder_index; // index into g_state.folders array
    struct folder_hash_node *next;
} folder_hash_node_t;

typedef struct req_hash_node {
    char *file;
    size_t req_index; // index into g_state.requests array
    struct req_hash_node *next;
} req_hash_node_t;

typedef struct trash_hash_node {
    char *file;
    size_t trash_index; // index into g_state.trash array
    struct trash_hash_node *next;
} trash_hash_node_t;

typedef struct {
    char **users;
    size_t n_users;
    size_t cap_users;
    // Hash maps for O(1) lookups
    user_hash_node_t *user_map[HASH_BUCKETS];
    acl_hash_node_t *acl_map[HASH_BUCKETS];
    folder_hash_node_t *folder_map[HASH_BUCKETS];
    req_hash_node_t *req_map[HASH_BUCKETS];
    trash_hash_node_t *trash_map[HASH_BUCKETS];
    // Active users (logged-in)
    char **active_users;
    size_t n_active;
    size_t cap_active;
    struct dir_entry { char *file; int ss_id; int *replicas; size_t n_repl; size_t cap_repl; char *last_modified_user; int last_modified_time; char *last_accessed_user; int last_accessed_time; } *dir;
    size_t n_dir;
    size_t cap_dir;
    struct acl_entry { char *file; char *owner; struct acl_user *grants; size_t n_grants; size_t cap_grants; } *acls;
    size_t n_acls;
    size_t cap_acls;
    // Folders list (logical prefixes)
    char **folders;
    size_t n_folders;
    size_t cap_folders;
    // Access requests per file (with mode per user: 'R' or 'W')
    struct req_entry { char *file; char **users; char *modes; size_t n_users; size_t cap_users; } *requests;
    size_t n_requests;
    size_t cap_requests;
    // Trash entries (soft-deleted files tracked at NM)
    struct trash_entry { char *file; char *trashed; int ssid; char *owner; int when; } *trash;
    size_t n_trash;
    size_t cap_trash;
} nm_state_t;

static nm_state_t g_state;

static void safe_copy(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void ensure_user_cap(size_t need) {
    if (g_state.cap_users >= need) return;
    size_t newcap = g_state.cap_users ? g_state.cap_users * 2 : 8;
    while (newcap < need) newcap *= 2;
    char **p = (char **)realloc(g_state.users, newcap * sizeof(char *));
    if (!p) return; // OOM; silently ignore growth
    g_state.users = p;
    g_state.cap_users = newcap;
}

static void ensure_active_cap(size_t need) {
    if (g_state.cap_active >= need) return;
    size_t newcap = g_state.cap_active ? g_state.cap_active * 2 : 8;
    while (newcap < need) newcap *= 2;
    char **p = (char **)realloc(g_state.active_users, newcap * sizeof(char *));
    if (!p) return; // OOM ignore
    g_state.active_users = p;
    g_state.cap_active = newcap;
}

void nm_state_init(void) {
    memset(&g_state, 0, sizeof(g_state));
    // Initialize hash maps
    for (int i = 0; i < HASH_BUCKETS; i++) {
        g_state.user_map[i] = NULL;
        g_state.acl_map[i] = NULL;
        g_state.folder_map[i] = NULL;
        g_state.req_map[i] = NULL;
        g_state.trash_map[i] = NULL;
    }
}

// User hash map helpers
static void user_map_insert(const char *user, int is_active) {
    unsigned h = hash_djb2(user);
    user_hash_node_t *node = (user_hash_node_t *)malloc(sizeof(user_hash_node_t));
    node->user = strdup(user);
    node->is_active = is_active;
    node->next = g_state.user_map[h];
    g_state.user_map[h] = node;
}

static user_hash_node_t *user_map_find(const char *user) {
    unsigned h = hash_djb2(user);
    user_hash_node_t *node = g_state.user_map[h];
    while (node) {
        if (strcmp(node->user, user) == 0) return node;
        node = node->next;
    }
    return NULL;
}

// ACL hash map helpers
static void acl_map_insert(const char *file, size_t index) {
    unsigned h = hash_djb2(file);
    acl_hash_node_t *node = (acl_hash_node_t *)malloc(sizeof(acl_hash_node_t));
    node->file = strdup(file);
    node->acl_index = index;
    node->next = g_state.acl_map[h];
    g_state.acl_map[h] = node;
}

static size_t acl_map_find(const char *file, int *found) {
    unsigned h = hash_djb2(file);
    acl_hash_node_t *node = g_state.acl_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            *found = 1;
            return node->acl_index;
        }
        node = node->next;
    }
    *found = 0;
    return 0;
}

static void acl_map_remove(const char *file) {
    unsigned h = hash_djb2(file);
    acl_hash_node_t *prev = NULL, *node = g_state.acl_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            if (prev) prev->next = node->next;
            else g_state.acl_map[h] = node->next;
            free(node->file);
            free(node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

static void acl_map_update_index(const char *file, size_t new_index) {
    unsigned h = hash_djb2(file);
    acl_hash_node_t *node = g_state.acl_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            node->acl_index = new_index;
            return;
        }
        node = node->next;
    }
}

// Folder hash map helpers
static void folder_map_insert(const char *path, size_t index) {
    unsigned h = hash_djb2(path);
    folder_hash_node_t *node = (folder_hash_node_t *)malloc(sizeof(folder_hash_node_t));
    node->path = strdup(path);
    node->folder_index = index;
    node->next = g_state.folder_map[h];
    g_state.folder_map[h] = node;
}

static int folder_map_exists(const char *path) {
    unsigned h = hash_djb2(path);
    folder_hash_node_t *node = g_state.folder_map[h];
    while (node) {
        if (strcmp(node->path, path) == 0) return 1;
        node = node->next;
    }
    return 0;
}

static void folder_map_remove(const char *path) {
    unsigned h = hash_djb2(path);
    folder_hash_node_t *prev = NULL, *node = g_state.folder_map[h];
    while (node) {
        if (strcmp(node->path, path) == 0) {
            if (prev) prev->next = node->next;
            else g_state.folder_map[h] = node->next;
            free(node->path);
            free(node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

static void folder_map_update_index(const char *path, size_t new_index) {
    unsigned h = hash_djb2(path);
    folder_hash_node_t *node = g_state.folder_map[h];
    while (node) {
        if (strcmp(node->path, path) == 0) {
            node->folder_index = new_index;
            return;
        }
        node = node->next;
    }
}

// Request hash map helpers
static void req_map_insert(const char *file, size_t index) {
    unsigned h = hash_djb2(file);
    req_hash_node_t *node = (req_hash_node_t *)malloc(sizeof(req_hash_node_t));
    node->file = strdup(file);
    node->req_index = index;
    node->next = g_state.req_map[h];
    g_state.req_map[h] = node;
}

static size_t req_map_find(const char *file, int *found) {
    unsigned h = hash_djb2(file);
    req_hash_node_t *node = g_state.req_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            *found = 1;
            return node->req_index;
        }
        node = node->next;
    }
    *found = 0;
    return 0;
}

static void req_map_remove(const char *file) {
    unsigned h = hash_djb2(file);
    req_hash_node_t *prev = NULL, *node = g_state.req_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            if (prev) prev->next = node->next;
            else g_state.req_map[h] = node->next;
            free(node->file);
            free(node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

static void req_map_update_index(const char *file, size_t new_index) {
    unsigned h = hash_djb2(file);
    req_hash_node_t *node = g_state.req_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            node->req_index = new_index;
            return;
        }
        node = node->next;
    }
}

// Trash hash map helpers
static void trash_map_insert(const char *file, size_t index) {
    unsigned h = hash_djb2(file);
    trash_hash_node_t *node = (trash_hash_node_t *)malloc(sizeof(trash_hash_node_t));
    node->file = strdup(file);
    node->trash_index = index;
    node->next = g_state.trash_map[h];
    g_state.trash_map[h] = node;
}

static size_t trash_map_find(const char *file, int *found) {
    unsigned h = hash_djb2(file);
    trash_hash_node_t *node = g_state.trash_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            *found = 1;
            return node->trash_index;
        }
        node = node->next;
    }
    *found = 0;
    return 0;
}

static void trash_map_remove(const char *file) {
    unsigned h = hash_djb2(file);
    trash_hash_node_t *prev = NULL, *node = g_state.trash_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            if (prev) prev->next = node->next;
            else g_state.trash_map[h] = node->next;
            free(node->file);
            free(node);
            return;
        }
        prev = node;
        node = node->next;
    }
}

static void trash_map_update_index(const char *file, size_t new_index) {
    unsigned h = hash_djb2(file);
    trash_hash_node_t *node = g_state.trash_map[h];
    while (node) {
        if (strcmp(node->file, file) == 0) {
            node->trash_index = new_index;
            return;
        }
        node = node->next;
    }
}


int nm_state_add_user(const char *user) {
    if (!user || !*user) return 0;
    // Check hash map first (O(1))
    if (user_map_find(user)) return 0;
    
    // Add to array
    ensure_user_cap(g_state.n_users + 1);
    if (g_state.cap_users < g_state.n_users + 1) return 0; // failed to grow
    g_state.users[g_state.n_users] = strdup(user);
    if (!g_state.users[g_state.n_users]) return 0;
    g_state.n_users++;
    
    // Add to hash map
    user_map_insert(user, 0);
    return 1;
}

size_t nm_state_get_users(char users[][128], size_t max_users) {
    size_t c = 0;
    for (size_t i = 0; i < g_state.n_users && c < max_users; ++i) {
        snprintf(users[c], 128, "%s", g_state.users[i]);
        c++;
    }
    return c;
}

size_t nm_state_get_active_users(char users[][128], size_t max_users) {
    size_t c = 0;
    for (size_t i = 0; i < g_state.n_active && c < max_users; ++i) {
        snprintf(users[c], 128, "%s", g_state.active_users[i]);
        c++;
    }
    return c;
}

int nm_state_user_is_active(const char *user) {
    if (!user || !*user) return 0;
    // O(1) hash map lookup
    user_hash_node_t *node = user_map_find(user);
    return node ? node->is_active : 0;
}

int nm_state_set_user_active(const char *user, int active) {
    if (!user || !*user) return 0;
    // Ensure it's a known user when activating
    if (active) nm_state_add_user(user);
    
    // Update hash map (O(1))
    user_hash_node_t *node = user_map_find(user);
    if (!node) {
        if (!active) return 0; // trying to deactivate non-existent user
        // Add new user node
        user_map_insert(user, 1);
        node = user_map_find(user);
    }
    
    if (node->is_active == active) return 0; // no change
    node->is_active = active;
    
    // Look for existing in active list
    for (size_t i = 0; i < g_state.n_active; ++i) {
        if (strcmp(g_state.active_users[i], user) == 0) {
            if (active) return 0; // already active (shouldn't happen with hash map check)
            // deactivate: remove by swap-with-last
            free(g_state.active_users[i]);
            if (i + 1 < g_state.n_active) g_state.active_users[i] = g_state.active_users[g_state.n_active - 1];
            g_state.n_active--;
            return 1;
        }
    }
    if (!active) return 0; // already inactive
    // add to active
    ensure_active_cap(g_state.n_active + 1);
    if (g_state.cap_active < g_state.n_active + 1) return 0;
    g_state.active_users[g_state.n_active] = strdup(user);
    if (!g_state.active_users[g_state.n_active]) return 0;
    g_state.n_active++;
    return 1;
}

static int write_atomic(const char *path, const char *data, size_t len) {
    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmppath, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    if (n != len) { fclose(f); unlink(tmppath); return -1; }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    if (rename(tmppath, path) != 0) { unlink(tmppath); return -1; }
    return 0;
}

int nm_state_save(const char *path) {
    // Compose JSON: users, directory, acls, replicas, requests, folders, trash
    size_t bufcap = 16384 + (g_state.n_users + g_state.n_active) * 64 + g_state.n_dir * 160 + g_state.n_acls * 320 + g_state.n_folders * 64 + g_state.n_trash * 256;
    char *buf = (char *)malloc(bufcap);
    if (!buf) return -1;
    buf[0] = '\0';
    strcat(buf, "{\n  \"users\":[");
    for (size_t i = 0; i < g_state.n_users; ++i) {
        if (i) strcat(buf, ",");
        strcat(buf, "\"");
        // escape quotes/backslashes minimally (usernames are simple in spec)
        const char *s = g_state.users[i];
        for (; *s; ++s) {
            if (*s == '"' || *s == '\\') {
                strncat(buf, "\\", bufcap - strlen(buf) - 1);
            }
            char ch[2] = {*s, 0};
            strncat(buf, ch, bufcap - strlen(buf) - 1);
        }
        strcat(buf, "\"");
    }
    strcat(buf, "],\n  \"active\":[");
    for (size_t i = 0; i < g_state.n_active; ++i) {
        if (i) strcat(buf, ",");
        strcat(buf, "\"");
        const char *s = g_state.active_users[i];
        for (; *s; ++s) { if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1); }
        strcat(buf, "\"");
    }
    strcat(buf, "],\n  \"directory\":{");
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (i) strcat(buf, ",");
        strcat(buf, "\"");
        const char *s = g_state.dir[i].file;
        for (; *s; ++s) {
            if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1);
            char ch[2] = {*s, 0};
            strncat(buf, ch, bufcap - strlen(buf) - 1);
        }
        strcat(buf, "\":{\"ss_id\":");
        char num[32]; snprintf(num, sizeof(num), "%d", g_state.dir[i].ss_id);
        strncat(buf, num, bufcap - strlen(buf) - 1);
        // Add metadata fields
        strcat(buf, ",\"last_modified_user\":");
        if (g_state.dir[i].last_modified_user && *g_state.dir[i].last_modified_user) {
            strcat(buf, "\"");
            s = g_state.dir[i].last_modified_user;
            for (; *s; ++s) {
                if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1);
                char ch[2] = {*s, 0};
                strncat(buf, ch, bufcap - strlen(buf) - 1);
            }
            strcat(buf, "\"");
        } else {
            strcat(buf, "null");
        }
        strcat(buf, ",\"last_modified_time\":");
        snprintf(num, sizeof(num), "%d", g_state.dir[i].last_modified_time);
        strncat(buf, num, bufcap - strlen(buf) - 1);
        strcat(buf, ",\"last_accessed_user\":");
        if (g_state.dir[i].last_accessed_user && *g_state.dir[i].last_accessed_user) {
            strcat(buf, "\"");
            s = g_state.dir[i].last_accessed_user;
            for (; *s; ++s) {
                if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1);
                char ch[2] = {*s, 0};
                strncat(buf, ch, bufcap - strlen(buf) - 1);
            }
            strcat(buf, "\"");
        } else {
            strcat(buf, "null");
        }
        strcat(buf, ",\"last_accessed_time\":");
        snprintf(num, sizeof(num), "%d", g_state.dir[i].last_accessed_time);
        strncat(buf, num, bufcap - strlen(buf) - 1);
        strcat(buf, "}");
    }
    strcat(buf, "},\n  \"acls\":{");
    for (size_t i = 0; i < g_state.n_acls; ++i) {
        if (i) strcat(buf, ",");
        // key = filename
        strcat(buf, "\"");
        const char *s = g_state.acls[i].file;
        for (; s && *s; ++s) { if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\":{");
        // owner
        strcat(buf, "\"owner\":\"");
        s = g_state.acls[i].owner ? g_state.acls[i].owner : "";
        for (; s && *s; ++s) { if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\",\"grants\":{");
        for (size_t j = 0; j < g_state.acls[i].n_grants; ++j) {
            if (j) strcat(buf, ",");
            strcat(buf, "\"");
            const char *u = g_state.acls[i].grants[j].user;
            for (; u && *u; ++u) { if (*u == '"' || *u == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*u,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}            
            strcat(buf, "\":\"");
            int p = g_state.acls[i].grants[j].perm; const char *pv = (p==3?"RW":(p==2?"W":"R"));
            strncat(buf, pv, bufcap - strlen(buf) - 1);
            strcat(buf, "\"");
        }
        strcat(buf, "}}");
    }
    strcat(buf, "},\n  \"replicas\":{");
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (i) strcat(buf, ",");
        strcat(buf, "\"");
        const char *s = g_state.dir[i].file;
        for (; s && *s; ++s) { if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\":[");
        for (size_t j=0;j<g_state.dir[i].n_repl;j++) {
            if (j) strcat(buf, ",");
            char num[32]; snprintf(num, sizeof(num), "%d", g_state.dir[i].replicas[j]);
            strncat(buf, num, bufcap - strlen(buf) - 1);
        }
        strcat(buf, "]");
    }
    strcat(buf, "},\n  \"requests\":{");
    for (size_t i=0;i<g_state.n_requests;i++) {
        if (i) strcat(buf, ",");
        strcat(buf, "\"");
        const char *s = g_state.requests[i].file;
        for (; s && *s; ++s) { if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\":[");
        for (size_t j=0;j<g_state.requests[i].n_users;j++) {
            if (j) strcat(buf, ",");
            // New format: object with user and mode
            strcat(buf, "{\"user\":\"");
            const char *u = g_state.requests[i].users[j];
            for (; u && *u; ++u) { if (*u == '"' || *u == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*u,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}            
            strcat(buf, "\",\"mode\":\"");
            char md[2] = { g_state.requests[i].modes ? g_state.requests[i].modes[j] : 'R', 0 };
            strncat(buf, md, bufcap - strlen(buf) - 1);
            strcat(buf, "\"}");
        }
        strcat(buf, "]");
    }
    strcat(buf, "},\n  \"folders\":[");
    for (size_t i = 0; i < g_state.n_folders; ++i) {
        if (i) strcat(buf, ",");
        strcat(buf, "\"");
        const char *s = g_state.folders[i];
        for (; s && *s; ++s) { if (*s == '"' || *s == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\"");
    }
    strcat(buf, "],\n  \"trash\":[");
    for (size_t i=0;i<g_state.n_trash;i++) {
        if (i) strcat(buf, ",");
        strcat(buf, "{\"file\":\"");
        const char *s = g_state.trash[i].file; for (; s && *s; ++s) { if (*s=='"' || *s=='\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\",\"trashed\":\"");
        s = g_state.trash[i].trashed; for (; s && *s; ++s) { if (*s=='"' || *s=='\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\",\"owner\":\"");
        s = g_state.trash[i].owner; for (; s && *s; ++s) { if (*s=='"' || *s=='\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*s,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}        
        strcat(buf, "\",\"ssid\":");
        char num[64]; snprintf(num, sizeof(num), "%d", g_state.trash[i].ssid); strncat(buf, num, bufcap - strlen(buf) - 1);
        strcat(buf, ",\"when\":"); snprintf(num, sizeof(num), "%d", g_state.trash[i].when); strncat(buf, num, bufcap - strlen(buf) - 1);
        strcat(buf, "}");
    }
    strcat(buf, "]\n}\n");

    int rc = write_atomic(path, buf, strlen(buf));
    free(buf);
    return rc;
}

static void parse_users_array(const char *json) {
    const char *p = strstr(json, "\"users\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') break;
        p++;
        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p += 2; else p++;
        }
        size_t len = (size_t)(p - start);
        char tmp[256];
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        size_t j = 0; const char *s = start;
        while (j < len && *s) {
            if (*s == '\\' && s[1]) { s++; tmp[j++] = *s++; }
            else { tmp[j++] = *s++; }
        }
        tmp[j] = '\0';
        nm_state_add_user(tmp);
        if (*p == '"') p++;
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
}

static void parse_active_array(const char *json) {
    const char *p = strstr(json, "\"active\"");
    if (!p) return;
    p = strchr(p, '[');
    if (!p) return;
    p++;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '"') break;
        p++;
        const char *start = p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
        size_t len = (size_t)(p - start);
        char tmp[256]; if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        size_t j = 0; const char *s = start;
        while (j < len && *s) { if (*s == '\\' && s[1]) { s++; tmp[j++] = *s++; } else { tmp[j++] = *s++; } }
        tmp[j] = '\0';
        nm_state_set_user_active(tmp, 1);
        if (*p == '"') p++;
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
}

static void parse_directory_object(const char *json) {
    const char *p = strstr(json, "\"directory\"");
    if (!p) return;
    p = strchr(p, '{');
    if (!p) return;
    p++;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++;
        if (*p == '}') break;
        if (*p != '"') break;
        p++;
        const char *kstart = p;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
        size_t klen = (size_t)(p - kstart);
        char key[256];
        size_t j = 0; const char *s = kstart;
        while (j < klen && *s) {
            if (*s == '\\' && s[1]) { s++; key[j++] = *s++; }
            else { key[j++] = *s++; }
        }
        key[j] = '\0';
        if (*p == '"') p++;
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p == ' ' || *p == '\n' || *p == '\t') p++;
        
        // Check if value is an object or integer (backwards compatibility)
        if (*p == '{') {
            // New format: object with ss_id and metadata
            p++;
            int ssid = 0;
            char mod_user[128] = {0};
            int mod_time = 0;
            char acc_user[128] = {0};
            int acc_time = 0;
            
            while (*p && *p != '}') {
                while (*p == ' ' || *p == '\n' || *p == '\t' || *p == ',') p++;
                if (*p == '}') break;
                if (*p != '"') break;
                p++;
                const char *fstart = p;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
                size_t flen = (size_t)(p - fstart);
                char field[64];
                size_t fj = 0; const char *fs = fstart;
                while (fj < flen && *fs && fj < sizeof(field) - 1) {
                    if (*fs == '\\' && fs[1]) { fs++; field[fj++] = *fs++; }
                    else { field[fj++] = *fs++; }
                }
                field[fj] = '\0';
                if (*p == '"') p++;
                while (*p && *p != ':') p++;
                if (*p == ':') p++;
                while (*p == ' ' || *p == '\n' || *p == '\t') p++;
                
                if (strcmp(field, "ss_id") == 0) {
                    const char *vstart = p;
                    while (*p && *p != ',' && *p != '}') p++;
                    char vbuf[32]; size_t vlen = (size_t)(p - vstart); if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf)-1;
                    memcpy(vbuf, vstart, vlen); vbuf[vlen] = '\0';
                    ssid = atoi(vbuf);
                } else if (strcmp(field, "last_modified_user") == 0 || strcmp(field, "last_accessed_user") == 0) {
                    if (*p == '"') {
                        p++;
                        const char *ustart = p;
                        while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
                        size_t ulen = (size_t)(p - ustart);
                        char *target = strcmp(field, "last_modified_user") == 0 ? mod_user : acc_user;
                        size_t uj = 0; const char *us = ustart;
                        while (uj < ulen && *us && uj < 127) {
                            if (*us == '\\' && us[1]) { us++; target[uj++] = *us++; }
                            else { target[uj++] = *us++; }
                        }
                        target[uj] = '\0';
                        if (*p == '"') p++;
                    } else if (*p == 'n' && strncmp(p, "null", 4) == 0) {
                        p += 4;
                    }
                } else if (strcmp(field, "last_modified_time") == 0 || strcmp(field, "last_accessed_time") == 0) {
                    const char *vstart = p;
                    while (*p && *p != ',' && *p != '}') p++;
                    char vbuf[32]; size_t vlen = (size_t)(p - vstart); if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf)-1;
                    memcpy(vbuf, vstart, vlen); vbuf[vlen] = '\0';
                    if (strcmp(field, "last_modified_time") == 0) {
                        mod_time = atoi(vbuf);
                    } else {
                        acc_time = atoi(vbuf);
                    }
                }
            }
            if (*p == '}') p++;
            
            // Set directory entry with metadata
            nm_state_set_dir(key, ssid);
            if (mod_user[0]) nm_state_set_file_modified(key, mod_user, mod_time);
            if (acc_user[0]) nm_state_set_file_accessed(key, acc_user, acc_time);
        } else {
            // Old format: just integer ssid (backwards compatibility)
            const char *vstart = p;
            while (*p && *p != ',' && *p != '}') p++;
            char vbuf[32]; size_t vlen = (size_t)(p - vstart); if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf)-1;
            memcpy(vbuf, vstart, vlen); vbuf[vlen] = '\0';
            int ssid = atoi(vbuf);
            nm_state_set_dir(key, ssid);
        }
        if (*p == ',') p++;
    }
}

// Forward decl for folders JSON parser
static void parse_folders_array(const char *json);
static void parse_replicas_object(const char *json);
static void parse_requests_object(const char *json);

int nm_state_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        // First run: create a skeleton file
        (void)nm_state_save(path);
        return 0;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 10 * 1024 * 1024) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    parse_users_array(buf);
    parse_active_array(buf);
    parse_directory_object(buf);
    // Parse acls
    const char *p = strstr(buf, "\"acls\"");
    if (p) {
        const char *obj = strchr(p, '{');
        if (obj) {
            obj++;
            while (*obj && *obj != '}') {
                while (*obj==' '||*obj=='\n'||*obj=='\t'||*obj==',') obj++;
                if (*obj=='}') break;
                if (*obj!='"') break;
                obj++;
                const char *fk = obj; while (*obj && *obj!='"') { if (*obj=='\\' && obj[1]) obj+=2; else obj++; }
                size_t fklen = (size_t)(obj - fk);
                char file[256]; size_t j=0; const char *s=fk; while (j<fklen && *s){ if(*s=='\\'&&s[1]){s++; file[j++]=*s++;} else file[j++]=*s++; } file[j]='\0';
                if (*obj=='"') obj++;
                while (*obj && *obj!='{') obj++;
                if (*obj!='{') break;
                obj++;
                // inner object: owner:"..", grants:{...}
                char owner[128] = {0};
                // find owner
                const char *ow = strstr(obj, "\"owner\"");
                if (ow) { const char *q=strchr(ow, '"'); if(q){ q++; const char *qe=strchr(q,'"'); if(qe){ size_t ol=(size_t)(qe-q); if(ol>=sizeof(owner)) ol=sizeof(owner)-1; size_t k=0; const char *t=q; while(k<ol && *t){ if(*t=='\\'&&t[1]){t++; owner[k++]=*t++;} else owner[k++]=*t++; } owner[k]='\0'; }} }
                // find grants object
                const char *gr = strstr(obj, "\"grants\"");
                if (gr) {
                    const char *go = strchr(gr, '{'); if (go){ go++;
                        while (*go && *go!='}') {
                            while(*go==' '||*go=='\n'||*go=='\t'||*go==',') go++;
                            if (*go=='}') break;
                            if (*go!='"') break;
                            go++;
                            const char *uk = go; while(*go && *go!='"'){ if(*go=='\\'&&go[1]) go+=2; else go++; }
                            size_t ulen=(size_t)(go-uk); char u[128]; size_t ui=0; const char *tp=uk; while(ui<ulen&&*tp){ if(*tp=='\\'&&tp[1]){tp++; u[ui++]=*tp++;} else u[ui++]=*tp++; } u[ui]='\0';
                            if (*go=='"') go++;
                            while(*go && *go!=':') go++;
                            if(*go==':') go++;
                            while(*go==' '||*go=='\n'||*go=='\t') go++;
                            if (*go=='"'){
                                go++;
                                const char *pe= strchr(go,'"'); if(pe){ char pv[4]={0}; size_t pl=(size_t)(pe-go); if(pl>3) pl=3; memcpy(pv,go,pl);
                                int perm = (pv[0]=='R' && pv[1]=='W')?3:(pv[0]=='W'?2:1);
                                nm_acl_set_owner(file, owner[0]?owner:NULL);
                                if (u[0]) nm_acl_grant(file, u, perm);
                                go = pe+1;
                            }}
                            while(*go && *go!=',' && *go!='}') go++;
                            if (*go==',') go++;
                        }
                    }
                } else {
                    // No grants; still set owner
                    nm_acl_set_owner(file, owner[0]?owner:NULL);
                }
                // move obj to end of this acl entry
                while (*obj && *obj!='}') obj++;
                if (*obj=='}') obj++;
            }
        }
    }
    // Parse replicas and requests (optional, backward-compatible)
    parse_replicas_object(buf);
    parse_requests_object(buf);
    // Parse folders
    parse_folders_array(buf);
    // Parse trash (optional)
    const char *tp = strstr(buf, "\"trash\"");
    if (tp) {
        const char *arr = strchr(tp, '[');
        if (arr) {
            arr++;
            while (*arr) {
                while (*arr==' '||*arr=='\n'||*arr=='\t'||*arr==',') arr++;
                if (*arr==']') break;
                if (*arr!='{') break;
                arr++;
                char of[256]={0}, tr[256]={0}, owner[128]={0};
                int ssid=0, when=0;
                const char *obj = arr;
                while (*obj && *obj!='}') {
                    while (*obj==' '||*obj=='\n'||*obj=='\t'||*obj==',') obj++;
                    if (*obj!='"') { while(*obj && *obj!=',' && *obj!='}') obj++; if (*obj==',') obj++; continue; }
                    obj++;
                    const char *ks=obj; while(*obj && *obj!='"'){ if(*obj=='\\'&&obj[1]) obj+=2; else obj++; }
                    size_t klen=(size_t)(obj-ks); char key[16]; size_t ki=0; const char *ps=ks; while(ki<klen&&*ps){ if(*ps=='\\'&&ps[1]){ps++; key[ki++]=*ps++;} else key[ki++]=*ps++; } key[ki]='\0';
                    if (*obj=='"') obj++;
                    while (*obj && *obj!=':') obj++;
                    if (*obj==':') obj++;
                    while (*obj==' '||*obj=='\n'||*obj=='\t') obj++;
                    if (strcmp(key, "file")==0 && *obj=='"') {
                        obj++; const char *vs=obj; while(*obj && *obj!='"'){ if(*obj=='\\'&&obj[1]) obj+=2; else obj++; }
                        size_t vlen=(size_t)(obj-vs); size_t j=0; const char *s=vs; while(j<vlen&&*s){ if(*s=='\\'&&s[1]){s++; of[j++]=*s++;} else of[j++]=*s++; } of[j]='\0';
                        if (*obj=='"') obj++;
                    } else if (strcmp(key, "trashed")==0 && *obj=='"') {
                        obj++; const char *vs=obj; while(*obj && *obj!='"'){ if(*obj=='\\'&&obj[1]) obj+=2; else obj++; }
                        size_t vlen=(size_t)(obj-vs); size_t j=0; const char *s=vs; while(j<vlen&&*s){ if(*s=='\\'&&s[1]){s++; tr[j++]=*s++;} else tr[j++]=*s++; } tr[j]='\0';
                        if (*obj=='"') obj++;
                    } else if (strcmp(key, "owner")==0 && *obj=='"') {
                        obj++; const char *vs=obj; while(*obj && *obj!='"'){ if(*obj=='\\'&&obj[1]) obj+=2; else obj++; }
                        size_t vlen=(size_t)(obj-vs); size_t j=0; const char *s=vs; while(j<vlen&&*s){ if(*s=='\\'&&s[1]){s++; owner[j++]=*s++;} else owner[j++]=*s++; } owner[j]='\0';
                        if (*obj=='"') obj++;
                    } else if (strcmp(key, "ssid")==0) {
                        const char *vs=obj; while(*obj && *obj!=',' && *obj!='}') obj++; char nb[32]; size_t n=(size_t)(obj-vs); if(n>=sizeof(nb)) n=sizeof(nb)-1; memcpy(nb,vs,n); nb[n]='\0'; ssid=atoi(nb);
                    } else if (strcmp(key, "when")==0) {
                        const char *vs=obj; while(*obj && *obj!=',' && *obj!='}') obj++; char nb[32]; size_t n=(size_t)(obj-vs); if(n>=sizeof(nb)) n=sizeof(nb)-1; memcpy(nb,vs,n); nb[n]='\0'; when=atoi(nb);
                    } else {
                        while(*obj && *obj!=',' && *obj!='}') obj++;
                    }
                    if (*obj==',') obj++;
                }
                if (*obj=='}') {
                    if (of[0] && tr[0]) nm_state_trash_add(of, tr, ssid, owner[0]?owner:NULL, when);
                    obj++;
                }
                arr = obj; while (*arr && *arr!=',' && *arr!=']') arr++; if (*arr==',') arr++;
            }
        }
    }
    free(buf);
    return 0;
}

// ---- Trash APIs ----
static void ensure_trash_cap(size_t need) {
    if (g_state.cap_trash >= need) return;
    size_t nc = g_state.cap_trash ? g_state.cap_trash * 2 : 8;
    while (nc < need) nc *= 2;
    struct trash_entry *p = (struct trash_entry *)realloc(g_state.trash, nc * sizeof(*g_state.trash));
    if (!p) return;
    g_state.trash = p; g_state.cap_trash = nc;
}

int nm_state_trash_add(const char *file, const char *trashed_path, int ssid, const char *owner, int when) {
    if (!file || !*file || !trashed_path || !*trashed_path) return 0;
    // If exists, replace
    int found = 0;
    size_t index = trash_map_find(file, &found);
    if (found && index < g_state.n_trash) {
        struct trash_entry *e = &g_state.trash[index];
        if (e->trashed) free(e->trashed);
        e->trashed = strdup(trashed_path);
        e->ssid = ssid;
        if (e->owner) free(e->owner);
        e->owner = owner && *owner ? strdup(owner) : NULL;
        e->when = when;
        return 1;
    }
    ensure_trash_cap(g_state.n_trash + 1);
    if (g_state.cap_trash < g_state.n_trash + 1) return 0;
    struct trash_entry *e = &g_state.trash[g_state.n_trash];
    e->file = strdup(file);
    e->trashed = strdup(trashed_path);
    e->ssid = ssid;
    e->owner = (owner && *owner) ? strdup(owner) : NULL;
    e->when = when;
    // Add to hash map
    trash_map_insert(file, g_state.n_trash);
    g_state.n_trash++;
    return 1;
}

int nm_state_trash_remove(const char *file) {
    if (!file || !*file) return 0;
    int found = 0;
    size_t index = trash_map_find(file, &found);
    if (!found || index >= g_state.n_trash) return 0;
    
    struct trash_entry *e = &g_state.trash[index];
    if (e->file) free(e->file);
    if (e->trashed) free(e->trashed);
    if (e->owner) free(e->owner);
    
    // Remove from hash map
    trash_map_remove(file);
    
    if (index != g_state.n_trash-1) {
        g_state.trash[index] = g_state.trash[g_state.n_trash-1];
        // Update hash map for swapped element
        trash_map_update_index(g_state.trash[index].file, index);
    }
    g_state.n_trash--;
    return 1;
}

int nm_state_trash_find(const char *file, char *trashed_out, size_t trashed_out_sz, int *ssid_out, char *owner_out, size_t owner_out_sz, int *when_out) {
    if (trashed_out && trashed_out_sz) {
        trashed_out[0] = '\0';
    }
    if (owner_out && owner_out_sz) {
        owner_out[0] = '\0';
    }
    if (ssid_out) {
        *ssid_out = 0;
    }
    if (when_out) {
        *when_out = 0;
    }
    
    // O(1) hash map lookup
    int found = 0;
    size_t index = trash_map_find(file, &found);
    if (!found || index >= g_state.n_trash) return -1;
    
    struct trash_entry *e = &g_state.trash[index];
    if (trashed_out && trashed_out_sz) snprintf(trashed_out, trashed_out_sz, "%s", e->trashed?e->trashed:"");
    if (ssid_out) *ssid_out = e->ssid;
    if (owner_out && owner_out_sz) snprintf(owner_out, owner_out_sz, "%s", e->owner?e->owner:"");
    if (when_out) *when_out = e->when;
    return 0;
}

size_t nm_state_get_trash(char files[][128], char trashed[][128], int ssids[], char owners[][128], int whens[], size_t max_entries) {
    size_t c = 0;
    for (size_t i=0;i<g_state.n_trash && c < max_entries; ++i) {
        snprintf(files[c], 128, "%s", g_state.trash[i].file);
        snprintf(trashed[c], 128, "%s", g_state.trash[i].trashed?g_state.trash[i].trashed:"");
        ssids[c] = g_state.trash[i].ssid;
        if (owners) snprintf(owners[c], 128, "%s", g_state.trash[i].owner?g_state.trash[i].owner:"");
        if (whens) whens[c] = g_state.trash[i].when;
        c++;
    }
    return c;
}

static void ensure_dir_cap(size_t need) {
    if (g_state.cap_dir >= need) return;
    size_t newcap = g_state.cap_dir ? g_state.cap_dir * 2 : 8;
    while (newcap < need) newcap *= 2;
    struct dir_entry *p = (struct dir_entry *)realloc(g_state.dir, newcap * sizeof(*g_state.dir));
    if (!p) return;
    g_state.dir = p;
    g_state.cap_dir = newcap;
}

int nm_state_set_dir(const char *file, int ss_id) {
    if (!file || !*file) return 0;
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, file) == 0) {
            if (g_state.dir[i].ss_id == ss_id) return 0;
            g_state.dir[i].ss_id = ss_id; return 1;
        }
    }
    ensure_dir_cap(g_state.n_dir + 1);
    if (g_state.cap_dir < g_state.n_dir + 1) return 0;
    g_state.dir[g_state.n_dir].file = strdup(file);
    if (!g_state.dir[g_state.n_dir].file) return 0;
    g_state.dir[g_state.n_dir].ss_id = ss_id;
    g_state.dir[g_state.n_dir].replicas = NULL; g_state.dir[g_state.n_dir].n_repl = 0; g_state.dir[g_state.n_dir].cap_repl = 0;
    g_state.dir[g_state.n_dir].last_modified_user = NULL;
    g_state.dir[g_state.n_dir].last_modified_time = 0;
    g_state.dir[g_state.n_dir].last_accessed_user = NULL;
    g_state.dir[g_state.n_dir].last_accessed_time = 0;
    g_state.n_dir++;
    return 1;
}

int nm_state_find_dir(const char *file, int *out_ss_id) {
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, file) == 0) {
            if (out_ss_id) *out_ss_id = g_state.dir[i].ss_id;
            return 0;
        }
    }
    return -1;
}

size_t nm_state_get_dir(char files[][128], int ss_ids[], size_t max_entries) {
    size_t c = 0;
    for (size_t i = 0; i < g_state.n_dir && c < max_entries; ++i) {
        snprintf(files[c], 128, "%s", g_state.dir[i].file);
        ss_ids[c] = g_state.dir[i].ss_id;
        c++;
    }
    return c;
}

int nm_state_del_dir(const char *file) {
    if (!file || !*file) return 0;
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, file) == 0) {
            free(g_state.dir[i].file);
            if (g_state.dir[i].replicas) { free(g_state.dir[i].replicas); g_state.dir[i].replicas=NULL; }
            if (g_state.dir[i].last_modified_user) { free(g_state.dir[i].last_modified_user); g_state.dir[i].last_modified_user=NULL; }
            if (g_state.dir[i].last_accessed_user) { free(g_state.dir[i].last_accessed_user); g_state.dir[i].last_accessed_user=NULL; }
            // move last into i
            if (i != g_state.n_dir - 1) g_state.dir[i] = g_state.dir[g_state.n_dir - 1];
            g_state.n_dir--;
            return 1;
        }
    }
    return 0;
}

int nm_state_rename_dir(const char *old_file, const char *new_file) {
    if (!old_file || !*old_file || !new_file || !*new_file) return 0;
    // Ensure new_file doesn't exist
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, new_file) == 0) return 0; // conflict
    }
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, old_file) == 0) {
            free(g_state.dir[i].file);
            g_state.dir[i].file = strdup(new_file);
            return 1;
        }
    }
    return 0;
}

// ---- Replicas ----
static void ensure_repl_cap(struct dir_entry *e, size_t need) {
    if (e->cap_repl >= need) return;
    size_t nc = e->cap_repl ? e->cap_repl * 2 : 4;
    while (nc < need) nc *= 2;
    int *nr = (int *)realloc(e->replicas, nc * sizeof(int));
    if (!nr) return;
    e->replicas = nr; e->cap_repl = nc;
}

int nm_state_set_replicas(const char *file, const int *replicas, size_t n) {
    if (!file) return -1;
    for (size_t i=0;i<g_state.n_dir;i++) {
        if (strcmp(g_state.dir[i].file, file)==0) {
            struct dir_entry *e = &g_state.dir[i];
            // Check if unchanged
            int same = (e->n_repl == n);
            if (same) {
                for (size_t j=0;j<n;j++) { if (e->replicas[j] != replicas[j]) { same = 0; break; } }
            }
            if (same) return 0;
            ensure_repl_cap(e, n);
            if (e->cap_repl < n) return -1;
            e->n_repl = n;
            for (size_t j=0;j<n;j++) e->replicas[j] = replicas[j];
            return 1;
        }
    }
    return -1;
}

size_t nm_state_get_replicas(const char *file, int *out, size_t max) {
    for (size_t i=0;i<g_state.n_dir;i++) {
        if (strcmp(g_state.dir[i].file, file)==0) {
            size_t n = g_state.dir[i].n_repl;
            size_t c = n < max ? n : max;
            for (size_t j=0;j<c;j++) out[j] = g_state.dir[i].replicas[j];
            return n;
        }
    }
    return 0;
}

int nm_state_get_primary(const char *file, int *out_ssid) {
    return nm_state_find_dir(file, out_ssid);
}

// ---- Metadata tracking (last modified/accessed user and time) ----
int nm_state_set_file_modified(const char *file, const char *user, int time) {
    if (!file || !*file) return 0;
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, file) == 0) {
            if (g_state.dir[i].last_modified_user) free(g_state.dir[i].last_modified_user);
            g_state.dir[i].last_modified_user = (user && *user) ? strdup(user) : NULL;
            g_state.dir[i].last_modified_time = time;
            return 1;
        }
    }
    return 0;
}

int nm_state_set_file_accessed(const char *file, const char *user, int time) {
    if (!file || !*file) return 0;
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, file) == 0) {
            if (g_state.dir[i].last_accessed_user) free(g_state.dir[i].last_accessed_user);
            g_state.dir[i].last_accessed_user = (user && *user) ? strdup(user) : NULL;
            g_state.dir[i].last_accessed_time = time;
            return 1;
        }
    }
    return 0;
}

int nm_state_get_file_metadata(const char *file, char *mod_user_out, size_t mod_user_sz, int *mod_time_out, char *acc_user_out, size_t acc_user_sz, int *acc_time_out) {
    if (!file || !*file) return -1;
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        if (strcmp(g_state.dir[i].file, file) == 0) {
            if (mod_user_out && mod_user_sz) {
                if (g_state.dir[i].last_modified_user) {
                    snprintf(mod_user_out, mod_user_sz, "%s", g_state.dir[i].last_modified_user);
                } else {
                    mod_user_out[0] = '\0';
                }
            }
            if (mod_time_out) *mod_time_out = g_state.dir[i].last_modified_time;
            if (acc_user_out && acc_user_sz) {
                if (g_state.dir[i].last_accessed_user) {
                    snprintf(acc_user_out, acc_user_sz, "%s", g_state.dir[i].last_accessed_user);
                } else {
                    acc_user_out[0] = '\0';
                }
            }
            if (acc_time_out) *acc_time_out = g_state.dir[i].last_accessed_time;
            return 0;
        }
    }
    return -1;
}

// ---- ACL helpers ----
static void ensure_acl_cap(size_t need){
    if (g_state.cap_acls >= need) return;
    size_t nc = g_state.cap_acls? g_state.cap_acls*2:8;
    while(nc<need) nc*=2;
    struct acl_entry *p = (struct acl_entry*)realloc(g_state.acls, nc*sizeof(*g_state.acls));
    if(!p) return;
    g_state.acls=p;
    g_state.cap_acls=nc;
}

static struct acl_entry* find_acl(const char *file){
    // O(1) hash map lookup
    int found = 0;
    size_t index = acl_map_find(file, &found);
    if (found && index < g_state.n_acls) {
        return &g_state.acls[index];
    }
    return NULL;
}

static struct acl_entry* upsert_acl(const char *file){
    struct acl_entry *e = find_acl(file);
    if (e) return e;
    ensure_acl_cap(g_state.n_acls+1);
    if (g_state.cap_acls < g_state.n_acls+1) return NULL;
    e = &g_state.acls[g_state.n_acls];
    memset(e, 0, sizeof(*e));
    e->file = strdup(file);
    // Add to hash map
    acl_map_insert(file, g_state.n_acls);
    g_state.n_acls++;
    return e;
}

static void ensure_grant_cap(struct acl_entry *e, size_t need){
    if (e->cap_grants >= need) return;
    size_t nc = e->cap_grants? e->cap_grants*2:4;
    while(nc<need) nc*=2;
    struct acl_user *p = (struct acl_user*)realloc(e->grants, nc*sizeof(*e->grants));
    if(!p) return;
    e->grants=p;
    e->cap_grants=nc;
}

int nm_acl_set_owner(const char *file, const char *owner) {
    if (!file || !*file) return 0;
    struct acl_entry *e = upsert_acl(file); if (!e) return 0;
    if (e->owner) { free(e->owner); e->owner=NULL; }
    if (owner && *owner) e->owner = strdup(owner);
    return 1;
}

int nm_acl_grant(const char *file, const char *user, int perm) {
    if (!file || !*file || !user || !*user) return 0;
    struct acl_entry *e = upsert_acl(file); if (!e) return 0;
    for (size_t i=0;i<e->n_grants;i++){ if (strcmp(e->grants[i].user, user)==0){ e->grants[i].perm = perm; return 1; }}
    ensure_grant_cap(e, e->n_grants+1); if (e->cap_grants < e->n_grants+1) return 0;
    e->grants[e->n_grants].user = strdup(user);
    e->grants[e->n_grants].perm = perm;
    e->n_grants++;
    return 1;
}

int nm_acl_revoke(const char *file, const char *user) {
    if (!file || !*file || !user || !*user) return 0;
    struct acl_entry *e = find_acl(file); if (!e) return 0;
    for (size_t i=0;i<e->n_grants;i++){ if (strcmp(e->grants[i].user, user)==0){ free(e->grants[i].user); if(i!=e->n_grants-1) e->grants[i]=e->grants[e->n_grants-1]; e->n_grants--; return 1; }}
    return 0;
}

int nm_acl_delete(const char *file) {
    if (!file || !*file) return 0;
    int found = 0;
    size_t index = acl_map_find(file, &found);
    if (!found || index >= g_state.n_acls) return 0;
    
    struct acl_entry *e = &g_state.acls[index];
    if (e->owner) { free(e->owner); e->owner=NULL; }
    for (size_t j=0;j<e->n_grants;j++) if (e->grants[j].user) free(e->grants[j].user);
    if (e->grants) { free(e->grants); e->grants=NULL; }
    if (e->file) { free(e->file); e->file=NULL; }
    
    // Remove from hash map
    acl_map_remove(file);
    
    // swap with last and update hash map
    if (index != g_state.n_acls-1) {
        g_state.acls[index] = g_state.acls[g_state.n_acls-1];
        // Update hash map for swapped element
        acl_map_update_index(g_state.acls[index].file, index);
    }
    g_state.n_acls--;
    return 1;
}

int nm_acl_check(const char *file, const char *user, const char *op) {
    if (!file || !user || !op) return -1;
    struct acl_entry *e = find_acl(file);
    // If no ACL entry exists, default allow for READ? Conservative deny.
    if (!e) return -1;
    if (e->owner && strcmp(e->owner, user)==0) return 0;
    int need;
    if (strcmp(op, "READ") == 0 || strcmp(op, "VIEWCHECKPOINT") == 0 || strcmp(op, "LISTCHECKPOINTS") == 0) need = ACL_R; // read-like
    else need = ACL_W; // WRITE/UNDO/REVERT and others require W
    for (size_t i=0;i<e->n_grants;i++){ if (strcmp(e->grants[i].user, user)==0){ if ((e->grants[i].perm & need) == need) return 0; else return -1; }}
    // Fallback: if 'anonymous' has the required permission, allow any user
    for (size_t i=0;i<e->n_grants;i++) {
        if (strcmp(e->grants[i].user, "anonymous")==0) {
            if ((e->grants[i].perm & need) == need) return 0;
            break;
        }
    }
    return -1;
}

int nm_acl_rename(const char *old_file, const char *new_file) {
    if (!old_file || !*old_file || !new_file || !*new_file) return 0;
    struct acl_entry *e = find_acl(old_file);
    if (!e) return 0;
    // Ensure no destination exists
    if (find_acl(new_file)) return 0;
    
    // Get the index of this ACL entry before updating
    int found = 0;
    size_t index = acl_map_find(old_file, &found);
    if (!found) return 0;
    
    // Remove old filename from hash map
    acl_map_remove(old_file);
    
    // Update the filename in the ACL entry
    free(e->file);
    e->file = strdup(new_file);
    
    // Insert new filename into hash map with same index
    acl_map_insert(new_file, index);
    
    return 1;
}

int nm_acl_get_owner(const char *file, char *owner_out, size_t owner_out_sz) {
    if (owner_out && owner_out_sz) owner_out[0] = '\0';
    struct acl_entry *e = find_acl(file);
    if (!e || !e->owner) return -1;
    if (owner_out && owner_out_sz) {
        snprintf(owner_out, owner_out_sz, "%s", e->owner);
    }
    return 0;
}

size_t nm_acl_format_access(const char *file, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return 0;
    dst[0] = '\0';
    struct acl_entry *e = find_acl(file);
    if (!e) return 0;
    size_t w = 0; int first = 1;
    // Owner first
    if (e->owner) {
        w += snprintf(dst + w, dst_sz - w, "%s%s (RW)", first?"":", ", e->owner);
        first = 0;
    }
    for (size_t i = 0; i < e->n_grants; ++i) {
        const char *u = e->grants[i].user; int p = e->grants[i].perm; const char *pv = (p==3?"RW":(p==2?"W":"R"));
        // Skip owner if present in grants
        if (e->owner && strcmp(e->owner, u) == 0) continue;
        w += snprintf(dst + w, dst_sz - w, "%s%s (%s)", first?"":", ", u, pv);
        first = 0;
        if (w >= dst_sz) break;
    }
    return w;
}

// ---- Folders (M13) ----
static void ensure_folder_cap(size_t need) {
    if (g_state.cap_folders >= need) return;
    size_t nc = g_state.cap_folders ? g_state.cap_folders * 2 : 8;
    while (nc < need) nc *= 2;
    char **p = (char **)realloc(g_state.folders, nc * sizeof(char *));
    if (!p) return;
    g_state.folders = p;
    g_state.cap_folders = nc;
}

static int folder_exists(const char *path) {
    // O(1) hash map lookup
    return folder_map_exists(path);
}

int nm_state_add_folder(const char *path) {
    if (!path || !*path) return 0;
    if (folder_exists(path)) return 0;
    ensure_folder_cap(g_state.n_folders + 1);
    if (g_state.cap_folders < g_state.n_folders + 1) return 0;
    g_state.folders[g_state.n_folders] = strdup(path);
    if (!g_state.folders[g_state.n_folders]) return 0;
    // Add to hash map
    folder_map_insert(path, g_state.n_folders);
    g_state.n_folders++;
    return 1;
}

int nm_state_remove_folder(const char *path) {
    if (!path || !*path) return 0;
    for (size_t i = 0; i < g_state.n_folders; ++i) {
        if (strcmp(g_state.folders[i], path) == 0) {
            // Remove from hash map
            folder_map_remove(path);
            free(g_state.folders[i]);
            if (i != g_state.n_folders - 1) {
                g_state.folders[i] = g_state.folders[g_state.n_folders - 1];
                // Update hash map index for swapped element
                folder_map_update_index(g_state.folders[i], i);
            }
            g_state.n_folders--;
            return 1;
        }
    }
    return 0;
}

size_t nm_state_get_folders(char folders[][256], size_t max_entries) {
    size_t c = 0;
    for (size_t i = 0; i < g_state.n_folders && c < max_entries; ++i) {
        snprintf(folders[c], 256, "%s", g_state.folders[i]);
        c++;
    }
    return c;
}

int nm_state_move_folder_prefix(const char *old_path, const char *new_path,
                                char files[][128], char new_files[][128], int ssids[], size_t max_files) {
    if (!old_path || !new_path || !*old_path || !*new_path) return 0;
    size_t moved = 0;
    size_t oldlen = strlen(old_path);
    // Update folders list: rename if exact match or update prefixes
    for (size_t i = 0; i < g_state.n_folders; ++i) {
        if (strcmp(g_state.folders[i], old_path) == 0) {
            // Remove old path from hash map
            folder_map_remove(g_state.folders[i]);
            free(g_state.folders[i]);
            g_state.folders[i] = strdup(new_path);
            // Insert new path into hash map
            folder_map_insert(new_path, i);
        } else if (strncmp(g_state.folders[i], old_path, oldlen) == 0 && g_state.folders[i][oldlen] == '/') {
            // nested folder
            const char *rest = g_state.folders[i] + oldlen;
            char buf[512]; snprintf(buf, sizeof(buf), "%s%s", new_path, rest);
            // Remove old path from hash map
            folder_map_remove(g_state.folders[i]);
            free(g_state.folders[i]);
            g_state.folders[i] = strdup(buf);
            // Insert new path into hash map
            folder_map_insert(buf, i);
        }
    }
    // Collect and update file mappings under prefix
    for (size_t i = 0; i < g_state.n_dir; ++i) {
        const char *fname = g_state.dir[i].file;
        if (strncmp(fname, old_path, oldlen) == 0 && (fname[oldlen] == '/' || fname[oldlen] == '\0')) {
            // compute new name
            const char *rest = fname + oldlen;
            char nbuf[256];
            if (*rest == '/') rest++; // skip separator
            if (*rest) snprintf(nbuf, sizeof(nbuf), "%s/%s", new_path, rest);
            else snprintf(nbuf, sizeof(nbuf), "%s", new_path);
            if (moved < max_files) {
                safe_copy(files[moved], 128, fname);
                safe_copy(new_files[moved], 128, nbuf);
                ssids[moved] = g_state.dir[i].ss_id;
            }
            free(g_state.dir[i].file);
            g_state.dir[i].file = strdup(nbuf);
            moved++;
        }
    }
    return (int)moved;
}

// ---- JSON load for folders ----
static void parse_folders_array(const char *json) {
    const char *p = strstr(json, "\"folders\"");
    if (!p) return;
    p = strchr(p, '['); if (!p) return; p++;
    while (*p) {
        while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
        if (*p == ']') break;
    if (*p != '"') break;
    p++;
        const char *start = p;
        while (*p && *p!='"') { if (*p=='\\' && p[1]) p+=2; else p++; }
        size_t len = (size_t)(p - start);
        char tmp[256]; size_t j=0; const char *s=start; while (j<len && *s) { if (*s=='\\' && s[1]) { s++; tmp[j++]=*s++; } else tmp[j++]=*s++; }
        tmp[j]='\0';
        nm_state_add_folder(tmp);
        if (*p=='"') p++;
        while (*p && *p!=',' && *p!=']') p++;
        if (*p==',') p++;
    }
}

// ---- JSON parsers for replicas/requests ----
static void parse_replicas_object(const char *json) {
    const char *p = strstr(json, "\"replicas\"");
    if (!p) return;
    p = strchr(p, '{'); if (!p) return; p++;
    while (*p) {
        while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
        if (*p=='}') break;
        if (*p!='"') break;
        p++;
        const char *kstart = p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
        size_t klen=(size_t)(p-kstart); char file[256]; size_t j=0; const char *s=kstart; while(j<klen&&*s){ if(*s=='\\'&&s[1]){s++; file[j++]=*s++;} else file[j++]=*s++; } file[j]='\0'; if (*p=='"') p++;
    while (*p && *p!='[') p++;
    if (*p!='[') break;
    p++;
        int reps[64]; size_t nr=0;
        while (*p && *p!=']') {
            while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
            const char *v = p; while (*p && *p!=',' && *p!=']') p++;
            char num[32]; size_t n=(size_t)(p-v); if (n>=sizeof(num)) n=sizeof(num)-1; memcpy(num,v,n); num[n]='\0';
            if (nr < 64) reps[nr++] = atoi(num);
            if (*p==',') p++;
        }
        if (*p==']') p++;
        nm_state_set_replicas(file, reps, nr);
    while (*p && *p!=',' && *p!='}') p++;
    if (*p==',') p++;
    }
}

static void ensure_req_cap(size_t need) {
    if (g_state.cap_requests >= need) return;
    size_t nc = g_state.cap_requests ? g_state.cap_requests * 2 : 8;
    while (nc < need) nc *= 2;
    struct req_entry *nr = (struct req_entry *)realloc(g_state.requests, nc * sizeof(*g_state.requests));
    if (!nr) return;
    g_state.requests = nr; g_state.cap_requests = nc;
}

static struct req_entry *find_req_entry(const char *file) {
    // O(1) hash map lookup
    int found = 0;
    size_t index = req_map_find(file, &found);
    if (found && index < g_state.n_requests) {
        return &g_state.requests[index];
    }
    return NULL;
}

int nm_state_add_request(const char *file, const char *user, char mode) {
    if (!file || !user || !*user) return 0;
    if (nm_state_find_dir(file, NULL) != 0) return 0; // only for known files
    struct req_entry *e = find_req_entry(file);
    if (!e) {
        ensure_req_cap(g_state.n_requests + 1);
        if (g_state.cap_requests < g_state.n_requests + 1) return 0;
        e = &g_state.requests[g_state.n_requests];
        memset(e, 0, sizeof(*e));
        e->file = strdup(file);
        // Add to hash map
        req_map_insert(file, g_state.n_requests);
        g_state.n_requests++;
    }
    // check duplicate
    for (size_t i=0;i<e->n_users;i++) if (strcmp(e->users[i], user)==0) return 0;
    size_t need = e->n_users + 1;
    if (e->cap_users < need) {
        size_t nc = e->cap_users ? e->cap_users * 2 : 4; while (nc < need) nc *= 2;
        char **nu = (char **)realloc(e->users, nc * sizeof(char *)); if (!nu) return 0; e->users = nu; e->cap_users = nc;
        char *nm = (char *)realloc(e->modes, nc * sizeof(char)); if (!nm) return 0; e->modes = nm;
    }
    e->users[e->n_users] = strdup(user); if (!e->users[e->n_users]) return 0;
    e->modes[e->n_users] = (mode=='W'?'W':'R');
    e->n_users++;
    return 1;
}

size_t nm_state_list_requests(const char *file, char users[][128], char modes[], size_t max_users) {
    struct req_entry *e = find_req_entry(file);
    if (!e) return 0;
    size_t c = e->n_users < max_users ? e->n_users : max_users;
    for (size_t i=0;i<c;i++) { snprintf(users[i], 128, "%s", e->users[i]); modes[i] = e->modes ? e->modes[i] : 'R'; }
    return c;
}

int nm_state_remove_request(const char *file, const char *user) {
    struct req_entry *e = find_req_entry(file);
    if (!e) return 0;
    for (size_t i=0;i<e->n_users;i++) {
        if (strcmp(e->users[i], user)==0) {
            free(e->users[i]);
            if (i != e->n_users - 1) { e->users[i] = e->users[e->n_users - 1]; if (e->modes) e->modes[i] = e->modes[e->n_users - 1]; }
            e->n_users--;
            return 1;
        }
    }
    return 0;
}

int nm_state_clear_requests_for(const char *file) {
    struct req_entry *e = find_req_entry(file);
    if (!e) return 0;
    
    // Get index for swap-with-last
    int found = 0;
    size_t index = req_map_find(file, &found);
    if (!found) return 0;
    
    for (size_t i=0;i<e->n_users;i++) if (e->users[i]) free(e->users[i]);
    free(e->users); e->users=NULL; e->n_users=0; e->cap_users=0;
    if (e->modes) { free(e->modes); e->modes=NULL; }
    
    // Remove from hash map
    req_map_remove(file);
    
    // remove entry itself
    free(e->file);
    if (e != &g_state.requests[g_state.n_requests-1]) {
        *e = g_state.requests[g_state.n_requests-1];
        // Update hash map index for swapped element
        if (index != g_state.n_requests - 1) {
            req_map_update_index(g_state.requests[index].file, index);
        }
    }
    g_state.n_requests--;
    return 1;
}

static void parse_requests_object(const char *json) {
    const char *p = strstr(json, "\"requests\"");
    if (!p) return;
    p = strchr(p, '{'); if (!p) return; p++;
    while (*p) {
        while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
        if (*p=='}') break;
        if (*p!='"') break;
        p++;
        const char *kstart=p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
        size_t klen=(size_t)(p-kstart); char file[256]; size_t j=0; const char *s=kstart; while(j<klen && *s){ if(*s=='\\'&&s[1]){s++; file[j++]=*s++;} else file[j++]=*s++; } file[j]='\0'; if (*p=='"') p++;
    while (*p && *p!='[') p++;
    if (*p!='[') break;
    p++;
        while (*p && *p!=']') {
            while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
            if (*p=='{') {
                // New object format: {"user":"...","mode":"R|W"}
                p++;
                char user[128]={0}; char mode='R';
                while (*p && *p!='}') {
                    while(*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                    if (*p=='"') {
                        p++; const char *k=p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
                        size_t klen=(size_t)(p-k); char key[16]; size_t ki=0; const char *ks=k; while(ki<klen&&*ks){ if(*ks=='\\'&&ks[1]){ks++; key[ki++]=*ks++;} else key[ki++]=*ks++; } key[ki]='\0'; if (*p=='"') p++;
                        while(*p && *p!=':') p++;
                        if (*p==':') p++;
                        while(*p==' '||*p=='\n'||*p=='\t') p++;
                        if (strcmp(key,"user")==0 && *p=='"') {
                            p++; const char *ust=p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
                            size_t ulen=(size_t)(p-ust); size_t ui=0; const char *us=ust; while(ui<ulen&&*us){ if(*us=='\\'&&us[1]){us++; user[ui++]=*us++;} else user[ui++]=*us++; } user[ui]='\0'; if (*p=='"') p++;
                        } else if (strcmp(key,"mode")==0 && *p=='"') {
                            p++; if (*p=='W'||*p=='R') mode=*p; while(*p && *p!='"') p++; if (*p=='"') p++;
                        } else {
                            // skip value
                            while(*p && *p!=',' && *p!='}') p++;
                        }
                    } else {
                        while(*p && *p!=',' && *p!='}') p++;
                    }
                    if (*p==',') p++;
                }
                if (*p=='}') p++;
                if (user[0]) nm_state_add_request(file, user, mode);
            } else if (*p=='"') {
                // Backward-compatible: old format was array of usernames
                p++; const char *ust=p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
                size_t ulen=(size_t)(p-ust); char user[128]; size_t ui=0; const char *us=ust; while(ui<ulen&&*us){ if(*us=='\\'&&us[1]){us++; user[ui++]=*us++;} else user[ui++]=*us++; } user[ui]='\0'; if (*p=='"') p++;
                nm_state_add_request(file, user, 'R');
            }
            while (*p && *p!=',' && *p!=']') p++;
            if (*p==',') p++;
        }
        if (*p==']') p++;
        while (*p && *p!=',' && *p!='}') p++;
        if (*p==',') p++;
    }
}

