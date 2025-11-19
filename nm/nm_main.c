#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>

#include "../common/net_proto.h"
#include "nm_persist.h"
#include "nm_dir.h"
#include "../common/tickets.h"
#include <errno.h>

#define BACKLOG 64

static volatile int g_running = 1;

typedef struct ss_entry {
    int ss_id;
    int ss_ctrl_port;
    int ss_data_port;
    char ss_addr[64];
    time_t last_heartbeat;
    int is_up;
    struct ss_entry *next;
} ss_entry_t;

static ss_entry_t *g_ss_list = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void add_ss(int id, int ctrl, int data, const char *addr) {
    pthread_mutex_lock(&g_mu);
    ss_entry_t *e = (ss_entry_t *)malloc(sizeof(ss_entry_t));
    e->ss_id = id; e->ss_ctrl_port = ctrl; e->ss_data_port = data;
    snprintf(e->ss_addr, sizeof(e->ss_addr), "%s", addr);
    e->last_heartbeat = time(NULL); e->is_up = 1; e->next = g_ss_list; g_ss_list = e;
    pthread_mutex_unlock(&g_mu);
}

static ss_entry_t *find_ss_nolock(int id) {
    for (ss_entry_t *e = g_ss_list; e; e = e->next) if (e->ss_id == id) return e;
    return NULL;
}

static int get_ss_info(int ssid, int *out_port, char *out_addr, size_t addr_sz) {
    pthread_mutex_lock(&g_mu);
    ss_entry_t *e = find_ss_nolock(ssid);
    if (e) {
        if (out_port) *out_port = e->ss_data_port;
        if (out_addr && addr_sz > 0) snprintf(out_addr, addr_sz, "%s", e->ss_addr);
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    pthread_mutex_unlock(&g_mu);
    return -1;
}

// Replication queue metric
static pthread_mutex_t g_rep_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_replication_queue = 0; // pending/in-flight tasks

static void repq_inc(int delta) { pthread_mutex_lock(&g_rep_mu); g_replication_queue += delta; if (g_replication_queue < 0) g_replication_queue = 0; pthread_mutex_unlock(&g_rep_mu); }
static int repq_get(void) { pthread_mutex_lock(&g_rep_mu); int v = g_replication_queue; pthread_mutex_unlock(&g_rep_mu); return v; }

// Minimal JSON string unescape for EXEC script bodies
static void json_unescape_inplace(char *str) {
    if (!str) return;
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '\\') {
            src++;
            if (!*src) break;
            switch (*src) {
                case 'n': *dst++ = '\n'; break;
                case 'r': *dst++ = '\r'; break;
                case 't': *dst++ = '\t'; break;
                case '\\': *dst++ = '\\'; break;
                case '"': *dst++ = '"'; break;
                default:   *dst++ = *src; break;
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Helper: fetch whole file text from a given SS (by ssid) using READ ticket
static int fetch_file_from_ss(const char *file, int ssid, char *out_body, size_t out_sz) {
    int data_port = 0; char ss_addr[64];
    if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port == 0) return -1;
    char ticket[256]; if (ticket_build(file, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) return -1;
    int sfd = tcp_connect(ss_addr, (uint16_t)data_port); if (sfd < 0) return -1;
    char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "READ", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
    if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); return -1; }
    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { if (r) free(r); close(sfd); return -1; }
    int ok = (json_get_string_field(r, "body", out_body, out_sz) == 0);
    free(r); close(sfd);
    return ok ? 0 : -1;
}

// Helper: fetch checkpoint content from a given SS by name using VIEWCHECKPOINT ticket
static int fetch_checkpoint_from_ss(const char *file, const char *cpname, int ssid, char *out_body, size_t out_sz) {
    int data_port = 0; char ss_addr[64];
    if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port == 0) return -1;
    char ticket[256]; if (ticket_build(file, "VIEWCHECKPOINT", ssid, 600, ticket, sizeof(ticket)) != 0) return -1;
    int sfd = tcp_connect(ss_addr, (uint16_t)data_port); if (sfd < 0) return -1;
    char req[512]; req[0]='\0';
    json_put_string_field(req, sizeof(req), "type", "VIEWCHECKPOINT", 1);
    json_put_string_field(req, sizeof(req), "file", file, 0);
    json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
    json_put_string_field(req, sizeof(req), "name", cpname, 0);
    strncat(req, "}", sizeof(req)-strlen(req)-1);
    if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); return -1; }
    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { if (r) free(r); close(sfd); return -1; }
    int ok = (json_get_string_field(r, "body", out_body, out_sz) == 0);
    free(r); close(sfd);
    return ok ? 0 : -1;
}

// Fire-and-forget PUT replicate to a target ssid (fetches from primary)
typedef struct { char file[128]; int primary_ssid; int target_ssid; } repl_put_args_t;
static void *repl_put_thread(void *arg) {
    repl_put_args_t *a = (repl_put_args_t *)arg;
    char body[8192]; body[0]='\0';
    if (fetch_file_from_ss(a->file, a->primary_ssid, body, sizeof(body)) == 0) {
        int dport = 0; char dest_addr[64];
        if (get_ss_info(a->target_ssid, &dport, dest_addr, sizeof(dest_addr)) == 0 && dport) {
            int dfd = tcp_connect(dest_addr, (uint16_t)dport);
            if (dfd >= 0) {
                char req[9216]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "PUT", 1); json_put_string_field(req, sizeof(req), "file", a->file, 0); json_put_string_field(req, sizeof(req), "body", body, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                (void)send_msg(dfd, req, (uint32_t)strlen(req)); char *rr=NULL; uint32_t rrl=0; (void)recv_msg(dfd, &rr, &rrl); if (rr) free(rr); close(dfd);
                fprintf(stderr, "[NM] Replicated PUT %s -> ss%d\n", a->file, a->target_ssid);
            }
        }
    }
    repq_inc(-1);
    free(a);
    return NULL;
}

static void schedule_put_repl(const char *file, int primary_ssid, int target_ssid) {
    repl_put_args_t *a = (repl_put_args_t *)malloc(sizeof(*a)); if (!a) return;
    snprintf(a->file, sizeof(a->file), "%s", file); a->primary_ssid = primary_ssid; a->target_ssid = target_ssid;
    pthread_t th; repq_inc(1); pthread_create(&th, NULL, repl_put_thread, a); pthread_detach(th);
}

// Fire-and-forget checkpoint replicate to a target ssid (fetches from primary)
typedef struct { char file[128]; char name[256]; int primary_ssid; int target_ssid; } repl_cp_args_t;
static void *repl_checkpoint_thread(void *arg) {
    repl_cp_args_t *a = (repl_cp_args_t *)arg;
    char body[8192]; body[0]='\0';
    if (fetch_checkpoint_from_ss(a->file, a->name, a->primary_ssid, body, sizeof(body)) == 0) {
        int dport = 0; char dest_addr[64];
        if (get_ss_info(a->target_ssid, &dport, dest_addr, sizeof(dest_addr)) == 0 && dport) {
            int dfd = tcp_connect(dest_addr, (uint16_t)dport);
            if (dfd >= 0) {
                char req[9216]; req[0]='\0';
                json_put_string_field(req, sizeof(req), "type", "PUT_CHECKPOINT", 1);
                json_put_string_field(req, sizeof(req), "file", a->file, 0);
                json_put_string_field(req, sizeof(req), "name", a->name, 0);
                json_put_string_field(req, sizeof(req), "body", body, 0);
                strncat(req, "}", sizeof(req)-strlen(req)-1);
                (void)send_msg(dfd, req, (uint32_t)strlen(req)); char *rr=NULL; uint32_t rrl=0; (void)recv_msg(dfd, &rr, &rrl); if (rr) free(rr); close(dfd);
                fprintf(stderr, "[NM] Replicated CHECKPOINT %s@%s -> ss%d\n", a->file, a->name, a->target_ssid);
            }
        }
    }
    repq_inc(-1);
    free(a);
    return NULL;
}

static void schedule_checkpoint_repl(const char *file, const char *name, int primary_ssid, int target_ssid) {
    repl_cp_args_t *a = (repl_cp_args_t *)malloc(sizeof(*a)); if (!a) return;
    snprintf(a->file, sizeof(a->file), "%s", file); snprintf(a->name, sizeof(a->name), "%s", name); a->primary_ssid = primary_ssid; a->target_ssid = target_ssid;
    pthread_t th; repq_inc(1); pthread_create(&th, NULL, repl_checkpoint_thread, a); pthread_detach(th);
}

// Fire-and-forget undo replicate to a target ssid (fetches from primary)
typedef struct { char file[128]; int primary_ssid; int target_ssid; } repl_undo_args_t;
static void *repl_undo_thread(void *arg) {
    repl_undo_args_t *a = (repl_undo_args_t *)arg;
    // Fetch undo from primary using READ ticket (undo is just a file)
    int pport = 0; char paddr[64];
    if (get_ss_info(a->primary_ssid, &pport, paddr, sizeof(paddr)) == 0 && pport) {
        char ticket[256]; 
        if (ticket_build(a->file, "READ", a->primary_ssid, 600, ticket, sizeof(ticket)) == 0) {
            int sfd = tcp_connect(paddr, (uint16_t)pport);
            if (sfd >= 0) {
                // Build request to read undo file directly
                char undo_file[256]; snprintf(undo_file, sizeof(undo_file), "../undo/%s.undo", a->file);
                char req[512]; req[0]='\0';
                json_put_string_field(req, sizeof(req), "type", "READ", 1);
                json_put_string_field(req, sizeof(req), "file", undo_file, 0);
                json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
                strncat(req, "}", sizeof(req)-strlen(req)-1);
                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                    char *resp = NULL; uint32_t rl = 0;
                    if (recv_msg(sfd, &resp, &rl) == 0 && resp) {
                        if (strstr(resp, "\"status\":\"OK\"")) {
                            char body[8192]; body[0] = '\0';
                            if (json_get_string_field(resp, "body", body, sizeof(body)) == 0) {
                                // Now send to target SS
                                int dport = 0; char dest_addr[64];
                                if (get_ss_info(a->target_ssid, &dport, dest_addr, sizeof(dest_addr)) == 0 && dport) {
                                    int dfd = tcp_connect(dest_addr, (uint16_t)dport);
                                    if (dfd >= 0) {
                                        char put_req[9216]; put_req[0]='\0';
                                        json_put_string_field(put_req, sizeof(put_req), "type", "PUT_UNDO", 1);
                                        json_put_string_field(put_req, sizeof(put_req), "file", a->file, 0);
                                        json_put_string_field(put_req, sizeof(put_req), "body", body, 0);
                                        strncat(put_req, "}", sizeof(put_req)-strlen(put_req)-1);
                                        (void)send_msg(dfd, put_req, (uint32_t)strlen(put_req));
                                        char *rr=NULL; uint32_t rrl=0; (void)recv_msg(dfd, &rr, &rrl);
                                        if (rr) free(rr);
                                        close(dfd);
                                        fprintf(stderr, "[NM] Replicated UNDO %s -> ss%d\n", a->file, a->target_ssid);
                                    }
                                }
                            }
                        }
                        free(resp);
                    }
                }
                close(sfd);
            }
        }
    }
    repq_inc(-1);
    free(a);
    return NULL;
}

static void schedule_undo_repl(const char *file, int primary_ssid, int target_ssid) {
    repl_undo_args_t *a = (repl_undo_args_t *)malloc(sizeof(*a)); if (!a) return;
    snprintf(a->file, sizeof(a->file), "%s", file); a->primary_ssid = primary_ssid; a->target_ssid = target_ssid;
    pthread_t th; repq_inc(1); pthread_create(&th, NULL, repl_undo_thread, a); pthread_detach(th);
}

// Fire-and-forget simple command replicate (CREATE/DELETE/RENAME)
typedef struct { char type[16]; char file[128]; char newfile[128]; int target_ssid; } repl_cmd_args_t;
static void *repl_cmd_thread(void *arg) {
    repl_cmd_args_t *a = (repl_cmd_args_t*)arg;
    int dport = 0; char dest_addr[64];
    if (get_ss_info(a->target_ssid, &dport, dest_addr, sizeof(dest_addr)) == 0 && dport) {
        int dfd = tcp_connect(dest_addr, (uint16_t)dport);
        if (dfd >= 0) {
            char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", a->type, 1); json_put_string_field(req, sizeof(req), "file", a->file, 0); if (strcmp(a->type, "RENAME")==0) json_put_string_field(req, sizeof(req), "newFile", a->newfile, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
            (void)send_msg(dfd, req, (uint32_t)strlen(req)); char *r=NULL; uint32_t rl=0; (void)recv_msg(dfd, &r, &rl); if (r) free(r); close(dfd);
            fprintf(stderr, "[NM] Replicated %s %s -> ss%d\n", a->type, a->file, a->target_ssid);
        }
    }
    repq_inc(-1);
    free(a);
    return NULL;
}

static void schedule_cmd_repl(const char *type, const char *file, const char *newfile, int target_ssid) {
    repl_cmd_args_t *a = (repl_cmd_args_t*)malloc(sizeof(*a)); if (!a) return;
    snprintf(a->type, sizeof(a->type), "%s", type); snprintf(a->file, sizeof(a->file), "%s", file); a->newfile[0]='\0'; if (newfile) snprintf(a->newfile, sizeof(a->newfile), "%s", newfile); a->target_ssid = target_ssid;
    pthread_t th; repq_inc(1); pthread_create(&th, NULL, repl_cmd_thread, a); pthread_detach(th);
}

// Background thread: mark SS down if heartbeat stale and promote replicas
static void *hb_monitor_thread(void *arg) {
    (void)arg;
    while (g_running) {
        time_t now = time(NULL);
        pthread_mutex_lock(&g_mu);
        for (ss_entry_t *e = g_ss_list; e; e = e->next) {
            int was_up = e->is_up;
            if (now - e->last_heartbeat > 6) e->is_up = 0; else e->is_up = 1;
            if (was_up && !e->is_up) {
                fprintf(stderr, "[NM] SS %d marked DOWN\n", e->ss_id);
            }
        }
        pthread_mutex_unlock(&g_mu);
        // Promote primaries whose SS is down
        char files[512][128]; int ssids[512]; size_t n = nm_state_get_dir(files, ssids, 512);
        for (size_t i=0;i<n;i++) {
            int primary = ssids[i];
            int up = 0; pthread_mutex_lock(&g_mu); ss_entry_t *e = find_ss_nolock(primary); if (e && e->is_up) up = 1; pthread_mutex_unlock(&g_mu);
            if (!up) {
                int repls[16]; size_t nr = nm_state_get_replicas(files[i], repls, 16);
                int promoted = 0;
                for (size_t j=0;j<nr;j++) {
                    int cand = repls[j]; int cand_up=0; pthread_mutex_lock(&g_mu); ss_entry_t *ce = find_ss_nolock(cand); if (ce && ce->is_up) cand_up=1; pthread_mutex_unlock(&g_mu);
                    if (cand_up) {
                        // Promote candidate and ensure old primary becomes a replica
                        nm_dir_set(files[i], cand);
                        int new_reps[16]; size_t nnr = 0;
                        // Include old primary as replica
                        new_reps[nnr++] = primary;
                        // Keep other replicas except the new primary and duplicates
                        for (size_t k=0;k<nr && nnr<16;k++) {
                            if (repls[k] == cand || repls[k] == primary) continue;
                            new_reps[nnr++] = repls[k];
                        }
                        nm_state_set_replicas(files[i], new_reps, nnr);
                        promoted = 1; fprintf(stderr, "[NM] Promoted %s primary -> ss%d; old primary %d set as replica\n", files[i], cand, primary);
                        break;
                    }
                }
                if (promoted) (void)nm_state_save("nm_state.json");
            }
        }
        sleep(1);
    }
    return NULL;
}

static int pick_least_loaded_ss(int *out_ssid, int *out_data_port, char *out_addr, size_t addr_sz) {
    // Count mappings per SS
    int counts[1024]; int ssids[1024]; int nss=0;
    pthread_mutex_lock(&g_mu);
    for (ss_entry_t *e = g_ss_list; e; e = e->next) { ssids[nss]=e->ss_id; counts[nss]=0; nss++; if (nss>=1024) break; }
    pthread_mutex_unlock(&g_mu);
    // Tally from directory
    char files[512][128]; int ssmap[512]; size_t n = nm_state_get_dir(files, ssmap, 512);
    for (size_t i=0; i<n; ++i) {
        for (int j=0; j<nss; ++j) if (ssmap[i] == ssids[j]) { counts[j]++; break; }
    }
    // Choose min count (tie: first)
    if (nss == 0) return -1;
    int best = 0; for (int j=1; j<nss; ++j) if (counts[j] < counts[best]) best = j;
    int chosen_ssid = ssids[best];
    int data_port = 0; char ss_addr[64] = "127.0.0.1";
    pthread_mutex_lock(&g_mu);
    for (ss_entry_t *e = g_ss_list; e; e=e->next) {
        if (e->ss_id==chosen_ssid) { data_port = e->ss_data_port; snprintf(ss_addr, sizeof(ss_addr), "%s", e->ss_addr); break; }
    }
    pthread_mutex_unlock(&g_mu);
    if (data_port==0) return -1;
    if (out_ssid) *out_ssid = chosen_ssid;
    if (out_data_port) *out_data_port = data_port;
    if (out_addr && addr_sz > 0) snprintf(out_addr, addr_sz, "%s", ss_addr);
    return 0;
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    while (g_running) {
        char *buf = NULL; uint32_t len = 0;
        if (recv_msg(fd, &buf, &len) < 0) break;
        if (len == 0) { free(buf); break; }
        // Parse minimal JSON
        char type[64];
        if (json_get_string_field(buf, "type", type, sizeof(type)) < 0) {
            fprintf(stderr, "[NM] Bad request: missing type\n");
            const char *resp = "{\"status\":\"ERR_BADREQ\"}";
            send_msg(fd, resp, (uint32_t)strlen(resp));
            free(buf); continue;
        }
        if (strcmp(type, "SS_REGISTER") == 0) {
            int ssId=0, ctrl=0, data=0;
            json_get_int_field(buf, "ssId", &ssId);
            json_get_int_field(buf, "ssCtrlPort", &ctrl);
            json_get_int_field(buf, "ssDataPort", &data);
            // Get client IP from socket
            struct sockaddr_in peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            char ss_ip[64] = "127.0.0.1";
            if (getpeername(fd, (struct sockaddr*)&peer_addr, &peer_len) == 0) {
                inet_ntop(AF_INET, &peer_addr.sin_addr, ss_ip, sizeof(ss_ip));
            }
            add_ss(ssId, ctrl, data, ss_ip);
            printf("[NM] Registered SS id=%d ctrl=%d data=%d addr=%s\n", ssId, ctrl, data, ss_ip);
            
            // Immediately resync files where this SS is a replica (from saved state)
            fprintf(stderr, "[NM] SS %d registered, checking for replicas to resync\n", ssId);
            char files[512][128]; int ps[512]; size_t n = nm_state_get_dir(files, ps, 512);
            for (size_t i=0;i<n;i++) {
                int repls[16]; size_t nr = nm_state_get_replicas(files[i], repls, 16);
                for (size_t j=0;j<nr;j++) if (repls[j]==ssId) {
                    fprintf(stderr, "[NM] Resyncing file %s to newly registered ss%d\n", files[i], ssId);
                    // Resync file content
                    schedule_put_repl(files[i], ps[i], ssId);
                    
                    // Resync undo snapshot (if exists on primary)
                    schedule_undo_repl(files[i], ps[i], ssId);
                    
                    // Best-effort: also resync checkpoints list and push each
                    int pport=0; char paddr[64];
                    if (get_ss_info(ps[i], &pport, paddr, sizeof(paddr)) == 0 && pport) {
                        char ticket[256]; if (ticket_build(files[i], "LISTCHECKPOINTS", ps[i], 600, ticket, sizeof(ticket)) == 0) {
                            int sfd = tcp_connect(paddr, (uint16_t)pport);
                            if (sfd >= 0) {
                                char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "LISTCHECKPOINTS", 1); json_put_string_field(req, sizeof(req), "file", files[i], 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) == 0 && r && strstr(r, "\"status\":\"OK\"")) {
                                        const char *p = strchr(r, '['); if (p) {
                                            p++;
                                            while (*p && *p!=']') {
                                                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                                                if (*p=='"') {
                                                    p++; const char *s=p; while(*p && *p!='"') p++; size_t nlen=(size_t)(p-s);
                                                    char name[256]; if (nlen>=sizeof(name)) nlen=sizeof(name)-1; memcpy(name, s, nlen); name[nlen]='\0';
                                                    if (*p=='"') p++;
                                                    schedule_checkpoint_repl(files[i], name, ps[i], ssId);
                                                } else break;
                                            }
                                        }
                                    }
                                    if (r) free(r);
                                }
                                close(sfd);
                            }
                        }
                    }
                }
            }
            
            const char *resp = "{\"status\":\"OK\"}";
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "SS_HEARTBEAT") == 0) {
            int ssId=0; json_get_int_field(buf, "ssId", &ssId);
            pthread_mutex_lock(&g_mu);
            ss_entry_t *e = find_ss_nolock(ssId);
            if (!e) {
                // Unknown server; add with unknown ports (will not be considered UP until it REGISTERs with ports)
                struct sockaddr_in peer_addr; socklen_t peer_len = sizeof(peer_addr); char ss_ip[64] = "127.0.0.1";
                if (getpeername(fd, (struct sockaddr*)&peer_addr, &peer_len) == 0) { inet_ntop(AF_INET, &peer_addr.sin_addr, ss_ip, sizeof(ss_ip)); }
                e = (ss_entry_t *)malloc(sizeof(ss_entry_t)); memset(e,0,sizeof(*e)); e->ss_id=ssId; e->ss_ctrl_port=0; e->ss_data_port=0; snprintf(e->ss_addr, sizeof(e->ss_addr), "%s", ss_ip); e->next=g_ss_list; g_ss_list=e;
            }
            int was_up = e->is_up;
            e->last_heartbeat = time(NULL);
            // Only mark as UP if we know its data port (i.e., it REGISTERed before/after heartbeat)
            e->is_up = (e->ss_data_port != 0);
            pthread_mutex_unlock(&g_mu);
            if (!was_up && e->is_up) {
                fprintf(stderr, "[NM] SS %d transitioned UP\n", ssId);
                // Resync files where this SS is a replica
                char files[512][128]; int ps[512]; size_t n = nm_state_get_dir(files, ps, 512);
                for (size_t i=0;i<n;i++) {
                    int repls[16]; size_t nr = nm_state_get_replicas(files[i], repls, 16);
                    for (size_t j=0;j<nr;j++) if (repls[j]==ssId) {
                        // Resync file content
                        schedule_put_repl(files[i], ps[i], ssId);
                        
                        // Resync undo snapshot (if exists on primary)
                        schedule_undo_repl(files[i], ps[i], ssId);
                        
                        // Best-effort: also resync checkpoints list and push each
                        // Fetch checkpoint names from primary
                        int pport=0; char paddr[64];
                        if (get_ss_info(ps[i], &pport, paddr, sizeof(paddr)) == 0 && pport) {
                            char ticket[256]; if (ticket_build(files[i], "LISTCHECKPOINTS", ps[i], 600, ticket, sizeof(ticket)) == 0) {
                                int sfd = tcp_connect(paddr, (uint16_t)pport);
                                if (sfd >= 0) {
                                    char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "LISTCHECKPOINTS", 1); json_put_string_field(req, sizeof(req), "file", files[i], 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                    if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) == 0 && r && strstr(r, "\"status\":\"OK\"")) {
                                            // Parse simple list of checkpoint names
                                            const char *p = strchr(r, '['); if (p) {
                                                p++;
                                                while (*p && *p!=']') {
                                                    while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                                                    if (*p=='"') {
                                                        p++; const char *s=p; while(*p && *p!='"') p++; size_t nlen=(size_t)(p-s);
                                                        char name[256]; if (nlen>=sizeof(name)) nlen=sizeof(name)-1; memcpy(name, s, nlen); name[nlen]='\0';
                                                        if (*p=='"') p++;
                                                        schedule_checkpoint_repl(files[i], name, ps[i], ssId);
                                                    } else break;
                                                }
                                            }
                                        }
                                        if (r) free(r);
                                    }
                                    close(sfd);
                                }
                            }
                        }
                    }
                }
            }
            const char *resp = "{\"status\":\"OK\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "SS_COMMIT") == 0) {
            char file[128]; int ssId=0; int okf=(json_get_string_field(buf, "file", file, sizeof(file))==0); json_get_int_field(buf, "ssId", &ssId);
            if (!okf || ssId==0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                int primary=0; if (nm_state_find_dir(file, &primary)==0 && primary==ssId) {
                    int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
                    for (size_t i=0;i<nr;i++) schedule_put_repl(file, primary, repls[i]);
                }
                const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
            }
        } else if (strcmp(type, "SS_CHECKPOINT") == 0) {
            // Primary created a checkpoint; replicate it to replicas
            char file[128]; char name[256]; int ssId=0;
            int okf = (json_get_string_field(buf, "file", file, sizeof(file))==0);
            (void)json_get_string_field(buf, "name", name, sizeof(name));
            (void)json_get_int_field(buf, "ssId", &ssId);
            if (!okf || !name[0] || ssId==0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                int primary=0; if (nm_state_find_dir(file, &primary)==0 && primary==ssId) {
                    int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
                    for (size_t i=0;i<nr;i++) schedule_checkpoint_repl(file, name, primary, repls[i]);
                }
                const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
            }
    } else if (strcmp(type, "LOOKUP") == 0) {
            // LOOKUP for READ/WRITE and other ops that require tickets
            char op[32]; char file[128]; char user[128]; user[0]='\0';
            int have_op = (json_get_string_field(buf, "op", op, sizeof(op)) == 0);
            int have_file = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
            (void)json_get_string_field(buf, "user", user, sizeof(user));
            if (!user[0]) snprintf(user, sizeof(user), "%s", "anonymous");
            fprintf(stderr, "[NM] LOOKUP op=%s file=%s have_op=%d have_file=%d\n", have_op?op:"?", have_file?file:"?", have_op, have_file);
            if (!have_op || !have_file || (strcmp(op, "READ") != 0 && strcmp(op, "WRITE") != 0 && strcmp(op, "UNDO") != 0 && strcmp(op, "REVERT") != 0 && strcmp(op, "CHECKPOINT") != 0 && strcmp(op, "VIEWCHECKPOINT") != 0 && strcmp(op, "LISTCHECKPOINTS") != 0)) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}";
                send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int ssid = 0;
                if (nm_state_find_dir(file, &ssid) != 0) {
                    if (strcmp(op, "WRITE") == 0) {
                        // Auto-provision mapping on first WRITE
                        int data_port = 0; int chosen_ssid = 0; char ss_addr[64];
                        (void)pick_least_loaded_ss(&chosen_ssid, &data_port, ss_addr, sizeof(ss_addr));
                        fprintf(stderr, "[NM] LOOKUP WRITE auto-provision chosen_ssid=%d data_port=%d\n", chosen_ssid, data_port);
                        if (data_port == 0) {
                            const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                        } else {
                            int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                            if (sfd >= 0) {
                                char req[256]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "CREATE", 1); json_put_string_field(req, sizeof(req), "file", file, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl)==0 && r && strstr(r, "\"status\":\"OK\"")) {
                                        nm_dir_set(file, chosen_ssid); nm_acl_set_owner(file, user); nm_acl_grant(file, user, ACL_R|ACL_W);
                                        
                                        // Initialize metadata: set creator and creation time
                                        int now = (int)time(NULL);
                                        nm_state_set_file_modified(file, user, now);
                                        nm_state_set_file_accessed(file, user, now);
                                        
                                        // Set up replicas for auto-provisioned file
                                        int replicas[16]; size_t nr = 0;
                                        pthread_mutex_lock(&g_mu);
                                        for (ss_entry_t *e = g_ss_list; e && nr < 16; e = e->next) {
                                            if (e->ss_id != chosen_ssid && e->is_up && e->ss_data_port > 0) {
                                                replicas[nr++] = e->ss_id;
                                                if (nr >= 1) break; // Limit to 1 replica
                                            }
                                        }
                                        pthread_mutex_unlock(&g_mu);
                                        if (nr > 0) {
                                            nm_state_set_replicas(file, replicas, nr);
                                            for (size_t i = 0; i < nr; i++) {
                                                schedule_cmd_repl("CREATE", file, NULL, replicas[i]);
                                            }
                                        }
                                        
                                        (void)nm_state_save("nm_state.json");
                                    }
                                    if (r) free(r);
                                }
                                close(sfd);
                            }
                            // After creation attempt, build ticket
                            int ssid_created=0; if (nm_state_find_dir(file, &ssid_created)==0) {
                                char ticket2[256]; if (ticket_build(file, op, ssid_created, 600, ticket2, sizeof(ticket2))==0) {
                                    int dport2=0; char ss_addr2[64];
                                    if (get_ss_info(ssid_created, &dport2, ss_addr2, sizeof(ss_addr2)) == 0 && dport2) {
                                        char resp2[512]; snprintf(resp2,sizeof(resp2),"{\"status\":\"OK\",\"ssAddr\":\"%s\",\"ssDataPort\":%d,\"ticket\":\"%s\"}", ss_addr2, dport2, ticket2); send_msg(fd, resp2, (uint32_t)strlen(resp2));
                                    }
                                    else { const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                } else { const char *er="{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            } else { const char *er="{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        }
                    } else {
                        const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                    }
                } else {
                    // File exists: build ticket
                    if ((strcmp(op, "READ") == 0 && nm_acl_check(file, user, "READ") != 0) || (strcmp(op, "WRITE") == 0 && nm_acl_check(file, user, "WRITE") != 0)) {
                        const char *er = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                    } else {
                        // Update metadata: track access time for READ, modification time for WRITE
                        int now = (int)time(NULL);
                        if (strcmp(op, "READ") == 0) {
                            nm_state_set_file_accessed(file, user, now);
                        } else if (strcmp(op, "WRITE") == 0) {
                            nm_state_set_file_modified(file, user, now);
                        }
                        (void)nm_state_save("nm_state.json");
                        
                        char ticket2[256]; if (ticket_build(file, op, ssid, 600, ticket2, sizeof(ticket2)) != 0) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else { int dport2=0; char ss_addr2[64];
                            if (get_ss_info(ssid, &dport2, ss_addr2, sizeof(ss_addr2)) == 0 && dport2) {
                                char resp2[512]; snprintf(resp2,sizeof(resp2),"{\"status\":\"OK\",\"ssAddr\":\"%s\",\"ssDataPort\":%d,\"ticket\":\"%s\"}", ss_addr2, dport2, ticket2); send_msg(fd, resp2, (uint32_t)strlen(resp2));
                            } else { const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); } }
                    }
                }
            }
        } else if (strcmp(type, "CREATE") == 0) {
            // Explicit CREATE: create empty file mapping and optional public ACL flags
            char file[128]; char user[128]; user[0]='\0'; int pubR=0, pubW=0;
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
            (void)json_get_int_field(buf, "publicRead", &pubR); (void)json_get_int_field(buf, "publicWrite", &pubW);
            if (!okf) { const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                // conflict check
                if (nm_state_find_dir(file, NULL) == 0) { const char *er = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    // pick least loaded SS
                    int chosen_ssid=0, data_port=0; char ss_addr[64];
                    if (pick_least_loaded_ss(&chosen_ssid, &data_port, ss_addr, sizeof(ss_addr))!=0 || data_port==0) { const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                    else {
                        int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                        if (sfd < 0) { const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else {
                            char req[256]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "CREATE", 1); json_put_string_field(req, sizeof(req), "file", file, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                            if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            else {
                                char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl)==0 && r && strstr(r, "\"status\":\"OK\"")) {
                                    nm_dir_set(file, chosen_ssid); nm_acl_set_owner(file, user); nm_acl_grant(file, user, ACL_R|ACL_W);
                                    if (pubR || pubW) { int anonPerm=0; if (pubR) anonPerm |= ACL_R; if (pubW) anonPerm |= (ACL_R|ACL_W); if (anonPerm) nm_acl_grant(file, "anonymous", anonPerm); }
                                    
                                    // Initialize metadata: set creator and creation time
                                    int now = (int)time(NULL);
                                    nm_state_set_file_modified(file, user, now);
                                    nm_state_set_file_accessed(file, user, now);
                                    
                                    // Set up replicas: pick other available SS as replica (1 replica only)
                                    int replicas[16]; size_t nr = 0;
                                    pthread_mutex_lock(&g_mu);
                                    for (ss_entry_t *e = g_ss_list; e && nr < 16; e = e->next) {
                                        if (e->ss_id != chosen_ssid && e->is_up && e->ss_data_port > 0) {
                                            replicas[nr++] = e->ss_id;
                                            if (nr >= 1) break; // Limit to 1 replica
                                        }
                                    }
                                    pthread_mutex_unlock(&g_mu);
                                    if (nr > 0) {
                                        nm_state_set_replicas(file, replicas, nr);
                                        // Asynchronously replicate initial empty file to replicas
                                        for (size_t i = 0; i < nr; i++) {
                                            schedule_cmd_repl("CREATE", file, NULL, replicas[i]);
                                        }
                                    }
                                    
                                    (void)nm_state_save("nm_state.json"); const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                } else { const char *er="{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                if (r) free(r);
                            }
                            close(sfd);
                        }
                    }
                }
            }
        } else if (strcmp(type, "DELETE") == 0) {
            // Soft delete: move file to .trash and record in NM state
            char file[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
            else {
                int ssid = 0; if (nm_state_find_dir(file, &ssid) != 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                else {
                    char owner[128]; owner[0]='\0'; if (nm_acl_get_owner(file, owner, sizeof(owner)) != 0 || strcmp(owner, user) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                    else {
                        int data_port = 0; char ss_addr[64];
                        if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port == 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else {
                            // Build trashed path: .trash/<epoch>_<escaped_original>
                            time_t now = time(NULL);
                            char esc[200]; size_t k=0; for (const char *p=file; *p && k<sizeof(esc)-1; ++p) { esc[k++] = (*p=='/'? '_': *p); } esc[k]='\0';
                            char tpath[256]; snprintf(tpath, sizeof(tpath), ".trash/%ld_%s", (long)now, esc);
                            // Issue RENAME on SS
                            int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                            if (sfd < 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                            else {
                                char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "RENAME", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "newFile", tpath, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                else {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r) { close(sfd); const char *er="{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                    else if (!strstr(r, "\"status\":\"OK\"")) { send_msg(fd, r, rl); free(r); close(sfd); }
                                    else {
                                        free(r); close(sfd);
                                        // Replicate rename to replicas (best-effort)
                                        int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
                                        for (size_t i=0;i<nr;i++) schedule_cmd_repl("RENAME", file, tpath, repls[i]);
                                        // Remove mapping and ACLs, add to trash state
                                        nm_dir_del(file); nm_acl_delete(file); nm_state_clear_requests_for(file);
                                        nm_state_trash_add(file, tpath, ssid, owner, (int)now);
                                        (void)nm_state_save("nm_state.json");
                                        const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type, "MIGRATE") == 0) {
            fprintf(stderr, "[NM] MIGRATE request: %s\n", buf ? buf : "<null>");
            char file[128]; int target=0; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0 || json_get_int_field(buf, "targetSsId", &target) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int src_ssid=0;
                if (nm_state_find_dir(file, &src_ssid) != 0) {
                    const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                } else if (src_ssid == target) {
                    const char *resp = "{\"status\":\"OK\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                } else {
                    if (nm_acl_check(file, user, "WRITE") != 0) {
                        const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                    } else {
                        // resolve data ports and addresses
                        int src_port=0, dst_port=0; char src_addr[64], dst_addr[64];
                        if (get_ss_info(src_ssid, &src_port, src_addr, sizeof(src_addr)) != 0 || get_ss_info(target, &dst_port, dst_addr, sizeof(dst_addr)) != 0 || src_port==0 || dst_port==0) {
                            fprintf(stderr, "[NM] MIGRATE resolve failed: src_port=%d dst_port=%d\n", src_port, dst_port);
                            const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                        } else {
                            // READ from source with ticket
                            char ticket[256];
                            if (ticket_build(file, "READ", src_ssid, 600, ticket, sizeof(ticket)) != 0) {
                                const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                            } else {
                                int sfd = tcp_connect(src_addr, (uint16_t)src_port);
                                if (sfd < 0) {
                                    fprintf(stderr, "[NM] MIGRATE connect to src failed: port=%d\n", src_port);
                                    const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                                } else {
                                    char req[512]; req[0]='\0';
                                    json_put_string_field(req, sizeof(req), "type", "READ", 1);
                                    json_put_string_field(req, sizeof(req), "file", file, 0);
                                    json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
                                    strncat(req, "}", sizeof(req)-strlen(req)-1);
                                    if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) {
                                        close(sfd);
                                        const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                    } else {
                                        char *r=NULL; uint32_t rl=0;
                                        if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) {
                                            fprintf(stderr, "[NM] MIGRATE READ failed from src\n");
                                            free(r); close(sfd);
                                            const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                        } else {
                                            // Extract body
                                            char body[8192];
                                            if (json_get_string_field(r, "body", body, sizeof(body)) != 0) {
                                                free(r); close(sfd);
                                                const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                            } else {
                                                free(r); close(sfd);
                                                // PUT to destination
                                                int dfd = tcp_connect(dst_addr, (uint16_t)dst_port);
                                                if (dfd < 0) {
                                                    fprintf(stderr, "[NM] MIGRATE connect to dst failed: port=%d\n", dst_port);
                                                    const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                                } else {
                                                    char preq[9216]; preq[0]='\0';
                                                    json_put_string_field(preq, sizeof(preq), "type", "PUT", 1);
                                                    json_put_string_field(preq, sizeof(preq), "file", file, 0);
                                                    json_put_string_field(preq, sizeof(preq), "body", body, 0);
                                                    strncat(preq, "}", sizeof(preq)-strlen(preq)-1);
                                                    if (send_msg(dfd, preq, (uint32_t)strlen(preq)) != 0) {
                                                        close(dfd);
                                                        const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                                    } else {
                                                        char *r2=NULL; uint32_t r2l=0;
                                                        if (recv_msg(dfd, &r2, &r2l) != 0 || !r2 || !strstr(r2, "\"status\":\"OK\"")) {
                                                            fprintf(stderr, "[NM] MIGRATE PUT failed at dst\n");
                                                            free(r2); close(dfd);
                                                            const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                                        } else {
                                                            free(r2); close(dfd);
                                                            // Delete from source (best-effort)
                                                            int sfd2 = tcp_connect(src_addr, (uint16_t)src_port);
                                                            if (sfd2 >= 0) {
                                                                char dreq[256]; dreq[0]='\0';
                                                                json_put_string_field(dreq, sizeof(dreq), "type", "DELETE", 1);
                                                                json_put_string_field(dreq, sizeof(dreq), "file", file, 0);
                                                                strncat(dreq, "}", sizeof(dreq)-strlen(dreq)-1);
                                                                (void)send_msg(sfd2, dreq, (uint32_t)strlen(dreq));
                                                                char *dr=NULL; uint32_t drl=0; (void)recv_msg(sfd2, &dr, &drl); free(dr); close(sfd2);
                                                            }
                                                            // Update mapping
                                                            nm_dir_set(file, target);
                                                            (void)nm_state_save("nm_state.json");
                                                            const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type, "RENAME") == 0) {
            char file[128], nfile[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0 || json_get_string_field(buf, "newFile", nfile, sizeof(nfile)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int ssid = 0; if (nm_state_find_dir(file, &ssid) != 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                else {
                    if (nm_acl_check(file, user, "WRITE") != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                    else {
                        // Check that new name is free
                        int exists = (nm_state_find_dir(nfile, NULL) == 0);
                        if (exists) { const char *resp = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else {
                            // find SS data port and address
                            int data_port = 0; char ss_addr[64];
                            if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port == 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                            else {
                                int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                                if (sfd < 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                                else {
                                    char req[512]; req[0] = '\0';
                                    json_put_string_field(req, sizeof(req), "type", "RENAME", 1);
                                    json_put_string_field(req, sizeof(req), "file", file, 0);
                                    json_put_string_field(req, sizeof(req), "newFile", nfile, 0);
                                    strncat(req, "}", sizeof(req) - strlen(req) - 1);
                                    if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                        char *r = NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) == 0 && r) {
                                            if (strstr(r, "\"status\":\"OK\"")) {
                                                nm_dir_rename(file, nfile);
                                                nm_acl_rename(file, nfile);
                                                // replicate rename to replicas (lookup after rename using new key)
                                                int repls[16]; size_t nr = nm_state_get_replicas(nfile, repls, 16);
                                                for (size_t i=0;i<nr;i++) schedule_cmd_repl("RENAME", file, nfile, repls[i]);
                                                (void)nm_state_save("nm_state.json");
                                                const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                            } else if (strstr(r, "ERR_CONFLICT")) { const char *er = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                            else if (strstr(r, "ERR_NOTFOUND")) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                            else { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                        } else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                        free(r);
                                    } else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                    close(sfd);
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type, "CREATEFOLDER") == 0) {
            char path[256];
            if (json_get_string_field(buf, "path", path, sizeof(path)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                // Persist logical folder in state
                nm_state_add_folder(path);
                (void)nm_state_save("nm_state.json");
                // Also create the folder physically on the primary SS (prefer ss1 if present)
                int data_port = 0;
                pthread_mutex_lock(&g_mu);
                // Prefer SS id 1 if registered
                for (ss_entry_t *e = g_ss_list; e; e = e->next) {
                    if (e->ss_id == 1 && e->is_up) { data_port = e->ss_data_port; break; }
                }
                // Fallback to any available SS
                if (data_port == 0) {
                    for (ss_entry_t *e = g_ss_list; e; e = e->next) { if (e->is_up) { data_port = e->ss_data_port; break; } }
                }
                pthread_mutex_unlock(&g_mu);
                char ss_addr[64] = "127.0.0.1";
                if (data_port != 0) {
                    pthread_mutex_lock(&g_mu);
                    for (ss_entry_t *e = g_ss_list; e; e = e->next) {
                        if (e->ss_data_port == data_port) { snprintf(ss_addr, sizeof(ss_addr), "%s", e->ss_addr); break; }
                    }
                    pthread_mutex_unlock(&g_mu);
                    int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                    if (sfd >= 0) {
                        char req[512]; req[0]='\0';
                        json_put_string_field(req, sizeof(req), "type", "CREATEFOLDER", 1);
                        json_put_string_field(req, sizeof(req), "path", path, 0);
                        strncat(req, "}", sizeof(req)-strlen(req)-1);
                        (void)send_msg(sfd, req, (uint32_t)strlen(req));
                        char *rr=NULL; uint32_t rrl=0; (void)recv_msg(sfd, &rr, &rrl); if (rr) free(rr); close(sfd);
                    }
                }
                const char *resp = "{\"status\":\"OK\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            }
        } else if (strcmp(type, "VIEWFOLDER") == 0) {
            char in_path[256]; in_path[0]='\0'; (void)json_get_string_field(buf, "path", in_path, sizeof(in_path));
            // Normalize: treat "~" or "/" or empty as root; include a label in response
            const char *label = NULL;
            char path[256]; // effective path used for filtering
            if (in_path[0] == '\0' || (strcmp(in_path, "~")==0) || (strcmp(in_path, "/")==0)) {
                path[0] = '\0';
                label = "~";
            } else {
                snprintf(path, sizeof(path), "%s", in_path);
                label = in_path;
            }
            // Build listing: immediate child folders and files under path
            char resp[8192]; size_t w=0; w += snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"path\":\"%s\",\"folders\":[", label);
            // Folders
            char folders[512][256]; size_t nf = nm_state_get_folders(folders, 512); int first=1;
            size_t plen = strlen(path);
            for (size_t i=0;i<nf;i++){
                const char *f = folders[i];
                if (plen==0 || (strncmp(f, path, plen)==0 && (f[plen]=='/' || plen==0))) {
                    // extract immediate child (the next segment)
                    const char *rest = f + plen; if (*rest=='/') rest++;
                    if (!*rest) continue;
                    const char *slash = strchr(rest, '/');
                    size_t seglen = slash? (size_t)(slash-rest) : strlen(rest);
                    char seg[256]; if (seglen >= sizeof(seg)) seglen = sizeof(seg)-1; memcpy(seg, rest, seglen); seg[seglen]='\0';
                    // emit unique children only (simple O(n^2) dedup given small sizes)
                    int seen = 0;
                    for (size_t j=0;j<i;j++){
                        const char *fj = folders[j];
                        if (plen==0 || (strncmp(fj, path, plen)==0 && (fj[plen]=='/' || plen==0))) {
                            const char *rj = fj + plen; if (*rj=='/') rj++;
                            if (!*rj) continue;
                            const char *sj = strchr(rj, '/'); size_t slen = sj? (size_t)(sj-rj) : strlen(rj);
                            if (slen == seglen && strncmp(rj, seg, seglen)==0) { seen = 1; break; }
                        }
                    }
                    if (!seen) {
                        if (!first) w += snprintf(resp+w, sizeof(resp)-w, ",");
                        first = 0; w += snprintf(resp+w, sizeof(resp)-w, "\"%s\"", seg);
                    }
                }
            }
            if (w < sizeof(resp)) w += snprintf(resp+w, sizeof(resp)-w, "],\"files\":[");
            // Files (from directory mapping)
            char files[512][128]; int ss[512]; size_t n = nm_state_get_dir(files, ss, 512); first=1;
            for (size_t i=0;i<n;i++){
                const char *f = files[i];
                if (plen==0 || (strncmp(f, path, plen)==0 && (f[plen]=='/' || plen==0))) {
                    const char *rest = f + plen; if (*rest=='/') rest++;
                    if (!*rest) continue; // the folder itself
                    // only immediate children that are files (i.e., no further slash in rest)
                    if (!strchr(rest, '/')) {
                        if (!first) w += snprintf(resp+w, sizeof(resp)-w, ",");
                        first=0; w += snprintf(resp+w, sizeof(resp)-w, "\"%s\"", rest);
                    }
                }
            }
            if (w < sizeof(resp)) w += snprintf(resp+w, sizeof(resp)-w, "]}");
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "MOVE") == 0) {
            // MOVE can move a file or a folder prefix; also support moving a file into a known folder
            char src[256], dst_in[256]; src[0]=dst_in[0]='\0';
            if (json_get_string_field(buf, "src", src, sizeof(src)) != 0 || json_get_string_field(buf, "dst", dst_in, sizeof(dst_in)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                // Normalize destination and detect if it's a known folder
                char dst[256]; snprintf(dst, sizeof(dst), "%s", dst_in);
                // strip trailing slashes (except if root "")
                size_t dl = strlen(dst); while (dl>0 && dst[dl-1]=='/') dst[--dl] = '\0';
                int is_folder = 0;
                {
                    char folders[256][256]; size_t nf = nm_state_get_folders(folders, 256);
                    for (size_t i=0;i<nf;i++){ if (strcmp(folders[i], dst) == 0) { is_folder = 1; break; } }
                }
                char final_dst[256];
                if (is_folder) {
                    // final path = dst + "/" + basename(src)
                    const char *base = strrchr(src, '/'); base = base ? base + 1 : src;
                    if (dl == 0) snprintf(final_dst, sizeof(final_dst), "%s", base);
                    else snprintf(final_dst, sizeof(final_dst), "%s/%s", dst, base);
                } else {
                    snprintf(final_dst, sizeof(final_dst), "%s", dst);
                }
                // If no-op (same path), acknowledge success
                if (strcmp(src, final_dst) == 0) { const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok)); }
                else {
                    // Check if src is an existing file mapping
                    int ssid = 0;
                    if (nm_state_find_dir(src, &ssid) == 0) {
                        // File move: reuse RENAME path
                        int data_port = 0; char ss_addr[64];
                        if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port == 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else {
                            int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                            if (sfd < 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                            else {
                                char req[512]; req[0]='\0';
                                json_put_string_field(req, sizeof(req), "type", "RENAME", 1);
                                json_put_string_field(req, sizeof(req), "file", src, 0);
                                json_put_string_field(req, sizeof(req), "newFile", final_dst, 0);
                                strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl)==0 && r && strstr(r, "\"status\":\"OK\"")) {
                                        // Capture replicas of source before state changes
                                        int repls[16]; size_t nr = nm_state_get_replicas(src, repls, 16);
                                        nm_dir_rename(src, final_dst); nm_acl_rename(src, final_dst);
                                        // Replicate rename to replicas
                                        for (size_t i=0;i<nr;i++) schedule_cmd_repl("RENAME", src, final_dst, repls[i]);
                                        (void)nm_state_save("nm_state.json");
                                        const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                    } else { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                    free(r);
                                } else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                close(sfd);
                            }
                        }
                    } else {
                        // Treat as folder move (prefix): compute impacted files and rename on respective SS
                        char files[1024][128]; char new_files[1024][128]; int ssids[1024];
                        int n = nm_state_move_folder_prefix(src, final_dst, files, new_files, ssids, 1024);
                        if (n <= 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else {
                            int failures = 0;
                            for (int i=0; i<n; ++i) {
                                int data_port = 0; char ss_addr[64];
                                if (get_ss_info(ssids[i], &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port == 0) { failures++; continue; }
                                int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                                if (sfd < 0) { failures++; continue; }
                                char req[512]; req[0]='\0';
                                json_put_string_field(req, sizeof(req), "type", "RENAME", 1);
                                json_put_string_field(req, sizeof(req), "file", files[i], 0);
                                json_put_string_field(req, sizeof(req), "newFile", new_files[i], 0);
                                strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); failures++; continue; }
                                char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { failures++; }
                                else {
                                    // Replicate this file rename to its replicas
                                    int repls[16]; size_t nr = nm_state_get_replicas(files[i], repls, 16);
                                    nm_acl_rename(files[i], new_files[i]);
                                    for (size_t j=0;j<nr;j++) schedule_cmd_repl("RENAME", files[i], new_files[i], repls[j]);
                                }
                                free(r); close(sfd);
                            }
                            if (failures) { const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                            else { (void)nm_state_save("nm_state.json"); const char *resp = "{\"status\":\"OK\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        }
                    }
                }
            }
        } else if (strcmp(type, "ADDACCESS") == 0) {
            char file[128], target[128], mode[8];
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0 || json_get_string_field(buf, "user", target, sizeof(target)) != 0 || json_get_string_field(buf, "mode", mode, sizeof(mode)) != 0) {
                const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er));
            } else {
                int perm = (strcmp(mode, "RW")==0)? (ACL_R|ACL_W) : (strcmp(mode, "W")==0? ACL_W : ACL_R);
                nm_acl_grant(file, target, perm); nm_state_remove_request(file, target);
                (void)nm_state_save("nm_state.json");
                const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
            }
        } else if (strcmp(type, "REMACCESS") == 0) {
            char file[128], target[128];
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0 || json_get_string_field(buf, "user", target, sizeof(target)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                nm_acl_revoke(file, target);
                (void)nm_state_save("nm_state.json");
                const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
            }
        } else if (strcmp(type, "VIEWREQUESTS") == 0) {
            char file[128]; char user[128];
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0 || json_get_string_field(buf, "user", user, sizeof(user)) != 0) {
                const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er));
            } else {
                char owner[128]; if (nm_acl_get_owner(file, owner, sizeof(owner)) != 0 || strcmp(owner, user) != 0) { const char *er="{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    char users[256][128]; char modes[256]; size_t n = nm_state_list_requests(file, users, modes, 256);
                    char resp[4096]; size_t w=0; w+=snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"requests\":[");
                    for (size_t i=0;i<n;i++) { if (i) w+=snprintf(resp+w, sizeof(resp)-w, ","); w+=snprintf(resp+w, sizeof(resp)-w, "{\"user\":\"%s\",\"mode\":\"%c\"}", users[i], (modes[i]=='W'?'W':'R')); }
                    if (w < sizeof(resp)) w+=snprintf(resp+w, sizeof(resp)-w, "]}");
                    send_msg(fd, resp, (uint32_t)strlen(resp));
                }
            }
        } else if (strcmp(type, "REQUEST_ACCESS") == 0) {
            char file[128]; char user[128]; char mode[8] = {0};
            if (json_get_string_field(buf, "file", file, sizeof(file))!=0 || json_get_string_field(buf, "user", user, sizeof(user))!=0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                (void)json_get_string_field(buf, "mode", mode, sizeof(mode)); char m = (mode[0]=='W' ? 'W' : 'R');
                if (nm_state_find_dir(file, NULL) != 0) { const char *er="{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    int added = nm_state_add_request(file, user, m);
                    if (added) { (void)nm_state_save("nm_state.json"); const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok)); }
                    else { const char *er="{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                }
            }
        } else if (strcmp(type, "CLIENT_HELLO") == 0) {
            char user[128];
            if (json_get_string_field(buf, "user", user, sizeof(user)) == 0) {
                printf("[NM] Client hello from user=%s\n", user);
                if (nm_state_user_is_active(user)) {
                    const char *er = "{\"status\":\"ERR_CONFLICT\",\"msg\":\"user-already-active\"}";
                    send_msg(fd, er, (uint32_t)strlen(er));
                    free(buf); close(fd); return NULL;
                }
                nm_state_set_user_active(user, 1);
                (void)nm_state_save("nm_state.json");
            } else {
                printf("[NM] Client hello (user unknown)\n");
            }
            const char *resp = "{\"status\":\"OK\"}";
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "LOGOUT") == 0 || strcmp(type, "USER_SET_ACTIVE") == 0) {
            char user[128]; int active = 0; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user));
            if (strcmp(type, "USER_SET_ACTIVE") == 0) {
                (void)json_get_int_field(buf, "active", &active);
            } else {
                active = 0; // LOGOUT means inactive
            }
            if (!user[0]) { const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                nm_state_set_user_active(user, active ? 1 : 0);
                (void)nm_state_save("nm_state.json");
                const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
            }
        } else if (strcmp(type, "LIST_SS") == 0) {
            // Debug endpoint to see registered SS
            pthread_mutex_lock(&g_mu);
            char resp[1024]; resp[0] = '\0';
            strcat(resp, "{\"status\":\"OK\",\"servers\":[");
            ss_entry_t *e = g_ss_list; int first = 1;
            while (e) {
                char item[128];
                snprintf(item, sizeof(item), "%s{\\\"id\\\":%d,\\\"ctrl\\\":%d,\\\"data\\\":%d}", first?"":",", e->ss_id, e->ss_ctrl_port, e->ss_data_port);
                strcat(resp, item);
                first = 0; e = e->next;
            }
            strcat(resp, "]}");
            pthread_mutex_unlock(&g_mu);
            send_msg(fd, resp, (uint32_t)strlen(resp));
    } else if (strcmp(type, "LIST_USERS") == 0) {
            // Return all users with their active status
            char all_users[256][128]; size_t n_all = nm_state_get_users(all_users, 256);
            char active_users[256][128]; size_t n_active = nm_state_get_active_users(active_users, 256);
            
            char resp[8192]; size_t w = 0; 
            w += snprintf(resp + w, sizeof(resp) - w, "{\"status\":\"OK\",\"active\":[");
            for (size_t i = 0; i < n_active; ++i) {
                if (i) w += snprintf(resp + w, sizeof(resp) - w, ",");
                w += snprintf(resp + w, sizeof(resp) - w, "\"%s\"", active_users[i]);
            }
            w += snprintf(resp + w, sizeof(resp) - w, "],\"inactive\":[");
            
            int first_inactive = 1;
            for (size_t i = 0; i < n_all; ++i) {
                // Check if this user is active
                int is_active = 0;
                for (size_t j = 0; j < n_active; ++j) {
                    if (strcmp(all_users[i], active_users[j]) == 0) {
                        is_active = 1;
                        break;
                    }
                }
                if (!is_active) {
                    if (!first_inactive) w += snprintf(resp + w, sizeof(resp) - w, ",");
                    w += snprintf(resp + w, sizeof(resp) - w, "\"%s\"", all_users[i]);
                    first_inactive = 0;
                }
            }
            
            if (w < sizeof(resp)) w += snprintf(resp + w, sizeof(resp) - w, "]}");
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "APPROVE_ACCESS") == 0) {
            char file[128]; char owner[128]; char target[128]; char mode[8]; owner[0]=mode[0]=0;
            if (json_get_string_field(buf, "file", file, sizeof(file))!=0 || json_get_string_field(buf, "user", owner, sizeof(owner))!=0 || json_get_string_field(buf, "target", target, sizeof(target))!=0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                char ow[128]; if (nm_acl_get_owner(file, ow, sizeof(ow))!=0 || strcmp(ow, owner)!=0) { const char *er="{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    (void)json_get_string_field(buf, "mode", mode, sizeof(mode)); int perm = (strcmp(mode, "W")==0? (ACL_R|ACL_W) : (strcmp(mode, "RW")==0? (ACL_R|ACL_W) : ACL_R));
                    nm_acl_grant(file, target, perm); nm_state_remove_request(file, target);
                    (void)nm_state_save("nm_state.json");
                    const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                }
            }
        } else if (strcmp(type, "DENY_ACCESS") == 0) {
            char file[128]; char owner[128]; char target[128]; if (json_get_string_field(buf, "file", file, sizeof(file))!=0 || json_get_string_field(buf, "user", owner, sizeof(owner))!=0 || json_get_string_field(buf, "target", target, sizeof(target))!=0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else { char ow[128]; if (nm_acl_get_owner(file, ow, sizeof(ow))!=0 || strcmp(ow, owner)!=0) { const char *er="{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); } else { nm_state_remove_request(file, target); (void)nm_state_save("nm_state.json"); const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok)); } }
        } else if (strcmp(type, "STATS") == 0) {
            // Count mapped files accurately by requesting a large snapshot
            char f2[1024][128]; int s2[1024]; size_t nf = nm_state_get_dir(f2, s2, 1024);
            int q = repq_get(); int locks = -1;
            char resp[256]; snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"files\":%zu,\"activeLocks\":%d,\"replicationQueue\":%d}", nf, locks, q);
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "LISTTRASH") == 0) {
            // List trashed items
            char files[256][128]; char trashed[256][128]; int ssids[256]; char owners[256][128]; int whens[256];
            size_t n = nm_state_get_trash(files, trashed, ssids, owners, whens, 256);
            char resp[16384]; size_t w=0; w += snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"trash\":[");
            for (size_t i=0;i<n;i++) {
                if (i) w += snprintf(resp+w, sizeof(resp)-w, ",");
                w += snprintf(resp+w, sizeof(resp)-w, "{\"file\":\"%s\",\"trashed\":\"%s\",\"owner\":\"%s\",\"ssid\":%d,\"when\":%d}", files[i], trashed[i], owners[i], ssids[i], whens[i]);
                if (w >= sizeof(resp)) break;
            }
            if (w < sizeof(resp)) w += snprintf(resp+w, sizeof(resp)-w, "]}");
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "RESTORE") == 0) {
            // Restore a trashed file back to original path; owner-only
            char file[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) { const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else if (nm_state_find_dir(file, NULL) == 0) { const char *er = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                char tpath[128]; int ssid=0; char owner[128]; int when=0; owner[0]='\0'; tpath[0]='\0';
                if (nm_state_trash_find(file, tpath, sizeof(tpath), &ssid, owner, sizeof(owner), &when) != 0) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else if (owner[0] && strcmp(owner, user) != 0) { const char *er = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    // RENAME back on SS
                    int data_port=0; char ss_addr[64];
                    if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port==0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                    else {
                        int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                        if (sfd < 0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else {
                            char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "RENAME", 1); json_put_string_field(req, sizeof(req), "file", tpath, 0); json_put_string_field(req, sizeof(req), "newFile", file, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                            if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            else {
                                char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r) { close(sfd); const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                else if (!strstr(r, "\"status\":\"OK\"")) { send_msg(fd, r, rl); free(r); close(sfd); }
                                else {
                                    free(r); close(sfd);
                                    // Recreate mapping and owner ACL
                                    nm_state_trash_remove(file);
                                    nm_dir_set(file, ssid);
                                    if (owner[0]) { nm_acl_set_owner(file, owner); nm_acl_grant(file, owner, ACL_R|ACL_W); }
                                    
                                    // Schedule replication of RENAME to replicas
                                    int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
                                    for (size_t i=0;i<nr;i++) schedule_cmd_repl("RENAME", tpath, file, repls[i]);
                                    
                                    (void)nm_state_save("nm_state.json");
                                    const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type, "EMPTYTRASH") == 0) {
            // Permanently delete trashed files; if 'file' provided, purge only that entry; otherwise purge all owned by 'user'
            char user[128]; user[0]='\0'; (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            char target[128]; int has_file = (json_get_string_field(buf, "file", target, sizeof(target)) == 0);
            // Iterate over trash entries safely by snapshot
            char files[256][128]; char trashed[256][128]; int ssids[256]; char owners[256][128]; int whens[256];
            size_t n = nm_state_get_trash(files, trashed, ssids, owners, whens, 256);
            int purged = 0;
            for (size_t i=0;i<n;i++) {
                if (has_file) { if (strcmp(files[i], target) != 0) continue; }
                else { if (owners[i][0] && strcmp(owners[i], user) != 0) continue; }
                // Delete on SS
                int data_port=0; char ss_addr[64];
                if (get_ss_info(ssids[i], &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port==0) continue;
                int sfd = tcp_connect(ss_addr, (uint16_t)data_port); if (sfd < 0) continue;
                char req[256]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "DELETE", 1); json_put_string_field(req, sizeof(req), "file", trashed[i], 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) { char *r=NULL; uint32_t rl=0; (void)recv_msg(sfd, &r, &rl); if (r) free(r); }
                close(sfd);
                
                // Schedule DELETE replication to replicas
                int repls[16]; size_t nr = nm_state_get_replicas(files[i], repls, 16);
                for (size_t j=0;j<nr;j++) schedule_cmd_repl("DELETE", trashed[i], NULL, repls[j]);
                
                nm_state_trash_remove(files[i]); purged++;
            }
            (void)nm_state_save("nm_state.json");
            const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
        } else if (strcmp(type, "VIEW") == 0) {
            // flags: -a (all), -l (details). Default: only files user can READ.
            char flags[32]; flags[0]='\0'; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            (void)json_get_string_field(buf, "flags", flags, sizeof(flags));
            // Support -a, -l, and combined forms like -al or -la
            int all = (strchr(flags, 'a') != NULL);
            int det = (strchr(flags, 'l') != NULL);
            // Enumerate files
            char files[512][128]; int ssids[512]; size_t n = nm_state_get_dir(files, ssids, 512);
            char resp[16384]; size_t w=0;
            if (!det) {
                w += snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"files\":[");
                int first=1;
                for (size_t i=0;i<n;i++) {
                    if (!all) {
                        int can_r = (nm_acl_check(files[i], user, "READ") == 0);
                        int can_w = (nm_acl_check(files[i], user, "WRITE") == 0);
                        if (!(can_r || can_w)) continue;
                    }
                    if (!first) w += snprintf(resp+w, sizeof(resp)-w, ",");
                    first=0;
                    w += snprintf(resp+w, sizeof(resp)-w, "\"%s\"", files[i]);
                    if (w >= sizeof(resp)) break;
                }
                if (w < sizeof(resp)) w += snprintf(resp+w, sizeof(resp)-w, "]}");
                send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                // Detailed: fetch INFO from SS for each matching file and include owner
                w += snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"details\":[");
                int first=1;
                for (size_t i=0;i<n;i++) {
                    const char *f = files[i]; int ssid = ssids[i];
                    int can_r = (nm_acl_check(f, user, "READ") == 0);
                    int can_w = (nm_acl_check(f, user, "WRITE") == 0);
                    if (!all && !(can_r || can_w)) continue;
                    int size=0, words=0, chars=0, mtime=0, atime=0;
            if (can_r || can_w) {
                        // resolve ss data port and address
                        int data_port=0; char ss_addr[64];
                        if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) == 0 && data_port!=0) {
                // Build READ ticket if allowed, else WRITE ticket
                char ticket[256]; const char *op = can_r ? "READ" : "WRITE";
                if (ticket_build(f, op, ssid, 600, ticket, sizeof(ticket)) == 0) {
                                // Query SS INFO
                                int sfd = tcp_connect(ss_addr, (uint16_t)data_port);
                                if (sfd >= 0) {
                                    char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "INFO", 1);
                                    json_put_string_field(req, sizeof(req), "file", f, 0);
                                    json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
                                    strncat(req, "}", sizeof(req)-strlen(req)-1);
                                    if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) == 0 && r && strstr(r, "\"status\":\"OK\"")) {
                                            (void)json_get_int_field(r, "size", &size); (void)json_get_int_field(r, "words", &words); (void)json_get_int_field(r, "chars", &chars); (void)json_get_int_field(r, "mtime", &mtime); (void)json_get_int_field(r, "atime", &atime);
                                        }
                                        if (r) free(r);
                                    }
                                    close(sfd);
                                }
                            }
                        }
                    }
                    char owner[128]; owner[0]='\0'; (void)nm_acl_get_owner(f, owner, sizeof(owner));
                    if (!first) { w += snprintf(resp+w, sizeof(resp)-w, ","); }
                    first = 0;
                    w += snprintf(resp+w, sizeof(resp)-w, "{\"name\":\"%s\",\"words\":%d,\"chars\":%d,\"size\":%d,\"mtime\":%d,\"atime\":%d,\"owner\":\"%s\"}", f, words, chars, size, mtime, atime, owner);
                    if (w >= sizeof(resp)) break;
                }
                if (w < sizeof(resp)) w += snprintf(resp+w, sizeof(resp)-w, "]}");
                send_msg(fd, resp, (uint32_t)strlen(resp));
            }
        } else if (strcmp(type, "DIR_SET") == 0) { // debug: set mapping
            char file[128]; int ssid = 0;
            if (json_get_string_field(buf, "file", file, sizeof(file)) == 0 && json_get_int_field(buf, "ssId", &ssid) == 0) {
                nm_dir_set(file, ssid);
                (void)nm_state_save("nm_state.json");
                const char *resp = "{\"status\":\"OK\"}";
                send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}";
                send_msg(fd, resp, (uint32_t)strlen(resp));
            }
        } else if (strcmp(type, "INFO") == 0) {
            // INFO collected by NM by querying SS INFO and combining with ACL owner
            char file[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) { const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                int ssid=0; if (nm_state_find_dir(file, &ssid) != 0) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else if (nm_acl_check(file, user, "READ") != 0) { const char *er = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    int data_port=0; char ss_addr[64];
                    if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port==0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                    else {
                        char ticket[256]; if (ticket_build(file, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else {
                            int sfd = tcp_connect(ss_addr, (uint16_t)data_port); if (sfd < 0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            else {
                                char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "INFO", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                else {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r) { close(sfd); const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                    else if (!strstr(r, "\"status\":\"OK\"")) { send_msg(fd, r, rl); free(r); close(sfd); }
                                    else {
                                        int size=0, words=0, chars=0, mtime=0, atime=0; (void)json_get_int_field(r, "size", &size); (void)json_get_int_field(r, "words", &words); (void)json_get_int_field(r, "chars", &chars); (void)json_get_int_field(r, "mtime", &mtime); (void)json_get_int_field(r, "atime", &atime);
                                        free(r); close(sfd);
                                        char owner[128]; owner[0]='\0'; (void)nm_acl_get_owner(file, owner, sizeof(owner));
                                        char access[1024]; access[0]='\0'; (void)nm_acl_format_access(file, access, sizeof(access));
                                        
                                        // Get metadata tracking info
                                        char mod_user[128] = {0}, acc_user[128] = {0};
                                        int mod_time = 0, acc_time = 0;
                                        (void)nm_state_get_file_metadata(file, mod_user, sizeof(mod_user), &mod_time, acc_user, sizeof(acc_user), &acc_time);
                                        
                                        char resp[2048]; snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"file\":\"%s\",\"owner\":\"%s\",\"size\":%d,\"words\":%d,\"chars\":%d,\"mtime\":%d,\"atime\":%d,\"access\":\"%s\",\"last_modified_user\":\"%s\",\"last_modified_time\":%d,\"last_accessed_user\":\"%s\",\"last_accessed_time\":%d}", file, owner, size, words, chars, mtime, atime, access, mod_user[0] ? mod_user : "", mod_time, acc_user[0] ? acc_user : "", acc_time);
                                        send_msg(fd, resp, (uint32_t)strlen(resp));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type, "EXEC") == 0) {
            // Execute file content at NM with Bash and return combined stdout/stderr
            char file[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) { const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                int ssid=0; if (nm_state_find_dir(file, &ssid) != 0) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else if (nm_acl_check(file, user, "READ") != 0) { const char *er = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    int data_port=0; char ss_addr[64];
                    if (get_ss_info(ssid, &data_port, ss_addr, sizeof(ss_addr)) != 0 || data_port==0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                    else {
                        char ticket[256]; if (ticket_build(file, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else {
                            int sfd = tcp_connect(ss_addr, (uint16_t)data_port); if (sfd < 0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            else {
                                char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "READ", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                else {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { if (r) { send_msg(fd, r, rl); free(r);} else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); } close(sfd); }
                                    else {
                                        char body[8192]; body[0]='\0'; (void)json_get_string_field(r, "body", body, sizeof(body));
                                        // Convert JSON escapes (\\n, \\t, etc.) to real characters so /bin/sh sees proper lines
                                        json_unescape_inplace(body);
                                        free(r); close(sfd);
                                        // Find first available SS data folder for execution context
                                        char exec_dir[512]; exec_dir[0]='\0';
                                        pthread_mutex_lock(&g_mu);
                                        for (ss_entry_t *e = g_ss_list; e; e = e->next) {
                                            if (e->is_up && e->ss_id > 0) {
                                                snprintf(exec_dir, sizeof(exec_dir), "ss_data/ss%d/files", e->ss_id);
                                                break;
                                            }
                                        }
                                        pthread_mutex_unlock(&g_mu);
                                        // Execute via /bin/sh reading the script from stdin; capture stdout+stderr via pipe
                                        int out_pipe[2]; int in_pipe[2];
                                        if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
                                            const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                        } else {
                                            pid_t pid = fork();
                                            if (pid < 0) {
                                                close(out_pipe[0]); close(out_pipe[1]); close(in_pipe[0]); close(in_pipe[1]);
                                                const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                            } else if (pid == 0) {
                                                // Child: stdin <- in_pipe[0], stdout/stderr -> out_pipe[1]
                                                dup2(in_pipe[0], STDIN_FILENO);
                                                dup2(out_pipe[1], STDOUT_FILENO);
                                                dup2(out_pipe[1], STDERR_FILENO);
                                                // Close inherited fds
                                                close(in_pipe[0]); close(in_pipe[1]);
                                                close(out_pipe[0]); close(out_pipe[1]);
                                                // Change to data directory if available
                                                if (exec_dir[0]) { int rc = chdir(exec_dir); (void)rc; }
                                                // Exec /bin/sh -s (read from stdin)
                                                execl("/bin/sh", "sh", "-s", (char *)NULL);
                                                _exit(127);
                                            } else {
                                                // Parent: write script to child's stdin, then read and stream output
                                                close(in_pipe[0]); close(out_pipe[1]);
                                                
                                                // Send initial OK with stream marker
                                                const char *start = "{\"status\":\"OK\",\"stream\":\"EXEC\"}";
                                                send_msg(fd, start, (uint32_t)strlen(start));
                                                
                                                // Write script to child's stdin
                                                size_t bl = strlen(body); size_t off = 0; 
                                                while (off < bl) {
                                                    ssize_t wn = write(in_pipe[1], body + off, bl - off);
                                                    if (wn < 0) {
                                                        if (errno == EINTR) continue;
                                                        break;
                                                    }
                                                    off += (size_t)wn;
                                                }
                                                close(in_pipe[1]);
                                                
                                                // Stream output in chunks
                                                char tmp[512]; ssize_t nrd;
                                                while ((nrd = read(out_pipe[0], tmp, sizeof(tmp))) > 0) {
                                                    // Build chunk message with escaped content
                                                    char chunk[2048]; chunk[0]='\0';
                                                    strncat(chunk, "{\"status\":\"OK\",\"chunk\":\"", sizeof(chunk)-strlen(chunk)-1);
                                                    for (ssize_t i=0; i<nrd && strlen(chunk)+4<sizeof(chunk); ++i) {
                                                        char ch = tmp[i];
                                                        if (ch=='\\') { strncat(chunk, "\\\\", sizeof(chunk)-strlen(chunk)-1); }
                                                        else if (ch=='"') { strncat(chunk, "\\\"", sizeof(chunk)-strlen(chunk)-1); }
                                                        else if (ch=='\n') { strncat(chunk, "\\n", sizeof(chunk)-strlen(chunk)-1); }
                                                        else if (ch=='\r') { strncat(chunk, "\\r", sizeof(chunk)-strlen(chunk)-1); }
                                                        else if (ch=='\t') { strncat(chunk, "\\t", sizeof(chunk)-strlen(chunk)-1); }
                                                        else { char s[2]={ch,0}; strncat(chunk, s, sizeof(chunk)-strlen(chunk)-1); }
                                                    }
                                                    strncat(chunk, "\"}", sizeof(chunk)-strlen(chunk)-1);
                                                    send_msg(fd, chunk, (uint32_t)strlen(chunk));
                                                }
                                                close(out_pipe[0]);
                                                
                                                // Wait for child and send final STOP message
                                                int status=0; (void)waitpid(pid, &status, 0);
                                                int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                                                char stop[128];
                                                snprintf(stop, sizeof(stop), "{\"status\":\"STOP\",\"exit\":%d}", exit_code);
                                                send_msg(fd, stop, (uint32_t)strlen(stop));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            fprintf(stderr, "[NM] Unknown type: %s\n", type);
            const char *resp = "{\"status\":\"ERR_BADREQ\"}";
            send_msg(fd, resp, (uint32_t)strlen(resp));
        }
        free(buf);
    }
    close(fd);
    return NULL;
}

static void on_sigint(int sig) {
    (void)sig; g_running = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <nm_ctrl_port>\n", argv[0]);
        return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    signal(SIGINT, on_sigint);

    nm_state_init();
    nm_dir_init();
    nm_state_load("nm_state.json");

    int lfd = tcp_listen(port, BACKLOG);
    if (lfd < 0) { perror("listen"); return 1; }
    printf("[NM] Listening on port %u\n", (unsigned)port);

    // Start heartbeat monitor
    pthread_t th_hb; pthread_create(&th_hb, NULL, hb_monitor_thread, NULL); pthread_detach(th_hb);

    while (g_running) {
        struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clilen);
        if (cfd < 0) { if (g_running) perror("accept"); continue; }
        pthread_t th; pthread_create(&th, NULL, client_thread, (void *)(intptr_t)cfd);
        pthread_detach(th);
    }

    close(lfd);
    // Save state on shutdown
    if (nm_state_save("nm_state.json") == 0) {
        char users[64][128];
        size_t n = nm_state_get_users(users, 64);
        printf("[NM] Saved state with %zu user(s).\n", n);
    }
    printf("[NM] Shutting down.\n");
    return 0;
}
