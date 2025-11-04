#define _POSIX_C_SOURCE 200809L
#include "nm_persist.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct acl_user { char *user; int perm; };

typedef struct {
    char **users;
    size_t n_users;
    size_t cap_users;
    struct dir_entry { char *file; int ss_id; int *replicas; size_t n_repl; size_t cap_repl; } *dir;
    size_t n_dir;
    size_t cap_dir;
    struct acl_entry { char *file; char *owner; struct acl_user *grants; size_t n_grants; size_t cap_grants; } *acls;
    size_t n_acls;
    size_t cap_acls;
    // Folders list (logical prefixes)
    char **folders;
    size_t n_folders;
    size_t cap_folders;
    // Access requests per file
    struct req_entry { char *file; char **users; size_t n_users; size_t cap_users; } *requests;
    size_t n_requests;
    size_t cap_requests;
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

void nm_state_init(void) {
    memset(&g_state, 0, sizeof(g_state));
}

int nm_state_add_user(const char *user) {
    if (!user || !*user) return 0;
    for (size_t i = 0; i < g_state.n_users; ++i) {
        if (strcmp(g_state.users[i], user) == 0) return 0;
    }
    ensure_user_cap(g_state.n_users + 1);
    if (g_state.cap_users < g_state.n_users + 1) return 0; // failed to grow
    g_state.users[g_state.n_users] = strdup(user);
    if (!g_state.users[g_state.n_users]) return 0;
    g_state.n_users++;
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
    // Compose JSON: users, directory, acls, replicas, requests, folders
    size_t bufcap = 8192 + g_state.n_users * 64 + g_state.n_dir * 160 + g_state.n_acls * 320 + g_state.n_folders * 64;
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
        strcat(buf, "\":");
        char num[32]; snprintf(num, sizeof(num), "%d", g_state.dir[i].ss_id);
        strncat(buf, num, bufcap - strlen(buf) - 1);
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
            strcat(buf, "\"");
            const char *u = g_state.requests[i].users[j];
            for (; u && *u; ++u) { if (*u == '"' || *u == '\\') strncat(buf, "\\", bufcap - strlen(buf) - 1); char ch[2] = {*u,0}; strncat(buf, ch, bufcap - strlen(buf) - 1);}            
            strcat(buf, "\"");
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
        // read integer value
        const char *vstart = p;
        while (*p && *p != ',' && *p != '}') p++;
        char vbuf[32]; size_t vlen = (size_t)(p - vstart); if (vlen >= sizeof(vbuf)) vlen = sizeof(vbuf)-1;
        memcpy(vbuf, vstart, vlen); vbuf[vlen] = '\0';
        int ssid = atoi(vbuf);
        nm_state_set_dir(key, ssid);
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
    free(buf);
    return 0;
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
    for (size_t i=0;i<g_state.n_acls;i++){ if (strcmp(g_state.acls[i].file, file)==0) return &g_state.acls[i]; }
    return NULL;
}

static struct acl_entry* upsert_acl(const char *file){
    struct acl_entry *e = find_acl(file);
    if (e) return e;
    ensure_acl_cap(g_state.n_acls+1);
    if (g_state.cap_acls < g_state.n_acls+1) return NULL;
    e = &g_state.acls[g_state.n_acls++];
    memset(e, 0, sizeof(*e));
    e->file = strdup(file);
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

int nm_acl_check(const char *file, const char *user, const char *op) {
    if (!file || !user || !op) return -1;
    struct acl_entry *e = find_acl(file);
    // If no ACL entry exists, default allow for READ? Conservative deny.
    if (!e) return -1;
    if (e->owner && strcmp(e->owner, user)==0) return 0;
    int need;
    if (strcmp(op, "READ") == 0 || strcmp(op, "HISTORY") == 0 || strcmp(op, "VIEWCHECKPOINT") == 0 || strcmp(op, "LISTCHECKPOINTS") == 0) need = ACL_R; // read-like
    else need = ACL_W; // WRITE/UNDO/REVERT and others require W
    for (size_t i=0;i<e->n_grants;i++){ if (strcmp(e->grants[i].user, user)==0){ if ((e->grants[i].perm & need) == need) return 0; else return -1; }}
    return -1;
}

int nm_acl_rename(const char *old_file, const char *new_file) {
    if (!old_file || !*old_file || !new_file || !*new_file) return 0;
    struct acl_entry *e = find_acl(old_file);
    if (!e) return 0;
    // Ensure no destination exists
    if (find_acl(new_file)) return 0;
    free(e->file);
    e->file = strdup(new_file);
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
    for (size_t i = 0; i < g_state.n_folders; ++i) {
        if (strcmp(g_state.folders[i], path) == 0) return 1;
    }
    return 0;
}

int nm_state_add_folder(const char *path) {
    if (!path || !*path) return 0;
    if (folder_exists(path)) return 0;
    ensure_folder_cap(g_state.n_folders + 1);
    if (g_state.cap_folders < g_state.n_folders + 1) return 0;
    g_state.folders[g_state.n_folders] = strdup(path);
    if (!g_state.folders[g_state.n_folders]) return 0;
    g_state.n_folders++;
    return 1;
}

int nm_state_remove_folder(const char *path) {
    if (!path || !*path) return 0;
    for (size_t i = 0; i < g_state.n_folders; ++i) {
        if (strcmp(g_state.folders[i], path) == 0) {
            free(g_state.folders[i]);
            if (i != g_state.n_folders - 1) g_state.folders[i] = g_state.folders[g_state.n_folders - 1];
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
            free(g_state.folders[i]);
            g_state.folders[i] = strdup(new_path);
        } else if (strncmp(g_state.folders[i], old_path, oldlen) == 0 && g_state.folders[i][oldlen] == '/') {
            // nested folder
            const char *rest = g_state.folders[i] + oldlen;
            char buf[512]; snprintf(buf, sizeof(buf), "%s%s", new_path, rest);
            free(g_state.folders[i]);
            g_state.folders[i] = strdup(buf);
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
    for (size_t i=0;i<g_state.n_requests;i++) if (strcmp(g_state.requests[i].file, file)==0) return &g_state.requests[i];
    return NULL;
}

int nm_state_add_request(const char *file, const char *user) {
    if (!file || !user || !*user) return 0;
    if (nm_state_find_dir(file, NULL) != 0) return 0; // only for known files
    struct req_entry *e = find_req_entry(file);
    if (!e) {
        ensure_req_cap(g_state.n_requests + 1);
        if (g_state.cap_requests < g_state.n_requests + 1) return 0;
        e = &g_state.requests[g_state.n_requests++];
        memset(e, 0, sizeof(*e));
        e->file = strdup(file);
    }
    // check duplicate
    for (size_t i=0;i<e->n_users;i++) if (strcmp(e->users[i], user)==0) return 0;
    size_t need = e->n_users + 1;
    if (e->cap_users < need) {
        size_t nc = e->cap_users ? e->cap_users * 2 : 4; while (nc < need) nc *= 2;
        char **nu = (char **)realloc(e->users, nc * sizeof(char *)); if (!nu) return 0; e->users = nu; e->cap_users = nc;
    }
    e->users[e->n_users] = strdup(user); if (!e->users[e->n_users]) return 0; e->n_users++;
    return 1;
}

size_t nm_state_list_requests(const char *file, char users[][128], size_t max_users) {
    struct req_entry *e = find_req_entry(file);
    if (!e) return 0;
    size_t c = e->n_users < max_users ? e->n_users : max_users;
    for (size_t i=0;i<c;i++) snprintf(users[i], 128, "%s", e->users[i]);
    return c;
}

int nm_state_remove_request(const char *file, const char *user) {
    struct req_entry *e = find_req_entry(file);
    if (!e) return 0;
    for (size_t i=0;i<e->n_users;i++) {
        if (strcmp(e->users[i], user)==0) {
            free(e->users[i]);
            if (i != e->n_users - 1) e->users[i] = e->users[e->n_users - 1];
            e->n_users--;
            return 1;
        }
    }
    return 0;
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
            if (*p=='"') {
                p++; const char *ust=p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
                size_t ulen=(size_t)(p-ust); char user[128]; size_t ui=0; const char *us=ust; while(ui<ulen&&*us){ if(*us=='\\'&&us[1]){us++; user[ui++]=*us++;} else user[ui++]=*us++; } user[ui]='\0'; if (*p=='"') p++;
                nm_state_add_request(file, user);
            }
            while (*p && *p!=',' && *p!=']') p++;
            if (*p==',') p++;
        }
        if (*p==']') p++;
        while (*p && *p!=',' && *p!='}') p++;
        if (*p==',') p++;
    }
}

