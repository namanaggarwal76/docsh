#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../common/net_proto.h"
#include "nm_persist.h"
#include "nm_dir.h"
#include "../common/tickets.h"

#define BACKLOG 64

static volatile int g_running = 1;

typedef struct ss_entry {
    int ss_id;
    int ss_ctrl_port;
    int ss_data_port;
    time_t last_heartbeat;
    int is_up;
    struct ss_entry *next;
} ss_entry_t;

static ss_entry_t *g_ss_list = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void add_ss(int id, int ctrl, int data) {
    pthread_mutex_lock(&g_mu);
    ss_entry_t *e = (ss_entry_t *)malloc(sizeof(ss_entry_t));
    e->ss_id = id; e->ss_ctrl_port = ctrl; e->ss_data_port = data; e->last_heartbeat = time(NULL); e->is_up = 1; e->next = g_ss_list; g_ss_list = e;
    pthread_mutex_unlock(&g_mu);
}

static ss_entry_t *find_ss_nolock(int id) {
    for (ss_entry_t *e = g_ss_list; e; e = e->next) if (e->ss_id == id) return e;
    return NULL;
}

static int get_data_port_for(int ssid) {
    int port = 0;
    pthread_mutex_lock(&g_mu);
    ss_entry_t *e = find_ss_nolock(ssid);
    if (e) port = e->ss_data_port;
    pthread_mutex_unlock(&g_mu);
    return port;
}

// Replication queue metric
static pthread_mutex_t g_rep_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_replication_queue = 0; // pending/in-flight tasks

static void repq_inc(int delta) { pthread_mutex_lock(&g_rep_mu); g_replication_queue += delta; if (g_replication_queue < 0) g_replication_queue = 0; pthread_mutex_unlock(&g_rep_mu); }
static int repq_get(void) { pthread_mutex_lock(&g_rep_mu); int v = g_replication_queue; pthread_mutex_unlock(&g_rep_mu); return v; }

// Helper: fetch whole file text from a given SS (by ssid) using READ ticket
static int fetch_file_from_ss(const char *file, int ssid, char *out_body, size_t out_sz) {
    int data_port = get_data_port_for(ssid);
    if (data_port == 0) return -1;
    char ticket[256]; if (ticket_build(file, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) return -1;
    int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port); if (sfd < 0) return -1;
    char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "READ", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
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
        int dport = get_data_port_for(a->target_ssid);
        if (dport) {
            int dfd = tcp_connect("127.0.0.1", (uint16_t)dport);
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

// Fire-and-forget simple command replicate (CREATE/DELETE/RENAME)
typedef struct { char type[16]; char file[128]; char newfile[128]; int target_ssid; } repl_cmd_args_t;
static void *repl_cmd_thread(void *arg) {
    repl_cmd_args_t *a = (repl_cmd_args_t*)arg;
    int dport = get_data_port_for(a->target_ssid);
    if (dport) {
        int dfd = tcp_connect("127.0.0.1", (uint16_t)dport);
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
                    if (cand_up) { nm_dir_set(files[i], cand); // promote
                        // adjust replicas: keep others incl old primary if it returns
                        // simple: move promoted to front but keep list unchanged
                        promoted = 1; fprintf(stderr, "[NM] Promoted %s primary -> ss%d\n", files[i], cand); break; }
                }
                if (promoted) (void)nm_state_save("nm_state.json");
            }
        }
        // Sleep ~2 seconds
        sleep(2);
    }
    return NULL;
}

static int pick_least_loaded_ss(int *out_ssid, int *out_data_port) {
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
    int data_port = 0;
    pthread_mutex_lock(&g_mu);
    for (ss_entry_t *e = g_ss_list; e; e=e->next) {
        if (e->ss_id==chosen_ssid) { data_port = e->ss_data_port; break; }
    }
    pthread_mutex_unlock(&g_mu);
    if (data_port==0) return -1;
    if (out_ssid) *out_ssid = chosen_ssid;
    if (out_data_port) *out_data_port = data_port;
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
            add_ss(ssId, ctrl, data);
            printf("[NM] Registered SS id=%d ctrl=%d data=%d\n", ssId, ctrl, data);
            const char *resp = "{\"status\":\"OK\"}";
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "SS_HEARTBEAT") == 0) {
            int ssId=0; json_get_int_field(buf, "ssId", &ssId);
            pthread_mutex_lock(&g_mu);
            ss_entry_t *e = find_ss_nolock(ssId);
            if (!e) {
                // Unknown server; add with unknown ports
                e = (ss_entry_t *)malloc(sizeof(ss_entry_t)); memset(e,0,sizeof(*e)); e->ss_id=ssId; e->ss_ctrl_port=0; e->ss_data_port=0; e->next=g_ss_list; g_ss_list=e;
            }
            int was_up = e->is_up;
            e->last_heartbeat = time(NULL); e->is_up = 1;
            pthread_mutex_unlock(&g_mu);
            if (!was_up) {
                fprintf(stderr, "[NM] SS %d transitioned UP\n", ssId);
                // Resync files where this SS is a replica
                char files[512][128]; int ps[512]; size_t n = nm_state_get_dir(files, ps, 512);
                for (size_t i=0;i<n;i++) {
                    int repls[16]; size_t nr = nm_state_get_replicas(files[i], repls, 16);
                    for (size_t j=0;j<nr;j++) if (repls[j]==ssId) { schedule_put_repl(files[i], ps[i], ssId); }
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
    } else if (strcmp(type, "LOOKUP") == 0) {
            // LOOKUP for READ/WRITE: {op:"READ"|"WRITE", file:"..."}
            char op[32]; char file[128]; char user[128]; user[0]='\0';
            int have_op = (json_get_string_field(buf, "op", op, sizeof(op)) == 0);
            int have_file = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
            (void)json_get_string_field(buf, "user", user, sizeof(user));
            if (!user[0]) snprintf(user, sizeof(user), "%s", "anonymous");
            fprintf(stderr, "[NM] LOOKUP op=%s file=%s have_op=%d have_file=%d\n", have_op?op:"?", have_file?file:"?", have_op, have_file);
            if (!have_op || !have_file || (strcmp(op, "READ") != 0 && strcmp(op, "WRITE") != 0 && strcmp(op, "UNDO") != 0 && strcmp(op, "HISTORY") != 0 && strcmp(op, "REVERT") != 0 && strcmp(op, "CHECKPOINT") != 0 && strcmp(op, "VIEWCHECKPOINT") != 0 && strcmp(op, "LISTCHECKPOINTS") != 0)) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}";
                send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int ssid = 0;
                if (nm_state_find_dir(file, &ssid) != 0) {
                    if (strcmp(op, "WRITE") == 0) {
                        // Auto-provision mapping on first WRITE: choose first SS and ensure file exists
                        int data_port = 0; int chosen_ssid = 0; (void)pick_least_loaded_ss(&chosen_ssid, &data_port);
                        fprintf(stderr, "[NM] LOOKUP WRITE auto-provision chosen_ssid=%d data_port=%d\n", chosen_ssid, data_port);
                        if (data_port == 0) {
                            const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                        } else {
                            int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port);
                            if (sfd < 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                            else {
                                char req[256]; req[0] = '\0';
                                json_put_string_field(req, sizeof(req), "type", "CREATE", 1);
                                json_put_string_field(req, sizeof(req), "file", file, 0);
                                strncat(req, "}", sizeof(req) - strlen(req) - 1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                    char *r = NULL; uint32_t rl = 0;
                                    if (recv_msg(sfd, &r, &rl) == 0 && r) {
                                        fprintf(stderr, "[NM] SS CREATE response: %.*s\n", rl, r);
                                        // On OK or CONFLICT, set mapping
                                        if (strstr(r, "\"status\":\"OK\"") || strstr(r, "ERR_CONFLICT")) {
                                            nm_dir_set(file, chosen_ssid);
                                            nm_acl_set_owner(file, user);
                                            nm_acl_grant(file, user, ACL_R|ACL_W);
                                            // choose a replica if any other SS available
                                            int repls[4]; size_t nr=0;
                                            pthread_mutex_lock(&g_mu);
                                            for (ss_entry_t *se=g_ss_list; se; se=se->next) if (se->ss_id != chosen_ssid) { if (nr < 4) repls[nr++] = se->ss_id; }
                                            pthread_mutex_unlock(&g_mu);
                                            if (nr>0) nm_state_set_replicas(file, repls, 1); // at least one replica
                                            (void)nm_state_save("nm_state.json");
                                            ssid = chosen_ssid;
                                            fprintf(stderr, "[NM] mapping set %s -> ssId=%d\n", file, ssid);
                                        }
                                    }
                                    free(r);
                                }
                                close(sfd);
                                if (ssid == 0) { const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                                else {
                                    // return endpoint for the chosen SS with a ticket
                                    char ticket[256];
                                    if (ticket_build(file, op, chosen_ssid, 600, ticket, sizeof(ticket)) != 0) {
                                        const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                    } else {
                                        char resp[512];
                                        snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"ssAddr\":\"127.0.0.1\",\"ssDataPort\":%d,\"ticket\":\"%s\"}", data_port, ticket);
                                        fprintf(stderr, "[NM] LOOKUP returning data_port=%d for %s (ticket issued)\n", data_port, file);
                                        send_msg(fd, resp, (uint32_t)strlen(resp));
                                    }
                                }
                            }
                        }
                    } else {
                        fprintf(stderr, "[NM] LOOKUP READ unmapped -> ERR_NOTFOUND\n");
                        const char *resp = "{\"status\":\"ERR_NOTFOUND\"}";
                        send_msg(fd, resp, (uint32_t)strlen(resp));
                    }
                } else {
                    // find ss entry, with failover if primary down
                    int data_port = 0; int target_ssid = ssid;
                    pthread_mutex_lock(&g_mu);
                    ss_entry_t *e = g_ss_list; while (e) { if (e->ss_id == ssid) { if (e->is_up) data_port = e->ss_data_port; break; } e = e->next; }
                    pthread_mutex_unlock(&g_mu);
                    if (data_port == 0) {
                        // try replicas
                        int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
                        for (size_t i=0;i<nr;i++) { int rp = get_data_port_for(repls[i]); int up=0; pthread_mutex_lock(&g_mu); ss_entry_t *re = find_ss_nolock(repls[i]); if (re && re->is_up) { up=1; } pthread_mutex_unlock(&g_mu); if (up && rp) { data_port = rp; target_ssid = repls[i]; break; } }
                        if (data_port && target_ssid != ssid) { nm_dir_set(file, target_ssid); (void)nm_state_save("nm_state.json"); fprintf(stderr, "[NM] LOOKUP failover: promoted ssId=%d for %s\n", target_ssid, file); }
                    }
                    if (data_port == 0) {
                        const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}";
                        send_msg(fd, resp, (uint32_t)strlen(resp));
                    } else {
                        // Enforce ACL at NM before issuing ticket
                        if (nm_acl_check(file, user, op) != 0) {
                            const char *er = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                        } else {
                        char ticket[256];
                        if (ticket_build(file, op, target_ssid, 600, ticket, sizeof(ticket)) != 0) {
                            const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                        } else {
                            char resp[512];
                            fprintf(stderr, "[NM] LOOKUP mapped file=%s ssId=%d data_port=%d\n", file, target_ssid, data_port);
                            snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"ssAddr\":\"127.0.0.1\",\"ssDataPort\":%d,\"ticket\":\"%s\"}", data_port, ticket);
                            send_msg(fd, resp, (uint32_t)strlen(resp));
                        }
                        }
                    }
                }
            }
        } else if (strcmp(type, "CREATE") == 0) {
            char file[128]; char user[128]; user[0]='\0'; (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            int publicRead = 0, publicWrite = 0; (void)json_get_int_field(buf, "publicRead", &publicRead); (void)json_get_int_field(buf, "publicWrite", &publicWrite);
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int exists = (nm_state_find_dir(file, NULL) == 0);
                if (exists) { const char *resp = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                else {
                    // pick first SS
                    pthread_mutex_lock(&g_mu);
                    ss_entry_t *e = g_ss_list; int data_port = e ? e->ss_data_port : 0; int ssid = e ? e->ss_id : 0;
                    pthread_mutex_unlock(&g_mu);
                    if (data_port == 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                    else {
                        int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port);
                        if (sfd < 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else {
                            char req[256]; req[0] = '\0';
                            json_put_string_field(req, sizeof(req), "type", "CREATE", 1);
                            json_put_string_field(req, sizeof(req), "file", file, 0);
                            strncat(req, "}", sizeof(req) - strlen(req) - 1);
                            if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                char *r = NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) == 0 && r) {
                                    if (strstr(r, "\"status\":\"OK\"")) { nm_dir_set(file, ssid); nm_acl_set_owner(file, user); nm_acl_grant(file, user, ACL_R|ACL_W);
                                        if (publicRead || publicWrite) {
                                            int anonPerm = 0;
                                            if (publicWrite) anonPerm |= (ACL_R | ACL_W); // write implies read for anonymous
                                            if (publicRead) anonPerm |= ACL_R;
                                            if (anonPerm) nm_acl_grant(file, "anonymous", anonPerm);
                                        }
                                        // choose a replica and replicate CREATE asynchronously
                                        int repls[4]; size_t nr=0; pthread_mutex_lock(&g_mu); for (ss_entry_t *se=g_ss_list; se; se=se->next){ if (se->ss_id!=ssid && nr<4) repls[nr++]=se->ss_id; } pthread_mutex_unlock(&g_mu);
                                        if (nr>0) { nm_state_set_replicas(file, repls, 1); schedule_cmd_repl("CREATE", file, NULL, repls[0]); }
                                        (void)nm_state_save("nm_state.json");
                                        const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok)); }
                                    else if (strstr(r, "ERR_CONFLICT")) { const char *er = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                    else { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                } else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                free(r);
                            } else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            close(sfd);
                        }
                    }
                }
            }
        } else if (strcmp(type, "DELETE") == 0) {
            char file[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user));
            if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int ssid = 0;
                if (nm_state_find_dir(file, &ssid) != 0) {
                    const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                } else {
                        // Owner-only delete: require requester to be the owner
                        char owner[128]; owner[0]='\0';
                        if (nm_acl_get_owner(file, owner, sizeof(owner)) != 0 || strcmp(owner, user) != 0) {
                        const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                    } else {
                        pthread_mutex_lock(&g_mu);
                        ss_entry_t *e = g_ss_list; int data_port = 0;
                        while (e) { if (e->ss_id == ssid) { data_port = e->ss_data_port; break; } e = e->next; }
                        pthread_mutex_unlock(&g_mu);
                        if (data_port == 0) {
                            const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                        } else {
                            int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port);
                            if (sfd < 0) {
                                const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                            } else {
                                char req[256]; req[0] = '\0';
                                json_put_string_field(req, sizeof(req), "type", "DELETE", 1);
                                json_put_string_field(req, sizeof(req), "file", file, 0);
                                strncat(req, "}", sizeof(req) - strlen(req) - 1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                    char *r = NULL; uint32_t rl=0;
                                    if (recv_msg(sfd, &r, &rl) == 0 && r) {
                                        if (strstr(r, "\"status\":\"OK\"")) {
                                            nm_dir_del(file);
                                                // Clean up ACLs and pending requests for the deleted file
                                                nm_acl_delete(file);
                                                nm_state_clear_requests_for(file);
                                            // replicate delete to replicas
                                            int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
                                            for (size_t i=0;i<nr;i++) schedule_cmd_repl("DELETE", file, NULL, repls[i]);
                                            (void)nm_state_save("nm_state.json");
                                            const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                        } else if (strstr(r, "ERR_NOTFOUND")) {
                                            const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                        } else {
                                            const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                        }
                                    } else {
                                        const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                    }
                                    free(r);
                                } else {
                                    const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                                }
                                close(sfd);
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
                        // resolve data ports
                        int src_port=0, dst_port=0;
                        pthread_mutex_lock(&g_mu);
                        for (ss_entry_t *e=g_ss_list; e; e=e->next){
                            if(e->ss_id==src_ssid && src_port==0) src_port=e->ss_data_port;
                            if(e->ss_id==target && dst_port==0) dst_port=e->ss_data_port;
                            if (src_port && dst_port) break;
                        }
                        pthread_mutex_unlock(&g_mu);
                        if (src_port==0 || dst_port==0) {
                            fprintf(stderr, "[NM] MIGRATE resolve failed: src_port=%d dst_port=%d\n", src_port, dst_port);
                            const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
                        } else {
                            // READ from source with ticket
                            char ticket[256];
                            if (ticket_build(file, "READ", src_ssid, 600, ticket, sizeof(ticket)) != 0) {
                                const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er));
                            } else {
                                int sfd = tcp_connect("127.0.0.1", (uint16_t)src_port);
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
                                                int dfd = tcp_connect("127.0.0.1", (uint16_t)dst_port);
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
                                                            int sfd2 = tcp_connect("127.0.0.1", (uint16_t)src_port);
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
                            // find SS data port
                            pthread_mutex_lock(&g_mu);
                            ss_entry_t *e = g_ss_list; int data_port = 0; while (e) { if (e->ss_id == ssid) { data_port = e->ss_data_port; break; } e = e->next; }
                            pthread_mutex_unlock(&g_mu);
                            if (data_port == 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                            else {
                                int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port);
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
                                                // replicate rename to replicas
                                                int repls[16]; size_t nr = nm_state_get_replicas(file, repls, 16);
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
                nm_state_add_folder(path);
                (void)nm_state_save("nm_state.json");
                const char *resp = "{\"status\":\"OK\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            }
        } else if (strcmp(type, "VIEWFOLDER") == 0) {
            char path[256]; path[0]='\0'; (void)json_get_string_field(buf, "path", path, sizeof(path));
            // Build listing: immediate child folders and files under path
            char resp[8192]; size_t w=0; w += snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"folders\":[");
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
            // MOVE can move a file or a folder prefix
            char src[256], dst[256]; src[0]=dst[0]='\0';
            if (json_get_string_field(buf, "src", src, sizeof(src)) != 0 || json_get_string_field(buf, "dst", dst, sizeof(dst)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                // Check if src is an existing file mapping
                int ssid = 0;
                if (nm_state_find_dir(src, &ssid) == 0) {
                    // File move: reuse RENAME path
                    pthread_mutex_lock(&g_mu);
                    ss_entry_t *e = g_ss_list; int data_port = 0; while (e) { if (e->ss_id == ssid) { data_port = e->ss_data_port; break; } e = e->next; }
                    pthread_mutex_unlock(&g_mu);
                    if (data_port == 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                    else {
                        int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port);
                        if (sfd < 0) { const char *resp = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else {
                            char req[512]; req[0]='\0';
                            json_put_string_field(req, sizeof(req), "type", "RENAME", 1);
                            json_put_string_field(req, sizeof(req), "file", src, 0);
                            json_put_string_field(req, sizeof(req), "newFile", dst, 0);
                            strncat(req, "}", sizeof(req)-strlen(req)-1);
                            if (send_msg(sfd, req, (uint32_t)strlen(req)) == 0) {
                                char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl)==0 && r && strstr(r, "\"status\":\"OK\"")) {
                                    nm_dir_rename(src, dst); nm_acl_rename(src, dst);
                                    (void)nm_state_save("nm_state.json");
                                    const char *ok = "{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok));
                                } else { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                free(r);
                            } else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            close(sfd);
                        }
                    }
                } else {
                    // Treat as folder move: compute impacted files and rename on respective SS
                    char files[1024][128]; char new_files[1024][128]; int ssids[1024];
                    int n = nm_state_move_folder_prefix(src, dst, files, new_files, ssids, 1024);
                    if (n <= 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                    else {
                        int failures = 0;
                        for (int i=0; i<n; ++i) {
                            int data_port = 0;
                            pthread_mutex_lock(&g_mu);
                            for (ss_entry_t *e = g_ss_list; e; e = e->next) { if (e->ss_id == ssids[i]) { data_port = e->ss_data_port; break; } }
                            pthread_mutex_unlock(&g_mu);
                            if (data_port == 0) { failures++; continue; }
                            int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port);
                            if (sfd < 0) { failures++; continue; }
                            char req[512]; req[0]='\0';
                            json_put_string_field(req, sizeof(req), "type", "RENAME", 1);
                            json_put_string_field(req, sizeof(req), "file", files[i], 0);
                            json_put_string_field(req, sizeof(req), "newFile", new_files[i], 0);
                            strncat(req, "}", sizeof(req)-strlen(req)-1);
                            if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); failures++; continue; }
                            char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { failures++; }
                            else { nm_acl_rename(files[i], new_files[i]); }
                            free(r); close(sfd);
                        }
                        if (failures) { const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                        else { (void)nm_state_save("nm_state.json"); const char *resp = "{\"status\":\"OK\"}"; send_msg(fd, resp, (uint32_t)strlen(resp)); }
                    }
                }
            }
        } else if (strcmp(type, "ADDACCESS") == 0) {
            char file[128], target[128], mode[8];
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0 || json_get_string_field(buf, "user", target, sizeof(target)) != 0 || json_get_string_field(buf, "mode", mode, sizeof(mode)) != 0) {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, resp, (uint32_t)strlen(resp));
            } else {
                int perm = (strcmp(mode, "RW")==0)? (ACL_R|ACL_W) : (strcmp(mode, "W")==0? ACL_W : ACL_R);
                nm_acl_grant(file, target, perm);
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
            // Return only active users (logged-in)
            char users[256][128]; size_t n = nm_state_get_active_users(users, 256);
            char resp[4096]; size_t w = 0; w += snprintf(resp + w, sizeof(resp) - w, "{\"status\":\"OK\",\"users\":[");
            for (size_t i = 0; i < n; ++i) {
                if (i) w += snprintf(resp + w, sizeof(resp) - w, ",");
                w += snprintf(resp + w, sizeof(resp) - w, "\"%s\"", users[i]);
            }
            if (w < sizeof(resp)) w += snprintf(resp + w, sizeof(resp) - w, "]}");
            send_msg(fd, resp, (uint32_t)strlen(resp));
        } else if (strcmp(type, "REQUEST_ACCESS") == 0) {
            char file[128]; char user[128]; if (json_get_string_field(buf, "file", file, sizeof(file))!=0 || json_get_string_field(buf, "user", user, sizeof(user))!=0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                if (nm_state_find_dir(file, NULL) != 0) { const char *er="{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    // Try add; if already exists, return ERR_CONFLICT as closest existing code
                    int added = nm_state_add_request(file, user);
                    if (added) { (void)nm_state_save("nm_state.json"); const char *ok="{\"status\":\"OK\"}"; send_msg(fd, ok, (uint32_t)strlen(ok)); }
                    else { const char *er="{\"status\":\"ERR_CONFLICT\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                }
            }
        } else if (strcmp(type, "VIEWREQUESTS") == 0) {
            char file[128]; char user[128]; if (json_get_string_field(buf, "file", file, sizeof(file))!=0 || json_get_string_field(buf, "user", user, sizeof(user))!=0) { const char *er="{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                char owner[128]; if (nm_acl_get_owner(file, owner, sizeof(owner)) != 0 || strcmp(owner, user) != 0) { const char *er="{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    char users[256][128]; size_t n = nm_state_list_requests(file, users, 256);
                    char resp[4096]; size_t w=0; w+=snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"requests\":[");
                    for (size_t i=0;i<n;i++){ if (i) w+=snprintf(resp+w, sizeof(resp)-w, ","); w+=snprintf(resp+w, sizeof(resp)-w, "\"%s\"", users[i]); }
                    if (w < sizeof(resp)) w+=snprintf(resp+w, sizeof(resp)-w, "]}");
                    send_msg(fd, resp, (uint32_t)strlen(resp));
                }
            }
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
                    if (!all && nm_acl_check(files[i], user, "READ") != 0) continue;
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
                    if (!all && nm_acl_check(f, user, "READ") != 0) continue;
                    // resolve ss data port
                    int data_port=0; pthread_mutex_lock(&g_mu);
                    for (ss_entry_t *e=g_ss_list; e; e=e->next){ if(e->ss_id==ssid){ data_port=e->ss_data_port; break; } }
                    pthread_mutex_unlock(&g_mu);
                    if (data_port==0) continue;
                    // Build READ ticket for INFO
                    char ticket[256]; if (ticket_build(f, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) continue;
                    // Query SS INFO
                    int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port); if (sfd < 0) continue;
                    char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "INFO", 1);
                    json_put_string_field(req, sizeof(req), "file", f, 0);
                    json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
                    strncat(req, "}", sizeof(req)-strlen(req)-1);
                    if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); continue; }
                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { if(r) free(r); close(sfd); continue; }
                    // parse fields
                    int size=0, words=0, chars=0, mtime=0, atime=0; (void)json_get_int_field(r, "size", &size); (void)json_get_int_field(r, "words", &words); (void)json_get_int_field(r, "chars", &chars); (void)json_get_int_field(r, "mtime", &mtime); (void)json_get_int_field(r, "atime", &atime);
                    free(r); close(sfd);
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
                    int data_port=0; pthread_mutex_lock(&g_mu); for (ss_entry_t *e=g_ss_list; e; e=e->next){ if(e->ss_id==ssid){ data_port=e->ss_data_port; break; } } pthread_mutex_unlock(&g_mu);
                    if (data_port==0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                    else {
                        char ticket[256]; if (ticket_build(file, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else {
                            int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port); if (sfd < 0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
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
                                        char resp[2048]; snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"file\":\"%s\",\"owner\":\"%s\",\"size\":%d,\"words\":%d,\"chars\":%d,\"mtime\":%d,\"atime\":%d,\"access\":\"%s\"}", file, owner, size, words, chars, mtime, atime, access);
                                        send_msg(fd, resp, (uint32_t)strlen(resp));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type, "EXEC") == 0) {
            // Execute file content at NM and return combined stdout
            char file[128]; char user[128]; user[0]='\0';
            (void)json_get_string_field(buf, "user", user, sizeof(user)); if(!user[0]) snprintf(user,sizeof(user),"%s","anonymous");
            if (json_get_string_field(buf, "file", file, sizeof(file)) != 0) { const char *er = "{\"status\":\"ERR_BADREQ\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
            else {
                int ssid=0; if (nm_state_find_dir(file, &ssid) != 0) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else if (nm_acl_check(file, user, "READ") != 0) { const char *er = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                else {
                    int data_port=0; pthread_mutex_lock(&g_mu); for (ss_entry_t *e=g_ss_list; e; e=e->next){ if(e->ss_id==ssid){ data_port=e->ss_data_port; break; } } pthread_mutex_unlock(&g_mu);
                    if (data_port==0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                    else {
                        char ticket[256]; if (ticket_build(file, "READ", ssid, 600, ticket, sizeof(ticket)) != 0) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                        else {
                            int sfd = tcp_connect("127.0.0.1", (uint16_t)data_port); if (sfd < 0) { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                            else {
                                char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "READ", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
                                if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { close(sfd); const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                else {
                                    char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) != 0 || !r || !strstr(r, "\"status\":\"OK\"")) { if (r) { send_msg(fd, r, rl); free(r);} else { const char *er = "{\"status\":\"ERR_UNAVAILABLE\"}"; send_msg(fd, er, (uint32_t)strlen(er)); } close(sfd); }
                                    else {
                                        char body[8192]; body[0]='\0'; (void)json_get_string_field(r, "body", body, sizeof(body)); free(r); close(sfd);
                                        // Execute via /bin/sh using temp files to capture output
                                        char tmps[512]; snprintf(tmps, sizeof(tmps), "/tmp/nm_exec_%d.sh", getpid());
                                        FILE *tf = fopen(tmps, "wb"); if (!tf) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                        else {
                                            fwrite(body, 1, strlen(body), tf); fflush(tf); fclose(tf);
                                            char tmpout[512]; snprintf(tmpout, sizeof(tmpout), "/tmp/nm_exec_%d.out", getpid());
                                            char cmd[1100]; snprintf(cmd, sizeof(cmd), "/bin/sh '%s' > '%s' 2>&1", tmps, tmpout);
                                            int rc = system(cmd);
                                            (void)rc; // even if non-zero, we capture stderr/stdout
                                            FILE *rf = fopen(tmpout, "rb");
                                            if (!rf) { unlink(tmps); unlink(tmpout); const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(fd, er, (uint32_t)strlen(er)); }
                                            else {
                                                char outbuf[8192]; size_t w=0; int c;
                                                while ((c = fgetc(rf)) != EOF) { if (w + 1 < sizeof(outbuf)) outbuf[w++] = (char)c; }
                                                outbuf[w] = '\0'; fclose(rf); unlink(tmps); unlink(tmpout);
                                                // escape JSON
                                                char j[16384]; j[0]='\0'; strncat(j, "{\"status\":\"OK\",\"output\":\"", sizeof(j)-strlen(j)-1);
                                                for (size_t i=0;i<w && strlen(j)+2<sizeof(j);++i){ char ch=outbuf[i]; if (ch=='"' || ch=='\\') strncat(j, "\\", sizeof(j)-strlen(j)-1); if (ch=='\n') strncat(j, "\\n", sizeof(j)-strlen(j)-1); else { char s[2]={ch,0}; strncat(j,s, sizeof(j)-strlen(j)-1);} }
                                                strncat(j, "\"}", sizeof(j)-strlen(j)-1);
                                                send_msg(fd, j, (uint32_t)strlen(j));
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
