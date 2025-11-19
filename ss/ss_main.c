#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/select.h>

#include <errno.h>
#include <sys/socket.h>
#include <dirent.h>

#include "../common/net_proto.h"
#include "ss_tokenize.h"
#include "../common/tickets.h"

#define SS_PATH_MAX 1024

static volatile int g_run = 1;
static int g_data_lfd = -1;
static int g_ss_id = 0;
static char g_nm_host[64] = {0};
static uint16_t g_nm_port = 0;
static char g_store_root[512] = "ss_data"; // base per-SS store under project dir

// Simple per-file sentence lock table (linked list)
typedef struct lock_node {
    char file[128];
    int sentence_idx;
    struct lock_node *next;
} lock_node_t;
static lock_node_t *g_locks = NULL;
static pthread_mutex_t g_lock_mu = PTHREAD_MUTEX_INITIALIZER;

// Forward declaration (defined later in file)
static void ensure_parent_dirs_for(const char *path);

// Snapshot-based read isolation removed; readers always see the latest committed file.

static int lock_acquire(const char *file, int sidx) {
    pthread_mutex_lock(&g_lock_mu);
    for (lock_node_t *n = g_locks; n; n = n->next) {
        if (n->sentence_idx == sidx && strcmp(n->file, file) == 0) {
            // Debug: log current locks when denying
            fprintf(stderr, "[SS] lock_acquire DENY file=%s sidx=%d (existing lock)\n", file, sidx);
            for (lock_node_t *m = g_locks; m; m = m->next) {
                fprintf(stderr, "[SS]   held: file=%s sidx=%d\n", m->file, m->sentence_idx);
            }
            pthread_mutex_unlock(&g_lock_mu);
            return -1; // already locked
        }
    }
    lock_node_t *n = (lock_node_t *)calloc(1, sizeof(lock_node_t));
    if (!n) { pthread_mutex_unlock(&g_lock_mu); return -1; }
    snprintf(n->file, sizeof(n->file), "%s", file);
    n->sentence_idx = sidx;
    n->next = g_locks; g_locks = n;
    pthread_mutex_unlock(&g_lock_mu);
    return 0;
}

static void lock_release(const char *file, int sidx) {
    pthread_mutex_lock(&g_lock_mu);
    lock_node_t **pp = &g_locks; lock_node_t *cur = g_locks;
    while (cur) {
        if (cur->sentence_idx == sidx && strcmp(cur->file, file) == 0) {
            *pp = cur->next; free(cur); break;
        }
        pp = &cur->next; cur = cur->next;
    }
    pthread_mutex_unlock(&g_lock_mu);
}

static void on_sigint(int sig){ (void)sig; g_run = 0; }

static void ensure_dirs(void) {
    // Create per-SS store root and subfolders if missing
    // Ensure project-level ss_data root exists
    mkdir("ss_data", 0755);
    // Ensure per-SS root and subdirs
    mkdir(g_store_root, 0755);
    char p[1024];
    snprintf(p, sizeof(p), "%s/files", g_store_root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/meta", g_store_root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/undo", g_store_root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/checkpoints", g_store_root); mkdir(p, 0755);
}

static void *heartbeat_thread(void *arg) {
    (void)arg;
    while (g_run) {
        int hfd = tcp_connect(g_nm_host[0]?g_nm_host:"127.0.0.1", g_nm_port);
        if (hfd >= 0) {
            char hb[128]; hb[0]='\0'; json_put_string_field(hb, sizeof(hb), "type", "SS_HEARTBEAT", 1); json_put_int_field(hb, sizeof(hb), "ssId", g_ss_id, 0); strncat(hb, "}", sizeof(hb)-strlen(hb)-1);
            (void)send_msg(hfd, hb, (uint32_t)strlen(hb)); char *hr=NULL; uint32_t hrl=0; (void)recv_msg(hfd, &hr, &hrl); if (hr) free(hr); close(hfd);
        }
        sleep(1);
    }
    return NULL;
}

// Create parent directories for a given path (in-place path not modified).
static void ensure_parent_dirs_for(const char *path) {
    if (!path) return;
    char tmp[SS_PATH_MAX]; snprintf(tmp, sizeof(tmp), "%s", path);
    // Find last slash
    char *p = strrchr(tmp, '/');
    if (!p) return; // no parent dirs
    *p = '\0';
    // Recursively create
    char buf[SS_PATH_MAX]; buf[0] = '\0';
    const char *s = tmp;
    if (*s == '\0') return;
    // Handle absolute-like paths won't occur; start from beginning
    while (*s) {
        // Append next segment
        const char *slash = strchr(s, '/');
        size_t seglen = slash ? (size_t)(slash - s) : strlen(s);
        if (strlen(buf) + seglen + 2 >= sizeof(buf)) break;
        if (buf[0]) strncat(buf, "/", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, s, seglen);
        char dirpath[SS_PATH_MAX]; snprintf(dirpath, sizeof(dirpath), "%s", buf);
        mkdir(dirpath, 0755);
        if (!slash) break;
        s = slash + 1;
    }
}

// Create a single-level undo snapshot for file_path into undo_path if not already present.
// If the source file doesn't exist, create an empty undo file (so UNDO restores to empty).
// (removed) save_undo_snapshot: replaced by session-based snapshot writing in END_WRITE



static void json_escape_append(char *dst, size_t dst_sz, const char *s) {
    while (*s && strlen(dst) + 2 < dst_sz) {
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\') strncat(dst, "\\", dst_sz - strlen(dst) - 1);
        if (c == '\n') { strncat(dst, "\\n", dst_sz - strlen(dst) - 1); continue; }
        char ch[2] = {(char)c, 0};
        strncat(dst, ch, dst_sz - strlen(dst) - 1);
    }
}

static int read_file_into(const char *path, char **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 10 * 1024 * 1024) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)sz + 1); if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f); buf[n] = '\0';
    *out_buf = buf; if (out_len) *out_len = n; return 0;
}

typedef struct { int data_port; int listen_fd; } data_server_args_t;

// Per-connection write session (single sentence at a time)
typedef struct conn_write_session {
    int active;
    char file[128];
    int sentence_idx;
    ss_doc_tokens_t doc;
    char *pre_image;
    size_t pre_image_len;
} conn_write_session_t;

// Per-connection handler to allow concurrent clients (top-level C function)
typedef struct { int cfd; } conn_args_t;
static void *ss_conn_handler(void *varg) {
    conn_args_t *ca = (conn_args_t *)varg;
    int cfd = ca->cfd;
    free(ca);
    fprintf(stderr, "[SS] accept cfd=%d\n", cfd); fflush(stderr);
    // handle multiple requests on same connection
    conn_write_session_t ws; memset(&ws, 0, sizeof(ws));
    for (;;) {
        char *buf = NULL; uint32_t len = 0;
        if (recv_msg(cfd, &buf, &len) != 0) { fprintf(stderr, "[SS] recv_msg error or EOF\n"); free(buf); break; }
        if (!buf || len == 0) { fprintf(stderr, "[SS] empty msg\n"); free(buf); break; }
        fprintf(stderr, "[SS] recv %u bytes: %.*s\n", len, (int)len, buf); fflush(stderr);
        char type[32];
        if (json_get_string_field(buf, "type", type, sizeof(type)) == 0) {
            fprintf(stderr, "[SS] type=%s\n", type); fflush(stderr);
            if (strcmp(type, "READ") == 0) {
                char file[128];
                char ticket[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                if (!okf || !okt) {
                    const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                } else if (ticket_validate(ticket, file, "READ", g_ss_id) != 0) {
                    const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                } else {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    char *content = NULL; size_t clen = 0;
                    if (read_file_into(path, &content, &clen) == 0) {
                        char resp[8192]; resp[0] = '\0';
                        strncat(resp, "{\"status\":\"OK\",\"body\":\"", sizeof(resp) - strlen(resp) - 1);
                        json_escape_append(resp, sizeof(resp), content);
                        strncat(resp, "\"}", sizeof(resp) - strlen(resp) - 1);
                        send_msg(cfd, resp, (uint32_t)strlen(resp));
                        free(content);
                    } else {
                        const char *resp = "{\"status\":\"ERR_NOTFOUND\"}";
                        send_msg(cfd, resp, (uint32_t)strlen(resp));
                    }
                }
            } else if (strcmp(type, "CREATE") == 0) {
                char file[128];
                if (json_get_string_field(buf, "file", file, sizeof(file)) == 0) {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    ensure_parent_dirs_for(path);
                    fprintf(stderr, "[SS] CREATE file=%s path=%s\n", file, path); fflush(stderr);
                    FILE *test = fopen(path, "rb");
                    if (test) { fclose(test); const char *resp = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        FILE *f = fopen(path, "wb");
                        if (!f) { const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                        else { fclose(f); const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    }
                } else { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
            } else if (strcmp(type, "DELETE") == 0) {
                char file[128];
                if (json_get_string_field(buf, "file", file, sizeof(file)) == 0) {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    fprintf(stderr, "[SS] DELETE file=%s path=%s\n", file, path); fflush(stderr);
                    int ok = (unlink(path) == 0);
                    // Best-effort: remove undo snapshot
                    char undopath[SS_PATH_MAX]; snprintf(undopath, sizeof(undopath), "%s/undo/%s.undo", g_store_root, file);
                    (void)unlink(undopath);
                    // Remove checkpoints folder for file
                    char chkdir[SS_PATH_MAX]; snprintf(chkdir, sizeof(chkdir), "%s/checkpoints/%s", g_store_root, file);
                    DIR *cd = opendir(chkdir);
                    if (cd) {
                        struct dirent *de; char entry[SS_PATH_MAX];
                        while ((de = readdir(cd)) != NULL) {
                            if (strcmp(de->d_name, ".")==0 || strcmp(de->d_name, "..")==0) continue;
                            snprintf(entry, sizeof(entry), "%s/checkpoints/%s/%s", g_store_root, file, de->d_name);
                            (void)unlink(entry);
                        }
                        closedir(cd);
                        (void)rmdir(chkdir);
                    }
                    if (ok) { const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                } else { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
            } else if (strcmp(type, "CREATEFOLDER") == 0) {
                // Create a folder inside files/ for this SS
                char pathrel[256];
                if (json_get_string_field(buf, "path", pathrel, sizeof(pathrel)) != 0 || !pathrel[0]) {
                    const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                } else {
                    char dirpath[SS_PATH_MAX];
                    snprintf(dirpath, sizeof(dirpath), "%s/files/%s", g_store_root, pathrel);
                    // Create parent dirs, then the final folder
                    ensure_parent_dirs_for(dirpath);
                    int rc = mkdir(dirpath, 0755);
                    if (rc != 0 && errno != EEXIST) { perror("[SS] mkdir CREATEFOLDER"); const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, er, (uint32_t)strlen(er)); }
                    else { const char *ok = "{\"status\":\"OK\"}"; send_msg(cfd, ok, (uint32_t)strlen(ok)); }
                }
            } else if (strcmp(type, "BEGIN_WRITE") == 0) {
                char file[128]; int sidx = 0; // default to 0 if absent
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                char ticket[256]; int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                int idxrc = json_get_int_field(buf, "sentenceIndex", &sidx);
                fprintf(stderr, "[SS] BEGIN_WRITE file=%s okf=%d idxrc=%d sidx=%d\n", okf?file:"?", okf, idxrc, sidx); fflush(stderr);
                if (!okf) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (!(okt && ticket_validate(ticket, file, "WRITE", g_ss_id) == 0)) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ws.active) { const char *resp = "{\"status\":\"ERR_BADREQ\",\"msg\":\"session-active\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    int lrc = lock_acquire(file, sidx);
                    fprintf(stderr, "[SS] lock_acquire rc=%d\n", lrc); fflush(stderr);
                    if (lrc != 0) { const char *resp = "{\"status\":\"ERR_LOCKED\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        // Mark session active and send OK immediately so client can show prompt without waiting
                        ws.active = 1; snprintf(ws.file, sizeof(ws.file), "%s", file); ws.sentence_idx = sidx; ws.pre_image=NULL; ws.pre_image_len=0; memset(&ws.doc, 0, sizeof(ws.doc));
                        const char *ok_immediate = "{\"status\":\"OK\"}"; send_msg(cfd, ok_immediate, (uint32_t)strlen(ok_immediate));

                        // Now, prepare the in-memory doc/token state. Any error will be surfaced on next APPLY/END_WRITE.
                        char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                        char *content = NULL; size_t clen = 0;
                        int rfirc = read_file_into(path, &content, &clen);
                        fprintf(stderr, "[SS] (post-OK) read_file_into rc=%d path=%s\n", rfirc, path); fflush(stderr);
                        if (rfirc != 0) {
                            // Create missing file and start with an empty document (one empty sentence)
                            ensure_parent_dirs_for(path);
                            FILE *nf = fopen(path, "wb"); if (nf) { fclose(nf); fprintf(stderr, "[SS] created missing file %s\n", path); } else { fprintf(stderr, "[SS] failed to create %s\n", path); }
                            ss_doc_tokens_t doc; memset(&doc, 0, sizeof(doc));
                            doc.num_sentences = 1; doc.sent_words = (char ***)calloc(1, sizeof(char **)); doc.word_counts = (int *)calloc(1, sizeof(int));
                            if (!doc.sent_words || !doc.word_counts || sidx < 0 || sidx >= doc.num_sentences) {
                                if (doc.sent_words) free(doc.sent_words);
                                if (doc.word_counts) free(doc.word_counts);
                                // Fail session lazily; release lock and mark inactive
                                lock_release(file, sidx);
                                ws.active = 0;
                                fprintf(stderr, "[SS] BEGIN_WRITE setup failed for empty doc; session aborted\n");
                            } else {
                                ws.doc = doc; // pre_image stays NULL for empty file
                                fprintf(stderr, "[SS] BEGIN_WRITE session ready (empty doc), sidx=%d\n", sidx); fflush(stderr);
                            }
                        } else {
                            // capture pre-image for UNDO from current content
                            char *pre = NULL; size_t prelen = clen; if (clen > 0) { pre = (char *)malloc(clen); if (pre) memcpy(pre, content, clen); }
                            ss_doc_tokens_t doc; if (ss_tokenize(content, &doc) != 0) {
                                fprintf(stderr, "[SS] tokenize failed (post-OK)\n"); if(pre) free(pre); lock_release(file, sidx); ws.active=0;
                            } else {
                                fprintf(stderr, "[SS] tokenized num_sentences=%d\n", doc.num_sentences); fflush(stderr);
                                if (doc.num_sentences == 0 && sidx == 0) {
                                    doc.num_sentences = 1;
                                    doc.sent_words = (char ***)calloc(1, sizeof(char **));
                                    doc.word_counts = (int *)calloc(1, sizeof(int));
                                    if (!doc.sent_words || !doc.word_counts) { ss_tokens_free(&doc); if(pre) free(pre); lock_release(file, sidx); ws.active=0; }
                                }
                                if (ws.active) {
                                    if (sidx < 0 || sidx > doc.num_sentences) {
                                        fprintf(stderr, "[SS] sidx out of range (post-OK): sidx=%d num_sentences=%d\n", sidx, doc.num_sentences);
                                        ss_tokens_free(&doc); if(pre) free(pre); lock_release(file, sidx); ws.active=0;
                                    } else if (sidx == doc.num_sentences) {
                                        // append new empty sentence
                                        int ns = doc.num_sentences + 1;
                                        char ***sw = (char ***)realloc(doc.sent_words, (size_t)ns * sizeof(char **));
                                        int *wcarr = (int *)realloc(doc.word_counts, (size_t)ns * sizeof(int));
                                        if (!sw || !wcarr) { ss_tokens_free(&doc); if(pre) free(pre); lock_release(file, sidx); ws.active=0; }
                                        else {
                                            doc.sent_words = sw; doc.word_counts = wcarr; doc.num_sentences = ns; doc.sent_words[ns-1] = NULL; doc.word_counts[ns-1] = 0;
                                            ws.doc = doc; ws.pre_image = pre; ws.pre_image_len = pre ? prelen : 0;
                                            fprintf(stderr, "[SS] BEGIN_WRITE session ready (append new sentence), sidx=%d\n", sidx); fflush(stderr);
                                        }
                                    } else {
                                        ws.doc = doc; ws.pre_image = pre; ws.pre_image_len = pre ? prelen : 0;
                                        fprintf(stderr, "[SS] BEGIN_WRITE session ready, sidx=%d\n", sidx); fflush(stderr);
                                    }
                                }
                            }
                        }
                        if (content) free(content);
                    }
                }
            } else if (strcmp(type, "APPLY") == 0) {
                if (!ws.active) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    int widx = -1; char content[512]; content[0] = '\0';
                    int okw = (json_get_int_field(buf, "wordIndex", &widx) == 0);
                    int okc = (json_get_string_field(buf, "content", content, sizeof(content)) == 0);
                    fprintf(stderr, "[SS] APPLY okw=%d okc=%d widx=%d content=%s\n", okw, okc, widx, okc?content:"?"); fflush(stderr);
                    if (!okw || !okc) { const char *resp = "{\"status\":\"ERR_BADREQ\",\"msg\":\"missing-fields\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        if (ss_tokens_replace_or_append(&ws.doc, ws.sentence_idx, widx, content) != 0) {
                            fprintf(stderr, "[SS] APPLY failed (indices)\n"); fflush(stderr);
                            const char *resp = "{\"status\":\"ERR_BADREQ\",\"msg\":\"invalid-index-or-content\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                        } else {
                            fprintf(stderr, "[SS] APPLY OK\n"); fflush(stderr);
                            const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                        }
                    }
                }
            } else if (strcmp(type, "END_WRITE") == 0) {
                if (!ws.active) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    // Merge-on-commit: re-read current file and replace only the target sentence with ws.doc's target sentence
                    char *new_text = NULL;
                    do {
                        char path_cur[SS_PATH_MAX]; snprintf(path_cur, sizeof(path_cur), "%s/files/%s", g_store_root, ws.file);
                        char *cur_content = NULL; size_t cur_len = 0;
                        ss_doc_tokens_t cur_doc; memset(&cur_doc, 0, sizeof(cur_doc));
                        int have_cur = (read_file_into(path_cur, &cur_content, &cur_len) == 0 && ss_tokenize(cur_content ? cur_content : "", &cur_doc) == 0);
                        if (cur_content) free(cur_content);

                        if (!have_cur) {
                            // Fallback: start from the session doc (legacy behavior)
                            new_text = ss_tokens_compose(&ws.doc);
                            break;
                        }

                        int sidx = ws.sentence_idx;
                        // Ensure cur_doc has enough sentences to hold sidx
                        if (sidx < 0) { ss_tokens_free(&cur_doc); break; }
                        if (sidx >= cur_doc.num_sentences) {
                            int needed = sidx + 1;
                            int old = cur_doc.num_sentences;
                            // Grow arrays
                            char ***sw = (char ***)realloc(cur_doc.sent_words, (size_t)needed * sizeof(char **));
                            int *wc = (int *)realloc(cur_doc.word_counts, (size_t)needed * sizeof(int));
                            if (!sw || !wc) { ss_tokens_free(&cur_doc); break; }
                            cur_doc.sent_words = sw; cur_doc.word_counts = wc;
                            for (int i = old; i < needed; ++i) { cur_doc.sent_words[i] = NULL; cur_doc.word_counts[i] = 0; }
                            cur_doc.num_sentences = needed;
                        }
                        // Replace sentence sidx in cur_doc with deep copy from ws.doc
                        // Free existing tokens in target sentence
                        for (int j = 0; j < cur_doc.word_counts[sidx]; ++j) free(cur_doc.sent_words[sidx][j]);
                        free(cur_doc.sent_words[sidx]);
                        // Duplicate from ws.doc
                        int wc2 = (sidx < ws.doc.num_sentences) ? ws.doc.word_counts[sidx] : 0;
                        char **row = NULL;
                        if (wc2 > 0) {
                            row = (char **)malloc((size_t)wc2 * sizeof(char *));
                            if (!row) { ss_tokens_free(&cur_doc); break; }
                            for (int j = 0; j < wc2; ++j) {
                                const char *srcw = ws.doc.sent_words[sidx][j];
                                size_t ln = strlen(srcw);
                                row[j] = (char *)malloc(ln + 1);
                                if (!row[j]) { for (int k=0;k<j;k++) free(row[k]); free(row); ss_tokens_free(&cur_doc); row=NULL; break; }
                                memcpy(row[j], srcw, ln + 1);
                            }
                            if (!row) break;
                        }
                        cur_doc.sent_words[sidx] = row;
                        cur_doc.word_counts[sidx] = wc2;

                        new_text = ss_tokens_compose(&cur_doc);
                        ss_tokens_free(&cur_doc);
                    } while (0);

                    fprintf(stderr, "[SS] END_WRITE composing: %s\n", new_text?new_text:"(null)"); fflush(stderr);
                    if (!new_text) { const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, ws.file);
                        // Before committing, save a one-level undo snapshot of the pre-commit content captured at BEGIN_WRITE
                        char undopath[SS_PATH_MAX]; snprintf(undopath, sizeof(undopath), "%s/undo/%s.undo", g_store_root, ws.file);
                        char tmppath[SS_PATH_MAX];
                        size_t pl = strlen(path);
                        if (pl + 4 + 1 <= sizeof(tmppath)) {
                            snprintf(tmppath, sizeof(tmppath), "%s.tmp", path);
                        } else {
                            char mp[SS_PATH_MAX]; snprintf(mp, sizeof(mp), "%s/meta", g_store_root);
                            mkdir(mp, 0755);
                            snprintf(tmppath, sizeof(tmppath), "%s", mp);
                            strncat(tmppath, "/commit.tmp", sizeof(tmppath) - strlen(tmppath) - 1);
                        }
                        fprintf(stderr, "[SS] END_WRITE write temp=%s final=%s\n", tmppath, path); fflush(stderr);
                        FILE *f = fopen(tmppath, "wb");
                        if (!f) { free(new_text); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                        else {
                            size_t n = fwrite(new_text, 1, strlen(new_text), f);
                            (void)n; fflush(f);
                            fclose(f);
                            // Snapshot for UNDO from session pre_image (best-effort)
                            ensure_parent_dirs_for(undopath);
                            FILE *uf = fopen(undopath, "wb");
                            if (uf) { if (ws.pre_image && ws.pre_image_len > 0) fwrite(ws.pre_image, 1, ws.pre_image_len, uf); fflush(uf); fclose(uf); fprintf(stderr, "[SS] undo snapshot saved from session: %s (len=%zu)\n", undopath, ws.pre_image_len);} else { perror("[SS] undo fopen"); }
                            if (rename(tmppath, path) != 0) {
                                perror("[SS] rename");
                                unlink(tmppath); free(new_text); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                            } else {
                                fprintf(stderr, "[SS] END_WRITE commit OK\n"); fflush(stderr);
                                free(new_text);
                                const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                                // Notify NM about commit for replication
                                int nfd = tcp_connect(g_nm_host[0]?g_nm_host:"127.0.0.1", g_nm_port);
                                if (nfd >= 0) {
                                    char note[256]; note[0]='\0'; json_put_string_field(note, sizeof(note), "type", "SS_COMMIT", 1); json_put_string_field(note, sizeof(note), "file", ws.file, 0); json_put_int_field(note, sizeof(note), "ssId", g_ss_id, 0); strncat(note, "}", sizeof(note)-strlen(note)-1);
                                    (void)send_msg(nfd, note, (uint32_t)strlen(note)); char *nr=NULL; uint32_t nrl=0; (void)recv_msg(nfd, &nr, &nrl); if (nr) free(nr); close(nfd);
                                }
                            }
                        }
                    }
                    lock_release(ws.file, ws.sentence_idx);
                    ss_tokens_free(&ws.doc);
                    if (ws.pre_image) { free(ws.pre_image); ws.pre_image=NULL; ws.pre_image_len=0; }
                    memset(&ws, 0, sizeof(ws));
                }
            } else if (strcmp(type, "UNDO") == 0) {
                char file[128]; char ticket[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                if (!okf || !okt) {
                    const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                } else if (ticket_validate(ticket, file, "UNDO", g_ss_id) != 0) {
                    const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                } else {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    char undopath[SS_PATH_MAX]; snprintf(undopath, sizeof(undopath), "%s/undo/%s.undo", g_store_root, file);
                    // Load undo snapshot
                    char *undo_content = NULL; size_t ulen = 0;
                    if (read_file_into(undopath, &undo_content, &ulen) != 0) {
                        const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                    } else {
                        // Write undo content to a temp and atomically replace
                        char tmppath[SS_PATH_MAX];
                        size_t pl = strlen(path);
                        if (pl + 6 + 1 <= sizeof(tmppath)) {
                            snprintf(tmppath, sizeof(tmppath), "%s.udtmp", path);
                        } else {
                            char mp[SS_PATH_MAX]; snprintf(mp, sizeof(mp), "%s/meta", g_store_root);
                            mkdir(mp, 0755);
                            snprintf(tmppath, sizeof(tmppath), "%s", mp);
                            strncat(tmppath, "/undo.tmp", sizeof(tmppath) - strlen(tmppath) - 1);
                        }
                    char path2[SS_PATH_MAX]; snprintf(path2, sizeof(path2), "%s/files/%s", g_store_root, file);
                        FILE *f = fopen(tmppath, "wb");
                        if (!f) { free(undo_content); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                        else {
                            size_t n = fwrite(undo_content, 1, ulen, f); (void)n; fflush(f); fclose(f);
                            free(undo_content);
                            if (rename(tmppath, path2) != 0) {
                                perror("[SS] undo rename"); unlink(tmppath); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                            } else {
                                // Consume the undo snapshot after successful restore
                                unlink(undopath);
                                const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                                // Notify NM about commit for replication
                                int nfd = tcp_connect(g_nm_host[0]?g_nm_host:"127.0.0.1", g_nm_port);
                                if (nfd >= 0) {
                                    char note[256]; note[0]='\0'; json_put_string_field(note, sizeof(note), "type", "SS_COMMIT", 1); json_put_string_field(note, sizeof(note), "file", file, 0); json_put_int_field(note, sizeof(note), "ssId", g_ss_id, 0); strncat(note, "}", sizeof(note)-strlen(note)-1);
                                    (void)send_msg(nfd, note, (uint32_t)strlen(note)); char *nr=NULL; uint32_t nrl=0; (void)recv_msg(nfd, &nr, &nrl); if (nr) free(nr); close(nfd);
                                }
                            }
                        }
                    }
                }
            } else if (strcmp(type, "REVERT") == 0) {
                char file[128]; char ticket[256]; char cname[256]; cname[0]='\0';
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                (void)json_get_string_field(buf, "name", cname, sizeof(cname));
                if (!okf || !okt || !cname[0]) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ticket_validate(ticket, file, "REVERT", g_ss_id) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    // Load checkpoint snapshot to revert to
                    char hpath[SS_PATH_MAX];
                    snprintf(hpath, sizeof(hpath), "%s/checkpoints/%s/%s.chk", g_store_root, file, cname);
                    char *snap=NULL; size_t slen=0;
                    if (read_file_into(hpath, &snap, &slen) != 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                        char tmppath[SS_PATH_MAX]; size_t pl=strlen(path);
                        if (pl + 6 + 1 <= sizeof(tmppath)) snprintf(tmppath, sizeof(tmppath), "%s.rvtmp", path); else { char mp[SS_PATH_MAX]; snprintf(mp, sizeof(mp), "%s/meta", g_store_root); mkdir(mp,0755); snprintf(tmppath, sizeof(tmppath), "%s", mp); strncat(tmppath, "/revert.tmp", sizeof(tmppath)-strlen(tmppath)-1);} 
                        FILE *f = fopen(tmppath, "wb"); if (!f) { free(snap); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                        else { fwrite(snap, 1, slen, f); fflush(f); fclose(f); free(snap); if (rename(tmppath, path)!=0) { perror("[SS] revert rename"); unlink(tmppath); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); } else { const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); 
                                // Notify NM about commit for replication
                                int nfd = tcp_connect(g_nm_host[0]?g_nm_host:"127.0.0.1", g_nm_port);
                                if (nfd >= 0) {
                                    char note[256]; note[0]='\0'; json_put_string_field(note, sizeof(note), "type", "SS_COMMIT", 1); json_put_string_field(note, sizeof(note), "file", file, 0); json_put_int_field(note, sizeof(note), "ssId", g_ss_id, 0); strncat(note, "}", sizeof(note)-strlen(note)-1);
                                    (void)send_msg(nfd, note, (uint32_t)strlen(note)); char *nr=NULL; uint32_t nrl=0; (void)recv_msg(nfd, &nr, &nrl); if (nr) free(nr); close(nfd);
                                }
                            } }
                    }
                }
            } else if (strcmp(type, "CHECKPOINT") == 0) {
                char file[128]; char ticket[256]; char name[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                int okn = (json_get_string_field(buf, "name", name, sizeof(name)) == 0);
                if (!okf || !okt || !okn || !name[0]) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ticket_validate(ticket, file, "CHECKPOINT", g_ss_id) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    char *cur=NULL; size_t clen=0; if (read_file_into(path, &cur, &clen) != 0) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, er, (uint32_t)strlen(er)); }
                    else {
                        char cpath[SS_PATH_MAX]; snprintf(cpath, sizeof(cpath), "%s/checkpoints/%s/%s.chk", g_store_root, file, name);
                        ensure_parent_dirs_for(cpath);
                        FILE *f = fopen(cpath, "wb"); if (!f) { free(cur); const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, er, (uint32_t)strlen(er)); }
                        else { fwrite(cur, 1, clen, f); fflush(f); fclose(f); free(cur); const char *ok="{\"status\":\"OK\"}"; send_msg(cfd, ok, (uint32_t)strlen(ok));
                            // Notify NM about checkpoint for replication
                            int nfd = tcp_connect(g_nm_host[0]?g_nm_host:"127.0.0.1", g_nm_port);
                            if (nfd >= 0) {
                                char note[512]; note[0]='\0';
                                json_put_string_field(note, sizeof(note), "type", "SS_CHECKPOINT", 1);
                                json_put_string_field(note, sizeof(note), "file", file, 0);
                                json_put_string_field(note, sizeof(note), "name", name, 0);
                                json_put_int_field(note, sizeof(note), "ssId", g_ss_id, 0);
                                strncat(note, "}", sizeof(note)-strlen(note)-1);
                                (void)send_msg(nfd, note, (uint32_t)strlen(note)); char *nr=NULL; uint32_t nrl=0; (void)recv_msg(nfd, &nr, &nrl); if (nr) free(nr); close(nfd);
                            }
                        }
                    }
                }
            } else if (strcmp(type, "PUT_CHECKPOINT") == 0) {
                // Internal replication endpoint: write provided body to a checkpoint file
                char file[128]; char name[256]; char body[8192]; body[0]='\0';
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okn = (json_get_string_field(buf, "name", name, sizeof(name)) == 0);
                int okb = (json_get_string_field(buf, "body", body, sizeof(body)) == 0);
                if (!okf || !okn || !okb || !name[0]) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char cpath[SS_PATH_MAX]; snprintf(cpath, sizeof(cpath), "%s/checkpoints/%s/%s.chk", g_store_root, file, name);
                    ensure_parent_dirs_for(cpath);
                    FILE *f = fopen(cpath, "wb");
                    if (!f) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, er, (uint32_t)strlen(er)); }
                    else { fwrite(body, 1, strlen(body), f); fflush(f); fclose(f); const char *ok = "{\"status\":\"OK\"}"; send_msg(cfd, ok, (uint32_t)strlen(ok)); }
                }
            } else if (strcmp(type, "PUT_UNDO") == 0) {
                // Internal replication endpoint: write provided body to an undo file
                char file[128]; char body[8192]; body[0]='\0';
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okb = (json_get_string_field(buf, "body", body, sizeof(body)) == 0);
                if (!okf || !okb) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char upath[SS_PATH_MAX]; snprintf(upath, sizeof(upath), "%s/undo/%s.undo", g_store_root, file);
                    ensure_parent_dirs_for(upath);
                    FILE *f = fopen(upath, "wb");
                    if (!f) { const char *er = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, er, (uint32_t)strlen(er)); }
                    else { fwrite(body, 1, strlen(body), f); fflush(f); fclose(f); fprintf(stderr, "[SS] PUT_UNDO saved: %s\n", upath); const char *ok = "{\"status\":\"OK\"}"; send_msg(cfd, ok, (uint32_t)strlen(ok)); }
                }
            } else if (strcmp(type, "LISTCHECKPOINTS") == 0) {
                char file[128]; char ticket[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                if (!okf || !okt) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ticket_validate(ticket, file, "LISTCHECKPOINTS", g_ss_id) != 0 && ticket_validate(ticket, file, "VIEWCHECKPOINT", g_ss_id) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char dpath[SS_PATH_MAX]; snprintf(dpath, sizeof(dpath), "%s/checkpoints/%s", g_store_root, file);
                    DIR *d = opendir(dpath);
                    char resp[4096]; size_t w=0; w += snprintf(resp+w, sizeof(resp)-w, "{\"status\":\"OK\",\"checkpoints\":[");
                    int first=1; if (d) {
                        struct dirent *de; while ((de=readdir(d))!=NULL) {
                            const char *n = de->d_name;
                            size_t ln = strlen(n);
                            if (ln > 4 && strcmp(n + ln - 4, ".chk") == 0) {
                                char name[256]; size_t sl = ln - 4; if (sl >= sizeof(name)) sl = sizeof(name)-1; memcpy(name, n, sl); name[sl]='\0';
                                if (!first) w += snprintf(resp+w, sizeof(resp)-w, ",");
                                first=0;
                                w += snprintf(resp+w, sizeof(resp)-w, "\"%s\"", name);
                            }
                        }
                        closedir(d);
                    }
                    if (w < sizeof(resp)) w += snprintf(resp+w, sizeof(resp)-w, "]}");
                    send_msg(cfd, resp, (uint32_t)strlen(resp));
                }
            } else if (strcmp(type, "VIEWCHECKPOINT") == 0) {
                char file[128]; char ticket[256]; char name[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                int okn = (json_get_string_field(buf, "name", name, sizeof(name)) == 0);
                if (!okf || !okt || !okn) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ticket_validate(ticket, file, "VIEWCHECKPOINT", g_ss_id) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char cpath[SS_PATH_MAX]; snprintf(cpath, sizeof(cpath), "%s/checkpoints/%s/%s.chk", g_store_root, file, name);
                    char *content=NULL; size_t clen=0; if (read_file_into(cpath, &content, &clen) != 0) { const char *er = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, er, (uint32_t)strlen(er)); }
                    else {
                        char resp[8192]; resp[0]='\0';
                        strncat(resp, "{\"status\":\"OK\",\"body\":\"", sizeof(resp)-strlen(resp)-1);
                        json_escape_append(resp, sizeof(resp), content);
                        strncat(resp, "\"}", sizeof(resp)-strlen(resp)-1);
                        free(content);
                        send_msg(cfd, resp, (uint32_t)strlen(resp));
                    }
                }
            } else if (strcmp(type, "RENAME") == 0) {
                char file[128], nfile[128];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okn = (json_get_string_field(buf, "newFile", nfile, sizeof(nfile)) == 0);
                if (!okf || !okn) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char path_old[SS_PATH_MAX]; snprintf(path_old, sizeof(path_old), "%s/files/%s", g_store_root, file);
                    char path_new[SS_PATH_MAX]; snprintf(path_new, sizeof(path_new), "%s/files/%s", g_store_root, nfile);
                    // Conflicts
                    struct stat st;
                    if (stat(path_old, &st) != 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else if (stat(path_new, &st) == 0) { const char *resp = "{\"status\":\"ERR_CONFLICT\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        // Rename undo snapshot if present
                        char u_old[SS_PATH_MAX]; snprintf(u_old, sizeof(u_old), "%s/undo/%s.undo", g_store_root, file);
                        char u_new[SS_PATH_MAX]; snprintf(u_new, sizeof(u_new), "%s/undo/%s.undo", g_store_root, nfile);
                        ensure_parent_dirs_for(u_new);
                        if (stat(u_old, &st) == 0) { (void)rename(u_old, u_new); }
                        // Rename checkpoints directory if present
                        char c_old[SS_PATH_MAX]; snprintf(c_old, sizeof(c_old), "%s/checkpoints/%s", g_store_root, file);
                        char c_new[SS_PATH_MAX]; snprintf(c_new, sizeof(c_new), "%s/checkpoints/%s", g_store_root, nfile);
                        if (stat(c_old, &st) == 0) {
                            ensure_parent_dirs_for(c_new);
                            // Attempt directory rename (will move the folder atomically within same filesystem)
                            (void)rename(c_old, c_new);
                        }
                        // Finally rename main file
                        ensure_parent_dirs_for(path_new);
                        if (rename(path_old, path_new) != 0) { perror("[SS] rename main"); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                        else { const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    }
                }
            } else if (strcmp(type, "PUT") == 0) {
                // Atomically replace file contents with provided body (raw text)
                char file[128]; char body[8192]; body[0]='\0';
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okb = (json_get_string_field(buf, "body", body, sizeof(body)) == 0);
                if (!okf || !okb) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    // Write to temp then rename
                    char tmppath[SS_PATH_MAX]; size_t pl=strlen(path);
                    if (pl + 5 + 1 <= sizeof(tmppath)) {
                        snprintf(tmppath, sizeof(tmppath), "%s.ptmp", path);
                    } else {
                        char mp[SS_PATH_MAX]; snprintf(mp, sizeof(mp), "%s/meta", g_store_root); mkdir(mp,0755);
                        snprintf(tmppath, sizeof(tmppath), "%s", mp);
                        strncat(tmppath, "/put.tmp", sizeof(tmppath) - strlen(tmppath) - 1);
                    }
                    fprintf(stderr, "[SS] PUT writing tmppath=%s final=%s len=%zu\n", tmppath, path, strlen(body)); fflush(stderr);
                    ensure_parent_dirs_for(path);
                    FILE *f = fopen(tmppath, "wb");
                    if (!f) { perror("[SS] put fopen"); const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        fwrite(body, 1, strlen(body), f); fflush(f); fclose(f);
                        if (rename(tmppath, path) != 0) {
                            perror("[SS] put rename");
                            unlink(tmppath);
                            const char *resp = "{\"status\":\"ERR_INTERNAL\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                        } else {
                            char cwd[512]; if (getcwd(cwd, sizeof(cwd))) fprintf(stderr, "[SS] PUT commit OK at %s -> %s\n", cwd, path);
                            else fprintf(stderr, "[SS] PUT commit OK -> %s\n", path);
                            const char *resp = "{\"status\":\"OK\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp));
                        }
                    }
                }
            } else if (strcmp(type, "INFO") == 0) {
                // Return file metadata: size (bytes), mtime, word count, char count
                char file[128]; char ticket[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                if (!okf || !okt) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ticket_validate(ticket, file, "READ", g_ss_id) != 0 && ticket_validate(ticket, file, "WRITE", g_ss_id) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    struct stat st;
                    if (stat(path, &st) != 0) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        // Read content to count words (best-effort)
                        char *content=NULL; size_t clen=0; int words=0;
                        if (read_file_into(path, &content, &clen) == 0 && content) {
                            // Count words by splitting on spaces/newlines
                            int in_word = 0; for (size_t i=0;i<clen;i++){ unsigned char c=(unsigned char)content[i]; if (c==' '||c=='\n' || c=='\t' || c=='\r'){ if (in_word){ words++; in_word=0; } } else { in_word=1; } }
                            if (in_word) words++;
                            free(content);
                        }
                        char resp[512];
                        snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"size\":%lld,\"mtime\":%lld,\"atime\":%lld,\"words\":%d,\"chars\":%lld}",
                                 (long long)st.st_size, (long long)st.st_mtime, (long long)st.st_atime, words, (long long)st.st_size);
                        send_msg(cfd, resp, (uint32_t)strlen(resp));
                    }
                }
            } else if (strcmp(type, "STREAM") == 0) {
                // Stream content word-by-word with 0.1s delay; client reads until STOP
                char file[128]; char ticket[256];
                int okf = (json_get_string_field(buf, "file", file, sizeof(file)) == 0);
                int okt = (json_get_string_field(buf, "ticket", ticket, sizeof(ticket)) == 0);
                if (!okf || !okt) { const char *resp = "{\"status\":\"ERR_BADREQ\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else if (ticket_validate(ticket, file, "READ", g_ss_id) != 0) { const char *resp = "{\"status\":\"ERR_NOAUTH\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                else {
                    char path[SS_PATH_MAX]; snprintf(path, sizeof(path), "%s/files/%s", g_store_root, file);
                    char *content=NULL; size_t clen=0;
                    if (read_file_into(path, &content, &clen) != 0 || !content) { const char *resp = "{\"status\":\"ERR_NOTFOUND\"}"; send_msg(cfd, resp, (uint32_t)strlen(resp)); }
                    else {
                        // Simple split by whitespace into words
                        size_t i=0;
                        while (i < clen) {
                            while (i < clen && (content[i]==' ' || content[i]=='\n' || content[i]=='\t' || content[i]=='\r')) i++;
                            size_t start=i;
                            while (i < clen && !(content[i]==' ' || content[i]=='\n' || content[i]=='\t' || content[i]=='\r')) i++;
                            if (i>start) {
                                size_t wlen = i-start; if (wlen > 256) wlen = 256;
                                char word[260]; memcpy(word, content+start, wlen); word[wlen]='\0';
                                char frame[320]; snprintf(frame, sizeof(frame), "{\"status\":\"OK\",\"word\":\"%s\"}", word);
                                if (send_msg(cfd, frame, (uint32_t)strlen(frame)) != 0) break;
                                {
                                    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000; // 0.1s
                                    select(0, NULL, NULL, NULL, &tv);
                                }
                            }
                        }
                        free(content);
                        const char *stop = "{\"status\":\"STOP\"}"; send_msg(cfd, stop, (uint32_t)strlen(stop));
                    }
                }
            } else {
                const char *resp = "{\"status\":\"ERR_BADREQ\"}";
                send_msg(cfd, resp, (uint32_t)strlen(resp));
            }
        } else {
            const char *resp = "{\"status\":\"ERR_BADREQ\"}";
            send_msg(cfd, resp, (uint32_t)strlen(resp));
        }
        free(buf);
    }
    if (ws.active) { lock_release(ws.file, ws.sentence_idx); ss_tokens_free(&ws.doc); if (ws.pre_image) free(ws.pre_image); }
    close(cfd);
    return NULL;
}

static void *data_server_thread(void *arg) {
    data_server_args_t *cfg = (data_server_args_t *)arg;
    int lfd = cfg->listen_fd;
    if (lfd < 0) {
        fprintf(stderr, "[SS] data server thread started without valid listen fd\n");
        return NULL;
    }
    g_data_lfd = lfd;
    printf("[SS] Data server listening on %d\n", cfg->data_port);
    while (g_run) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno==EINTR) continue; perror("accept"); break; }
        pthread_t th;
        conn_args_t *ca = (conn_args_t *)malloc(sizeof(conn_args_t));
        if (!ca) { close(cfd); continue; }
        ca->cfd = cfd;
        // spawn detached thread
        if (pthread_create(&th, NULL, ss_conn_handler, ca) == 0) {
            pthread_detach(th);
        } else {
            close(cfd); free(ca);
        }
    }
    close(lfd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <nm_host> <nm_port> <ss_ctrl_port> <ss_data_port> [ss_id]\n", argv[0]);
        return 1;
    }
    const char *nm_host = argv[1];
    uint16_t nm_port = (uint16_t)atoi(argv[2]);
    int ss_ctrl_port = atoi(argv[3]);
    int ss_data_port = atoi(argv[4]);
    int ss_id = argc >= 6 ? atoi(argv[5]) : ss_ctrl_port; // default id
    g_ss_id = ss_id;

    // Set per-SS data root inside project dir: ss_data/ss<id>
    snprintf(g_store_root, sizeof(g_store_root), "ss_data/ss%d", g_ss_id);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    ensure_dirs();

    // cache NM endpoint for heartbeats/commit
    snprintf(g_nm_host, sizeof(g_nm_host), "%s", nm_host);
    g_nm_port = nm_port;

    // 1) Bind the data port FIRST so we never register an unusable endpoint with NM
    int pre_lfd = tcp_listen((uint16_t)ss_data_port, 64);
    if (pre_lfd < 0) {
        // Keep perror for detailed errno while also giving a helpful hint
        perror("[SS] data listen");
        fprintf(stderr, "[SS] Hint: Another process is likely using port %d. Stop it or choose a different ss_data_port.\n", ss_data_port);
        return 1;
    }

    // 2) Now that the port is guaranteed available, register with the NM
    int fd = tcp_connect(nm_host, nm_port);
    if (fd < 0) { perror("connect NM"); close(pre_lfd); return 1; }

    char payload[256]; payload[0] = '\0';
    json_put_string_field(payload, sizeof(payload), "type", "SS_REGISTER", 1);
    json_put_int_field(payload, sizeof(payload), "ssId", ss_id, 0);
    json_put_int_field(payload, sizeof(payload), "ssCtrlPort", ss_ctrl_port, 0);
    json_put_int_field(payload, sizeof(payload), "ssDataPort", ss_data_port, 0);
    strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);

    if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); close(pre_lfd); return 1; }

    char *resp = NULL; uint32_t rlen = 0;
    if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); close(pre_lfd); return 1; }
    printf("[SS] NM response: %.*s\n", rlen, resp ? resp : "");
    free(resp);

    // Start heartbeat thread (detached)
    pthread_t th_hb;
    pthread_create(&th_hb, NULL, heartbeat_thread, NULL);
    pthread_detach(th_hb);

    // Start data server thread
    pthread_t th_data;
    data_server_args_t cfg = {.data_port = ss_data_port, .listen_fd = pre_lfd};
    pthread_create(&th_data, NULL, data_server_thread, &cfg);
    printf("[SS] Registered with NM. Running... (Ctrl+C to exit)\n");
    while (g_run) sleep(1);
    // Stop data server by closing lfd to break accept
    if (g_data_lfd >= 0) { close(g_data_lfd); g_data_lfd = -1; }
    pthread_join(th_data, NULL);
    printf("[SS] Shutting down cleanly.\n");
    return 0;
}
