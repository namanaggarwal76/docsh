#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include "../common/net_proto.h"

// Forward declaration for reuse in REPL
static int client_handle_oneshot(int argc, char **argv, const char *username);

// Case-insensitive command compare
#define CMDEQ(a,b) (strcasecmp((a),(b))==0)

// Tiny helpers
static void rstrip(char *s){ if(!s) return; size_t n=strlen(s); while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]='\0'; }
static int tokenize(const char *line, char **outv, int maxv){
    int argc=0; const char *p=line; while(*p==' '||*p=='\t') p++;
    while(*p && argc<maxv){
        while(*p==' '||*p=='\t') p++;
        if(!*p) break;
        const char *start=p; int inq=0; if(*p=='"'){ inq=1; start=++p; }
        const char *q=p; while(*q){ if(inq){ if(*q=='"'){ break; } } else { if(*q==' '||*q=='\t'||*q=='\n') break; } q++; }
        size_t len=(size_t)(q-start); char *tok=(char*)malloc(len+1); if(!tok) break; memcpy(tok,start,len); tok[len]='\0'; outv[argc++]=tok;
        p=q; if(inq && *p=='"') p++; while(*p==' '||*p=='\t') p++;
    }
    return argc;
}
static void free_tokens(char **v, int n){ for(int i=0;i<n;i++) free(v[i]); }

// Process escape sequences in a string (e.g., convert "\\n" to actual newline)
static void unescape_string(char *str) {
    if (!str) return;
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '\\' && *(src+1)) {
            src++; // skip backslash
            switch (*src) {
                case 'n': *dst++ = '\n'; break;
                case 't': *dst++ = '\t'; break;
                case 'r': *dst++ = '\r'; break;
                case '\\': *dst++ = '\\'; break;
                case '"': *dst++ = '"'; break;
                default: *dst++ = *src; break; // keep unknown escapes as-is
            }
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Simple interactive line editor with history (TTY only)
typedef struct {
    char *items[200];
    int count;
} hist_t;

static char *sdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void hist_add(hist_t *h, const char *line) {
    if (!h || !line || !line[0]) return;
    if (h->count > 0 && strcmp(h->items[h->count-1], line) == 0) return; // no dup of immediate previous
    if (h->count >= (int)(sizeof(h->items)/sizeof(h->items[0]))) {
        // drop oldest
        free(h->items[0]);
        memmove(&h->items[0], &h->items[1], (size_t)(h->count-1) * sizeof(h->items[0]));
        h->count--;
    }
    h->items[h->count] = sdup(line);
    if (h->items[h->count]) h->count++;
}

static int set_raw_mode(struct termios *orig) {
    if (!isatty(STDIN_FILENO)) return 0;
    if (tcgetattr(STDIN_FILENO, orig) != 0) return -1;
    struct termios raw = *orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    return 0;
}

static void restore_mode(const struct termios *orig) {
    if (!isatty(STDIN_FILENO)) return;
    if (orig) (void)tcsetattr(STDIN_FILENO, TCSANOW, orig);
}

static void clear_line_and_prompt(const char *prompt, size_t prev_len) {
    // Move to start, clear line, reprint prompt
    (void)prev_len;
    { ssize_t _wr = write(STDOUT_FILENO, "\r", 1); if (_wr < 0) {} }
    // ANSI clear line
    const char *clr = "\x1b[2K"; { ssize_t _wr = write(STDOUT_FILENO, clr, strlen(clr)); if (_wr < 0) {} }
    if (prompt) { ssize_t _wr = write(STDOUT_FILENO, prompt, strlen(prompt)); if (_wr < 0) {} }
}

static int read_line_tty(const char *prompt, char *out, size_t out_sz, hist_t *hist) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    if (!isatty(STDIN_FILENO)) {
        if (prompt) fputs(prompt, stdout), fflush(stdout);
        return fgets(out, (int)out_sz, stdin) ? 0 : -1;
    }
    if (prompt) fputs(prompt, stdout), fflush(stdout);
    struct termios orig; if (set_raw_mode(&orig) != 0) return -1;
    char buf[2048]; size_t len = 0; buf[0] = '\0';
    int browse = hist ? hist->count : 0; // browsing index: hist->count means new line
    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) { restore_mode(&orig); return -1; }
        if (c == '\r' || c == '\n') {
            { ssize_t _wr = write(STDOUT_FILENO, "\n", 1); if (_wr < 0) {} }
            break;
        } else if ((unsigned char)c == 127 || c == '\b') {
            if (len > 0) {
                len--; buf[len] = '\0';
                { ssize_t _wr = write(STDOUT_FILENO, "\b \b", 3); if (_wr < 0) {} }
            }
        } else if (c == 27) {
            // Escape sequence
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
            if (seq[0] == '[') {
                if (seq[1] == 'A' || seq[1] == 'B') { // Up or Down
                    if (!hist || hist->count == 0) continue;
                    if (seq[1] == 'A') { // Up
                        if (browse > 0) browse--;
                    } else { // Down
                        if (browse < hist->count) browse++;
                    }
                    clear_line_and_prompt(prompt, len);
                    if (browse >= 0 && browse < hist->count) {
                        strncpy(buf, hist->items[browse], sizeof(buf) - 1);
                        buf[sizeof(buf) - 1] = '\0'; len = strlen(buf);
                        { ssize_t _wr = write(STDOUT_FILENO, buf, len); if (_wr < 0) {} }
                    } else {
                        buf[0] = '\0'; len = 0;
                    }
                }
            }
        } else if (isprint((unsigned char)c)) {
            if (len + 1 < sizeof(buf)) {
                buf[len++] = c; buf[len] = '\0';
                { ssize_t _wr = write(STDOUT_FILENO, &c, 1); if (_wr < 0) {} }
            }
        } else {
            // ignore other controls
        }
    }
    restore_mode(&orig);
    // copy out
    {
        size_t bl = strlen(buf);
        if (bl >= out_sz) bl = out_sz - 1;
        memcpy(out, buf, bl);
        out[bl] = '\0';
    }
    // Add to history
    if (hist) hist_add(hist, out);
    return 0;
}

// Format epoch seconds to human-readable local time string
static void format_time_hr(int t, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (t <= 0) { snprintf(out, out_sz, "-"); return; }
    time_t tt = (time_t)t;
    struct tm *ptm = localtime(&tt);
    if (!ptm) { snprintf(out, out_sz, "%d", t); return; }
    strftime(out, out_sz, "%Y-%m-%d %H:%M:%S", ptm);
}

static void print_human(const char *who, const char *json) {
    if (!json) { fprintf(stderr, "%s: (no response)\n", who); return; }
    int color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;
    const char *G = color?"\x1b[32m":""; const char *R = color?"\x1b[31m":""; const char *C = color?"\x1b[36m":""; const char *Z = color?"\x1b[0m":"";
    char status[64]; status[0]='\0';
    (void)json_get_string_field(json, "status", status, sizeof(status));
    if (strcmp(status, "OK") == 0) {
    // Common OK shapes: may include body/checkpoints/etc.
        char body[8192];
        if (json_get_string_field(json, "body", body, sizeof(body)) == 0) {
            // READ-like
            printf("%s\n", body);
            return;
        }
        // VIEWREQUESTS: requests list
        if (strstr(json, "\"requests\":")) {
            if (color) printf("%sAccess Requests:%s\n", C, Z); else printf("Access Requests:\n");
            const char *p = strstr(json, "["); if (!p) { printf("(none)\n"); return; }
            printf("┌────────────────────────┬──────┐\n");
            printf("│ User                   │ Mode │\n");
            printf("├────────────────────────┼──────┤\n");
            p++;
            int count = 0;
            while (*p && *p!=']') {
                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                if (*p=='{') {
                    p++;
                    char name[256] = {0}; char mode = 'R';
                    while (*p && *p!='}') {
                        while(*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                        if (*p=='"') {
                            p++; const char *k=p; while(*p && *p!='"'){ if(*p=='\\'&&p[1]) p+=2; else p++; }
                            size_t klen=(size_t)(p-k); char key[16]; size_t ki=0; const char *ks=k; while(ki<klen&&*ks){ if(*ks=='\\'&&ks[1]){ks++; key[ki++]=*ks++;} else key[ki++]=*ks++; } key[ki]='\0'; if (*p=='"') p++;
                            while(*p && *p!=':') p++;
                            if (*p==':') p++;
                            while(*p==' '||*p=='\n'||*p=='\t') p++;
                            if (strcmp(key, "user")==0 && *p=='"') {
                                p++; const char *s=p; while(*p && *p!='"') p++; size_t n=(size_t)(p-s); if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0'; if (*p=='"') p++;
                            } else if (strcmp(key, "mode")==0 && *p=='"') {
                                p++; if (*p=='W' || *p=='R') mode=*p; while(*p && *p!='"') p++; if (*p=='"') p++;
                            } else {
                                while(*p && *p!=',' && *p!='}') p++;
                            }
                        } else { while(*p && *p!=',' && *p!='}') p++; }
                        if (*p==',') p++;
                    }
                    if (*p=='}') p++;
                    printf("│ %-22s │ %-4c │\n", name[0]?name:"?", mode);
                    count++;
                } else if (*p=='"') {
                    // Backward compat: username string only
                    p++; const char *s=p; while(*p && *p!='"') p++; size_t n=(size_t)(p-s);
                    char name[256]; if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0';
                    if (*p=='"') p++;
                    printf("│ %-22s │ %-4c │\n", name, 'R');
                    count++;
                } else break;
            }
            if (count > 0) {
                printf("└────────────────────────┴──────┘\n");
            } else {
                printf("│ (none)                 │      │\n");
                printf("└────────────────────────┴──────┘\n");
            }
            return;
        }
        // LISTTRASH: trashed items
        if (strstr(json, "\"trash\":")) {
            const char *p = strstr(json, "["); if (!p) { printf("%sOK%s\n", G, Z); return; }
            if (color) printf("%sTrash:%s\n", C, Z); else printf("Trash:\n");
            printf("┌──────────────┬────────┬───────┬──────────────────┬────────┐\n");
            printf("│ File         │ Owner  │ SS ID │ Time             │ Status │\n");
            printf("├──────────────┼────────┼───────┼──────────────────┼────────┤\n");
            p++;
            int count = 0;
            while (*p && *p != ']') {
                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                if (*p != '{') break;
                p++;
                char file[256]={0}, trashed[256]={0}, owner[128]={0}; int ssid=0, when=0;
                (void)json_get_string_field(p, "file", file, sizeof(file));
                (void)json_get_string_field(p, "trashed", trashed, sizeof(trashed));
                (void)json_get_string_field(p, "owner", owner, sizeof(owner));
                (void)json_get_int_field(p, "ssid", &ssid);
                (void)json_get_int_field(p, "when", &when);
                char tbuf[32]; format_time_hr(when, tbuf, sizeof(tbuf));
                printf("│ %-12s │ %-6s │ %5d │ %16s │ %-6s │\n", 
                       file[0]?file:"?", owner[0]?owner:"-", ssid, tbuf, 
                       trashed[0]?"yes":"no");
                while (*p && *p != '}') p++;
                if (*p == '}') p++;
                while (*p && *p!=',' && *p!=']') p++;
                if (*p == ',') p++;
                count++;
            }
            if (count > 0) {
                printf("└──────────────┴────────┴───────┴──────────────────┴────────┘\n");
            } else {
                printf("│ (empty)      │        │       │                  │        │\n");
                printf("└──────────────┴────────┴───────┴──────────────────┴────────┘\n");
            }
            return;
        }
    // LIST_USERS: users list with active and inactive sections
    if (strstr(json, "\"active\":[") || strstr(json, "\"inactive\":[")) {
            // Parse active users
            const char *p_active = strstr(json, "\"active\":");
            if (p_active) {
                p_active = strchr(p_active, '[');
                if (p_active) {
                    p_active++;
                    if (color) printf("%sActive Users:%s\n", C, Z); else printf("Active Users:\n");
                    printf("┌────────────────────────┐\n");
                    printf("│ Username               │\n");
                    printf("├────────────────────────┤\n");
                    int count = 0;
                    while (*p_active && *p_active != ']') {
                        while (*p_active==' '||*p_active=='\n'||*p_active=='\t'||*p_active==',') p_active++;
                        if (*p_active == '"') {
                            p_active++; const char *s = p_active; while (*p_active && *p_active != '"') p_active++; 
                            size_t n = (size_t)(p_active - s);
                            char name[256]; if (n >= sizeof(name)) n = sizeof(name)-1; memcpy(name, s, n); name[n] = '\0';
                            printf("│ %-22s │\n", name);
                            count++;
                            if (*p_active == '"') p_active++;
                        } else break;
                    }
                    if (count > 0) {
                        printf("└────────────────────────┘\n");
                    } else {
                        printf("│ (no active users)      │\n");
                        printf("└────────────────────────┘\n");
                    }
                }
            }
            
            // Parse inactive users
            const char *p_inactive = strstr(json, "\"inactive\":");
            if (p_inactive) {
                p_inactive = strchr(p_inactive, '[');
                if (p_inactive) {
                    p_inactive++;
                    printf("\n");
                    if (color) printf("%sInactive Users:%s\n", C, Z); else printf("Inactive Users:\n");
                    printf("┌────────────────────────┐\n");
                    printf("│ Username               │\n");
                    printf("├────────────────────────┤\n");
                    int count = 0;
                    while (*p_inactive && *p_inactive != ']') {
                        while (*p_inactive==' '||*p_inactive=='\n'||*p_inactive=='\t'||*p_inactive==',') p_inactive++;
                        if (*p_inactive == '"') {
                            p_inactive++; const char *s = p_inactive; while (*p_inactive && *p_inactive != '"') p_inactive++; 
                            size_t n = (size_t)(p_inactive - s);
                            char name[256]; if (n >= sizeof(name)) n = sizeof(name)-1; memcpy(name, s, n); name[n] = '\0';
                            printf("│ %-22s │\n", name);
                            count++;
                            if (*p_inactive == '"') p_inactive++;
                        } else break;
                    }
                    if (count > 0) {
                        printf("└────────────────────────┘\n");
                    } else {
                        printf("│ (no inactive users)    │\n");
                        printf("└────────────────────────┘\n");
                    }
                }
            }
            
            // print final ok as requested
            if (color) printf("%sok%s\n", G, Z); else printf("ok\n");
            return;
        }
    // Backward compatibility: old "users" array format (active only)
    if (strstr(json, "\"users\":[")) {
            const char *p = strstr(json, "\"users\":");
            if (!p) { if (color) printf("%sok%s\n", G, Z); else printf("ok\n"); return; }
            p = strchr(p, '[');
            if (!p) { if (color) printf("%sok%s\n", G, Z); else printf("ok\n"); return; }
            p++;
            if (color) printf("%sActive Users:%s\n", C, Z); else printf("Active Users:\n");
            printf("┌────────────────────────┐\n");
            printf("│ Username               │\n");
            printf("├────────────────────────┤\n");
            int count = 0;
            while (*p && *p != ']') {
                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                if (*p == '"') {
                    p++; const char *s = p; while (*p && *p != '"') p++; size_t n = (size_t)(p - s);
                    char name[256]; if (n >= sizeof(name)) n = sizeof(name)-1; memcpy(name, s, n); name[n] = '\0';
                    printf("│ %-22s │\n", name);
                    count++;
                    if (*p == '"') p++;
                } else break;
            }
            if (count > 0) {
                printf("└────────────────────────┘\n");
            } else {
                printf("│ (no active users)      │\n");
                printf("└────────────────────────┘\n");
            }
            // print final ok as requested
            if (color) printf("%sok%s\n", G, Z); else printf("ok\n");
            return;
        }
        // STATS summary
        if (strstr(json, "\"replicationQueue\":")) {
            int files=0, locks=0, rq=0;
            (void)json_get_int_field(json, "files", &files);
            (void)json_get_int_field(json, "activeLocks", &locks);
            (void)json_get_int_field(json, "replicationQueue", &rq);
            printf("OK: files=%d, activeLocks=%d, replicationQueue=%d\n", files, locks, rq);
            return;
        }
        // LISTCHECKPOINTS: checkpoints list
        if (strstr(json, "\"checkpoints\":")) {
            const char *p = strstr(json, "[");
            if (!p) { printf("(no checkpoints)\n"); return; }
            p++;
            int count = 0;
            while (*p && *p != ']') {
                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                if (*p=='"') {
                    p++; const char *s=p; while(*p && *p!='"') p++; size_t n=(size_t)(p-s);
                    char name[256]; if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0';
                    if (*p=='"') p++;
                    printf("│ %-22s │\n", name);
                    count++;
                } else break;
            }
            if (count == 0) printf("(no checkpoints)\n");
            return;
        }
        // INFO pretty print
        {
            char fname[256]; char owner[128]; int size=0, words=0, chars=0, mtime=0, atime=0; int got=0;
            char mod_user[128] = {0}, acc_user[128] = {0};
            int mod_time = 0, acc_time = 0;
            
            if (json_get_string_field(json, "file", fname, sizeof(fname)) == 0) got=1;
            (void)json_get_string_field(json, "owner", owner, sizeof(owner));
            (void)json_get_int_field(json, "size", &size);
            (void)json_get_int_field(json, "words", &words);
            (void)json_get_int_field(json, "chars", &chars);
            (void)json_get_int_field(json, "mtime", &mtime);
            (void)json_get_int_field(json, "atime", &atime);
            (void)json_get_string_field(json, "last_modified_user", mod_user, sizeof(mod_user));
            (void)json_get_int_field(json, "last_modified_time", &mod_time);
            (void)json_get_string_field(json, "last_accessed_user", acc_user, sizeof(acc_user));
            (void)json_get_int_field(json, "last_accessed_time", &acc_time);
            
            if (got) {
                char mtime_s[32], atime_s[32], mod_time_s[32], acc_time_s[32];
                format_time_hr(mtime, mtime_s, sizeof(mtime_s));
                format_time_hr(atime, atime_s, sizeof(atime_s));
                format_time_hr(mod_time, mod_time_s, sizeof(mod_time_s));
                format_time_hr(acc_time, acc_time_s, sizeof(acc_time_s));
                
                printf("--> File: %s\n", fname);
                printf("--> Owner: %s\n", owner[0]?owner:"-");
                printf("--> Created: %s\n", mtime_s);
                printf("--> Last Modified: %s%s%s\n", 
                       mod_time > 0 ? mod_time_s : mtime_s,
                       mod_user[0] ? " by " : "",
                       mod_user[0] ? mod_user : "");
                printf("--> Size: %d bytes\n", size);
                printf("--> Access: ");
                char acc[1024]; if (json_get_string_field(json, "access", acc, sizeof(acc))==0) printf("%s\n", acc); else printf("-\n");
                printf("--> Last Accessed: %s%s%s\n", 
                       acc_time > 0 ? acc_time_s : atime_s,
                       acc_user[0] ? " by " : "",
                       acc_user[0] ? acc_user : "");
                return;
            }
        }
        if (strstr(json, "\"files\":[")) {
            // VIEWFOLDER support: when response also contains folders, print both and return
            if (strstr(json, "\"folders\":[")) {
                // Optional header: show folder path (use ~/ for root)
                char vpath[256]; vpath[0]='\0';
                if (json_get_string_field(json, "path", vpath, sizeof(vpath)) == 0) {
                    if (strcmp(vpath, "~") == 0) printf("~/\n"); else printf("%s/\n", vpath);
                }
                // Print folders
                const char *pf = strstr(json, "\"folders\":");
                if (pf) {
                    pf = strchr(pf, '[');
                    if (pf) {
                        pf++;
                        int fcount = 0;
                        while (*pf && *pf != ']') {
                            while (*pf==' '||*pf=='\n'||*pf=='\t'||*pf==',') pf++;
                            if (*pf=='"') {
                                pf++; const char *s=pf; while(*pf && *pf!='"') pf++; size_t n=(size_t)(pf-s);
                                char name[256]; if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0'; if (*pf=='"') pf++;
                                printf("--/ %s/\n", name); // folder with trailing slash
                                fcount++;
                            } else break;
                        }
                        if (fcount==0) {
                            // optional: print nothing when no folders
                            (void)0;
                        }
                    }
                }
                // Print files
                const char *p2 = strstr(json, "\"files\":");
                if (p2) {
                    p2 = strchr(p2, '['); if (p2) { p2++;
                        int count=0;
                        while (*p2 && *p2!=']') {
                            while (*p2==' '||*p2=='\n'||*p2=='\t'||*p2==',') p2++;
                            if (*p2=='"') {
                                p2++; const char *s=p2; while(*p2 && *p2!='"') p2++; size_t n=(size_t)(p2-s);
                                char name[256]; if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0'; if (*p2=='"') p2++;
                                printf("--> %s\n", name);
                                count++;
                            } else break;
                        }
                        // If both empty, print nothing (silent like other list outputs)
                    }
                }
                return;
            }
            // VIEW basic list
            const char *p = strstr(json, "["); if (!p) { if (color) printf("%sOK%s\n", G, Z); else printf("OK\n"); return; }
            p++;
            while (*p && *p!=']') {
                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                if (*p=='"') {
                    p++; const char *s=p; while(*p && *p!='"') p++; size_t n=(size_t)(p-s);
                    char name[256]; if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0';
                    printf("--> %s\n", name);
                    if (*p=='"') p++;
                } else break;
            }
            return;
        }
        if (strstr(json, "\"details\":[")) {
            // VIEW -l: print table
            printf("┌────────────┬───────┬───────┬──────────────────┬───────┐\n");
            printf("│  Filename  │ Words │ Chars │ Last Access Time │ Owner │\n");
            printf("├────────────┼───────┼───────┼──────────────────┼───────┤\n");
            const char *p = strstr(json, "["); if (!p) { printf("---------------------------------------------------------\n"); return; }
            p++;
            while (*p && *p!=']') {
                while (*p && *p!='{') p++;
                if (*p!='{') break;
                p++;
                char name[256]={0}, owner[128]={0}; int words=0, chars=0; int atime=0; // mtime also exists but not shown here
                // naive field scan
                (void)json_get_string_field(p, "name", name, sizeof(name));
                (void)json_get_string_field(p, "owner", owner, sizeof(owner));
                (void)json_get_int_field(p, "words", &words);
                (void)json_get_int_field(p, "chars", &chars);
                (void)json_get_int_field(p, "atime", &atime);
                char atime_s[32]; format_time_hr(atime, atime_s, sizeof(atime_s));
                // print row with human-readable time
                printf("│ %-10s │ %5d │ %5d │ %16s │ %-5s │\n", name, words, chars, atime_s, owner[0]?owner:"-");
                while (*p && *p!='}') p++;
                if (*p=='}') p++;
                while (*p && *p!=',' && *p!=']') p++;
                if (*p==',') p++;
            }
            printf("---------------------------------------------------------\n");
            return;
        }
        // RENAME/MIGRATE/UNDO/CREATE/DELETE generic success
        if (color) printf("%sOK%s\n", G, Z); else printf("OK\n");
        return;
    }
    // Errors: map to human messages (avoid dumping raw JSON)
    if (strcmp(status, "ERR_NOTFOUND") == 0) {
    if (color) printf("%sERROR:%s resource not found (file or checkpoint may not exist)\n", R, Z); else printf("ERROR: resource not found (file or checkpoint may not exist)\n"); return;
    } else if (strcmp(status, "ERR_NOAUTH") == 0) {
        if (color) printf("%sERROR:%s permission denied (request access or contact owner)\n", R, Z); else printf("ERROR: permission denied (request access or contact owner)\n"); return;
    } else if (strcmp(status, "ERR_CONFLICT") == 0) {
        if (color) printf("%sERROR:%s conflict (name already exists or operation conflicts)\n", R, Z); else printf("ERROR: conflict (name already exists or operation conflicts)\n"); return;
    } else if (strcmp(status, "ERR_LOCKED") == 0) {
        if (color) printf("%sERROR:%s sentence locked by another writer; try again later\n", R, Z); else printf("ERROR: sentence locked by another writer; try again later\n"); return;
    } else if (strcmp(status, "ERR_UNAVAILABLE") == 0) {
        if (color) printf("%sERROR:%s service unavailable (no storage server reachable)\n", R, Z); else printf("ERROR: service unavailable (no storage server reachable)\n"); return;
    } else if (strcmp(status, "ERR_BADREQ") == 0) {
        char msg[256]={0};
        if (json_get_string_field(json, "msg", msg, sizeof(msg))==0 && msg[0])
            if (color) printf("%sERROR:%s bad request (%s)\n", R, Z, msg); else printf("ERROR: bad request (%s)\n", msg);
        else
            if (color) printf("%sERROR:%s bad request (invalid arguments/index or wrong sequence)\n", R, Z); else printf("ERROR: bad request (invalid arguments/index or wrong sequence)\n");
        return;
    } else if (strcmp(status, "ERR_INTERNAL") == 0) {
    if (color) printf("%sERROR:%s internal server error (I/O failure or unexpected state)\n", R, Z);
    else printf("ERROR: internal server error (I/O failure or unexpected state)\n");
    return;
    }
    // Fallback without raw JSON
    if (status[0]) { if (color) printf("%sERROR:%s server returned status '%s'\n", R, Z, status); else printf("ERROR: server returned status '%s'\n", status); }
    else { if (color) printf("%sERROR:%s unrecognized server response\n", R, Z); else printf("ERROR: unrecognized server response\n"); }
}

int main(int argc, char **argv) {
    // Interactive REPL mode only; optional host/port override
    char nm_host[64]; snprintf(nm_host, sizeof(nm_host), "%s", "127.0.0.1");
    uint16_t nm_port = 5000;
    if (argc == 3) { snprintf(nm_host, sizeof(nm_host), "%s", argv[1]); nm_port = (uint16_t)atoi(argv[2]); }
    else if (argc != 1) {
        fprintf(stderr, "[CLI] One-shot mode has been removed. Starting interactive shell. To set host/port, run: %s <host> <port>\n", argv[0]);
    }

    char username[128];
    for (;;) {
        fprintf(stdout, "Enter username: "); fflush(stdout);
        if (!fgets(username, sizeof(username), stdin)) { fprintf(stderr, "Failed to read username\n"); return 1; }
        rstrip(username);
        if (!username[0]) continue; // Empty username, try again
        
        // Say hello once
        int hfd = tcp_connect(nm_host, nm_port);
        if (hfd >= 0) {
            char hello[256]; hello[0]='\0'; json_put_string_field(hello, sizeof(hello), "type", "CLIENT_HELLO", 1); json_put_string_field(hello, sizeof(hello), "user", username, 0); strncat(hello, "}", sizeof(hello)-strlen(hello)-1);
            (void)send_msg(hfd, hello, (uint32_t)strlen(hello)); 
            char *hr=NULL; uint32_t hrl=0; 
            if (recv_msg(hfd, &hr, &hrl) == 0 && hr) {
                char status[32] = {0};
                json_get_string_field(hr, "status", status, sizeof(status));
                if (strcmp(status, "ERR_CONFLICT") == 0) {
                    fprintf(stderr, "ERROR: User already exists. Create new.\n");
                    free(hr); close(hfd);
                    continue; // Ask for username again
                }
                free(hr);
            }
            close(hfd);
            break; // Successfully registered
        } else {
            fprintf(stderr, "[CLI] Could not connect to NM at %s:%u (will try per command)\n", nm_host, (unsigned)nm_port);
            break; // Proceed even if can't connect
        }
    }
    printf("Welcome to Docs++ shell. Connected to %s:%u as %s. Type 'help' or 'exit'.\n", nm_host, (unsigned)nm_port, username);
    char line[2048];
    hist_t hist = {0};
    for (;;) {
        char prompt[256]; snprintf(prompt, sizeof(prompt), "%s@docs> ", username[0]?username:"user");
        if (read_line_tty(prompt, line, sizeof(line), &hist) != 0) break;
        rstrip(line);
    if (!line[0]) continue;
    if (CMDEQ(line, "exit") || CMDEQ(line, "quit")) break;
    if (CMDEQ(line, "clear")) {
            // Clear screen like terminal 'clear'
            fputs("\x1b[2J\x1b[H", stdout);
            fflush(stdout);
            continue;
        }
        if (CMDEQ(line, "help")) {
            printf("Commands:\n");
            printf("  VIEW [-a] [-l]\n");
            printf("  READ <file>\n");
            printf("  CREATE <file> [-r] [-w]\n");
            printf("  WRITE <file> <sentenceIndex>\n");
            printf("  UNDO <file>\n");
            printf("  INFO <file>\n");
            printf("  DELETE <file>\n");
            printf("  LISTTRASH\n");
            printf("  RESTORE <file>\n");
            printf("  EMPTYTRASH [<file>]\n");
            printf("  STREAM <file>\n");
            printf("  LIST\n");
            printf("  ADDACCESS -r|-w <file> <user>\n");
            printf("  REMACCESS <file> <user>\n");
            printf("  REQUEST_ACCESS <file> [-r|-w]\n");
            printf("  VIEWREQUESTS <file>\n");
            printf("  EXEC <file>\n");
            printf("  CREATEFOLDER <path>\n");
            printf("  VIEWFOLDER <path>\n");
            printf("  MOVE <src> <dst>\n");
            printf("  RENAME <old> <new>\n");
            printf("  CHECKPOINT <file> <name>\n");
            printf("  VIEWCHECKPOINT <file> <name>\n");
            printf("  LISTCHECKPOINTS <file>\n");
            printf("  REVERT <file> <checkpoint_tag>\n");
            printf("  CLEAR | EXIT\n");
            continue;
        }
        // Tokenize
        char *tokv[64]; int tn = tokenize(line, tokv, 64);
        if (tn <= 0) continue;
        // Build argv: program, host, port, then tokens
        char portbuf[16]; snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)nm_port);
        char *argv2[70]; int ac=0; argv2[ac++] = argv[0]; argv2[ac++] = nm_host; argv2[ac++] = portbuf;
        for (int i=0;i<tn && ac<70;i++) argv2[ac++] = tokv[i];
        (void)client_handle_oneshot(ac, argv2, username);
        free_tokens(tokv, tn);
    }
    // On exit, try to notify NM to mark this user inactive
    int lfd = tcp_connect(nm_host, nm_port);
    if (lfd >= 0) {
        char bye[256]; bye[0]='\0'; json_put_string_field(bye, sizeof(bye), "type", "LOGOUT", 1); json_put_string_field(bye, sizeof(bye), "user", username, 0); strncat(bye, "}", sizeof(bye)-strlen(bye)-1);
        (void)send_msg(lfd, bye, (uint32_t)strlen(bye)); char *br=NULL; uint32_t bl=0; (void)recv_msg(lfd, &br, &bl); if (br) free(br); close(lfd);
    }
    return 0;
}

// The existing one-shot implementation, refactored into a function
static int client_handle_oneshot(int argc, char **argv, const char *username) {
    const char *nm_host = argv[1];
    uint16_t nm_port = (uint16_t)atoi(argv[2]);
    const char *cmd = argv[3];
    int fd = tcp_connect(nm_host, nm_port);
    if (CMDEQ(cmd, "CLEAR")) {
        if (fd >= 0) close(fd);
        fputs("\x1b[2J\x1b[H", stdout);
        fflush(stdout);
        return 0;
    }
    if (fd < 0) { perror("connect NM"); return 1; }

    char payload[512]; payload[0] = '\0';
    if (CMDEQ(cmd, "LIST")) {
        json_put_string_field(payload, sizeof(payload), "type", "LIST_USERS", 1);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "LISTTRASH")) {
        json_put_string_field(payload, sizeof(payload), "type", "LISTTRASH", 1);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "RESTORE")) {
        if (argc < 5) { fprintf(stderr, "RESTORE requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "RESTORE", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "EMPTYTRASH")) {
        json_put_string_field(payload, sizeof(payload), "type", "EMPTYTRASH", 1);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        if (argc >= 5) {
            json_put_string_field(payload, sizeof(payload), "file", argv[4], 0);
        }
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "VIEW")) {
        json_put_string_field(payload, sizeof(payload), "type", "VIEW", 1);
        // collect flags from argv[4..]
        char flags[32] = {0};
        for (int i=4;i<argc && strlen(flags)+4<sizeof(flags);++i){ if (argv[i][0]=='-'){ if (flags[0]) strncat(flags, "", sizeof(flags)-strlen(flags)-1); strncat(flags, argv[i], sizeof(flags)-strlen(flags)-1);} }
        if (flags[0]) {
            json_put_string_field(payload, sizeof(payload), "flags", flags, 0);
        }
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "INFO")) {
        if (argc < 5) { fprintf(stderr, "INFO requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "INFO", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);
    } else if (CMDEQ(cmd, "EXEC")) {
        if (argc < 5) { fprintf(stderr, "EXEC requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "EXEC", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);
        // Send request then stream frames until STOP
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) != 0) { perror("send EXEC"); close(fd); return 1; }
        int started=0;
        for (;;) {
            char *fr=NULL; uint32_t fl=0; if (recv_msg(fd, &fr, &fl) != 0) { fprintf(stderr, "ERROR: EXEC stream interrupted\n"); close(fd); return 1; }
            char st[32]={0}; (void)json_get_string_field(fr, "status", st, sizeof(st));
            if (strcmp(st, "STOP") == 0) {
                int ec=0; (void)json_get_int_field(fr, "exit", &ec);
                free(fr);
                if (ec!=0) printf("\n(exit code %d)\n", ec);
                else printf("\n(done)\n");
                break;
            }
            if (!started && strcmp(st, "OK") == 0 && strstr(fr, "\"stream\":\"EXEC\"")) {
                // initial marker
                started=1; free(fr); continue;
            }
            if (strcmp(st, "OK") == 0) {
                char chunk[1200]; chunk[0]='\0';
                if (json_get_string_field(fr, "chunk", chunk, sizeof(chunk)) == 0) {
                    // print without additional formatting (already decoded newlines by server escaping rules -> contains literal \n sequences we need to convert?)
                    // Replace escaped newlines with real newlines for nicer output
                    for (size_t i=0; i<strlen(chunk); ++i) {
                        if (chunk[i]=='\\' && chunk[i+1]=='n') { fputc('\n', stdout); i++; } else if (chunk[i]=='\\' && chunk[i+1]=='r') { /* ignore carriage */ i++; } else { fputc(chunk[i], stdout); }
                    }
                    fflush(stdout);
                }
                free(fr); continue;
            }
            // Any other status -> print via print_human and abort
            print_human("NM", fr); free(fr); break;
        }
        close(fd); return 0;
    } else if (CMDEQ(cmd, "READ")) {
        if (argc < 5) { fprintf(stderr, "read requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        // First LOOKUP
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "READ", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0;
    if (recv_msg(fd, &resp, &rlen) < 0) { fprintf(stderr, "ERROR: failed to receive LOOKUP from NM\n"); close(fd); return 1; }
        // Check status first
        char st[32]={0}; (void)json_get_string_field(resp, "status", st, sizeof(st));
        if (st[0] && strcmp(st, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        // Parse ssDataPort
    int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
    int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd);
    if (!ok || dport <= 0) { fprintf(stderr, "ERROR: LOOKUP failed (no storage server available)\n"); return 1; }
        // Connect to SS and READ
        int sfd = tcp_connect(ssaddr, (uint16_t)dport);
        if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "READ", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
    json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send READ"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2len = 0;
        if (recv_msg(sfd, &r2, &r2len) < 0) { perror("recv READ"); close(sfd); return 1; }
    print_human("SS", r2);
        free(r2); close(sfd);
        return 0;
    } else if (CMDEQ(cmd, "STREAM")) {
        if (argc < 5) { fprintf(stderr, "STREAM requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        // LOOKUP READ
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "READ", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        // Check status first
        char st[32]={0}; (void)json_get_string_field(resp, "status", st, sizeof(st));
        if (st[0] && strcmp(st, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
    int dport=0; char ssaddr[64]={0}; char ticket[256]={0}; int ok = (json_get_int_field(resp, "ssDataPort", &dport)==0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr))==0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket))==0); free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "ERROR: LOOKUP failed (no storage server available)\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "STREAM", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { perror("send STREAM"); close(sfd); return 1; }
        // receive frames until STOP
        int first=1; for (;;) {
            char *fr=NULL; uint32_t fl=0; if (recv_msg(sfd, &fr, &fl) != 0 || !fr || fl==0) { fprintf(stderr, "\nERROR: service unavailable (stream interrupted)\n"); close(sfd); return 1; }
            char st[16]={0}; (void)json_get_string_field(fr, "status", st, sizeof(st));
            if (strcmp(st, "STOP")==0) { free(fr); break; }
            char word[260]={0}; if (json_get_string_field(fr, "word", word, sizeof(word)) == 0) {
                if (!first) { printf(" "); }
                first = 0;
                printf("%s", word); fflush(stdout);
            }
            free(fr);
        }
        printf("\n"); close(sfd); return 0;
    } else if (CMDEQ(cmd, "CREATE")) {
        if (argc < 5) { fprintf(stderr, "create requires <file> [-r] [-w]\n"); close(fd); return 1; }
        const char *file = argv[4];
        int pubR = 0, pubW = 0;
        for (int i = 5; i < argc; ++i) {
            if (strcmp(argv[i], "-r") == 0) pubR = 1;
            else if (strcmp(argv[i], "-w") == 0) pubW = 1;
        }
        json_put_string_field(payload, sizeof(payload), "type", "CREATE", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        if (pubR) json_put_int_field(payload, sizeof(payload), "publicRead", 1, 0);
        if (pubW) json_put_int_field(payload, sizeof(payload), "publicWrite", 1, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "DELETE")) {
        if (argc < 5) { fprintf(stderr, "delete requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "DELETE", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "WRITE")) {
        if (argc < 6) { fprintf(stderr, "WRITE requires <file> <sentenceIndex>\n"); close(fd); return 1; }
        const char *file = argv[4]; int sidx = atoi(argv[5]);
        // LOOKUP WRITE
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "WRITE", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) != 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) != 0) { perror("recv"); close(fd); return 1; }
        char st_lookup[32]={0}; (void)json_get_string_field(resp, "status", st_lookup, sizeof(st_lookup));
        if (st_lookup[0] && strcmp(st_lookup, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0}; int ok=(json_get_int_field(resp, "ssDataPort", &dport)==0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr))==0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket))==0);
        if (!ok || dport<=0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        free(resp); close(fd);
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd<0){ perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "BEGIN_WRITE", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_int_field(req, sizeof(req), "sentenceIndex", sidx, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { perror("send BEGIN_WRITE"); close(sfd); return 1; }
        char *r1=NULL; uint32_t r1l=0; if (recv_msg(sfd, &r1, &r1l) != 0 || !r1 || !strstr(r1, "\"status\":\"OK\"")) { print_human("SS", r1); free(r1); close(sfd); return 1; }
        free(r1);
        // interactive APPLYs
        fprintf(stdout, "Enter <word_index> <content> lines; finish with ETIRW on its own line\n"); fflush(stdout);
        char line[1024];
        while (fgets(line, sizeof(line), stdin)) {
            if (strncmp(line, "ETIRW", 5) == 0) break;
            // parse first token as integer, rest as content
            char *sp = line; while (*sp==' '||*sp=='\t') sp++;
            if (!*sp) continue;
            char *end=NULL; long widx = strtol(sp, &end, 10);
            if (end==sp) { fprintf(stderr, "ERROR: invalid input, expected '<word_index> <content>'\n"); continue; }
            while (*end==' '||*end=='\t') end++;
            if (!*end) { fprintf(stderr, "ERROR: missing content\n"); continue; }
            // trim trailing newline
            size_t clen = strlen(end); if (clen && end[clen-1]=='\n') end[--clen]='\0';
            // Process escape sequences
            unescape_string(end);
            char areq[1200]; areq[0]='\0'; json_put_string_field(areq, sizeof(areq), "type", "APPLY", 1); json_put_int_field(areq, sizeof(areq), "wordIndex", (int)widx, 0); json_put_string_field(areq, sizeof(areq), "content", end, 0); strncat(areq, "}", sizeof(areq)-strlen(areq)-1);
            if (send_msg(sfd, areq, (uint32_t)strlen(areq)) != 0) { perror("send APPLY"); break; }
            char *ar=NULL; uint32_t al=0; if (recv_msg(sfd, &ar, &al) != 0) { perror("recv APPLY"); break; } print_human("SS", ar); free(ar);
        }
        // end write
        char ereq[64]; ereq[0]='\0'; json_put_string_field(ereq, sizeof(ereq), "type", "END_WRITE", 1); strncat(ereq, "}", sizeof(ereq)-strlen(ereq)-1);
        if (send_msg(sfd, ereq, (uint32_t)strlen(ereq)) == 0) { char *er=NULL; uint32_t el=0; if (recv_msg(sfd, &er, &el) == 0) print_human("SS", er); free(er);} close(sfd); return 0;
    } else if (CMDEQ(cmd, "RENAME")) {
        if (argc < 6) { fprintf(stderr, "rename requires <old> <new>\n"); close(fd); return 1; }
        const char *of = argv[4]; const char *nf = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "RENAME", 1);
        json_put_string_field(payload, sizeof(payload), "file", of, 0);
        json_put_string_field(payload, sizeof(payload), "newFile", nf, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "UNDO")) {
        if (argc < 5) { fprintf(stderr, "undo requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        // LOOKUP for UNDO
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "UNDO", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0;
    if (recv_msg(fd, &resp, &rlen) < 0) { fprintf(stderr, "ERROR: failed to receive LOOKUP from NM\n"); close(fd); return 1; }
        char st_lookup_undo[32]={0}; (void)json_get_string_field(resp, "status", st_lookup_undo, sizeof(st_lookup_undo));
        if (st_lookup_undo[0] && strcmp(st_lookup_undo, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        if (!ok || dport <= 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        free(resp); close(fd);
        int sfd = tcp_connect(ssaddr, (uint16_t)dport);
        if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "UNDO", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send UNDO"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2l = 0;
        if (recv_msg(sfd, &r2, &r2l) < 0) { perror("recv UNDO"); close(sfd); return 1; }
    print_human("SS", r2);
        free(r2); close(sfd);
        return 0;
    } else if (CMDEQ(cmd, "REVERT")) {
        if (argc < 6) { fprintf(stderr, "revert requires <file> <checkpoint_tag>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *ver_or_name = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "REVERT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
    char *resp = NULL; uint32_t rlen = 0; if (recv_msg(fd, &resp, &rlen) < 0) { fprintf(stderr, "ERROR: failed to receive LOOKUP from NM\n"); close(fd); return 1; }
        char st_lookup_revert[32]={0}; (void)json_get_string_field(resp, "status", st_lookup_revert, sizeof(st_lookup_revert));
        if (st_lookup_revert[0] && strcmp(st_lookup_revert, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        if (!ok || dport <= 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        free(resp); close(fd);
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "REVERT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        // Always treat second argument as checkpoint tag
        json_put_string_field(req, sizeof(req), "name", ver_or_name, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send REVERT"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2l = 0; if (recv_msg(sfd, &r2, &r2l) < 0) { perror("recv REVERT"); close(sfd); return 1; }
    print_human("SS", r2);
        free(r2); close(sfd);
        return 0;
    } else if (CMDEQ(cmd, "ADDACCESS")) {
        if (argc < 7) { fprintf(stderr, "ADDACCESS requires -r|-w <file> <user>\n"); close(fd); return 1; }
        const char *flag = argv[4]; const char *file = argv[5]; const char *user = argv[6]; 
        const char *mode = (strcmp(flag, "-w")==0 || strcmp(flag, "-rw")==0 || strcmp(flag, "-wr")==0)?"RW":"R";
        json_put_string_field(payload, sizeof(payload), "type", "ADDACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", user, 0);
        json_put_string_field(payload, sizeof(payload), "mode", mode, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "REMACCESS")) {
        if (argc < 6) { fprintf(stderr, "remaccess requires <file> <user>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *user = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "REMACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", user, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "CREATEFOLDER")) {
        if (argc < 5) { fprintf(stderr, "CREATEFOLDER requires <path>\n"); close(fd); return 1; }
        const char *path = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "CREATEFOLDER", 1);
        json_put_string_field(payload, sizeof(payload), "path", path, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "VIEWFOLDER")) {
        if (argc < 5) { fprintf(stderr, "VIEWFOLDER requires <path>\n"); close(fd); return 1; }
        const char *path = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "VIEWFOLDER", 1);
        json_put_string_field(payload, sizeof(payload), "path", path, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "MOVE")) {
        if (argc < 6) { fprintf(stderr, "MOVE requires <src> <dst>\n"); close(fd); return 1; }
        const char *src = argv[4]; const char *dst = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "MOVE", 1);
        json_put_string_field(payload, sizeof(payload), "src", src, 0);
        json_put_string_field(payload, sizeof(payload), "dst", dst, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "REQUEST_ACCESS")) {
        if (argc < 5) { fprintf(stderr, "REQUEST_ACCESS requires <file> [ -r | -w ]\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *mode = "R";
        if (argc >= 6) {
            if (strcmp(argv[5], "-w") == 0 || strcmp(argv[5], "-rw") == 0 || strcmp(argv[5], "-wr") == 0) mode = "W";
            else if (strcmp(argv[5], "-r") == 0) mode = "R";
        }
        json_put_string_field(payload, sizeof(payload), "type", "REQUEST_ACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        json_put_string_field(payload, sizeof(payload), "mode", mode, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "VIEWREQUESTS")) {
        if (argc < 5) { fprintf(stderr, "VIEWREQUESTS requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "VIEWREQUESTS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (CMDEQ(cmd, "CHECKPOINT")) {
        if (argc < 6) { fprintf(stderr, "CHECKPOINT requires <file> <name>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *name = argv[5];
        // LOOKUP for CHECKPOINT
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "CHECKPOINT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        char st_lookup_checkpoint[32]={0}; (void)json_get_string_field(resp, "status", st_lookup_checkpoint, sizeof(st_lookup_checkpoint));
        if (st_lookup_checkpoint[0] && strcmp(st_lookup_checkpoint, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        if (!ok || dport<=0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        free(resp); close(fd);
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "CHECKPOINT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        json_put_string_field(req, sizeof(req), "name", name, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send CHECKPOINT"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv CHECKPOINT"); close(sfd); return 1; }
        print_human("SS", r); free(r); close(sfd); return 0;
    } else if (CMDEQ(cmd, "LISTCHECKPOINTS")) {
        if (argc < 5) { fprintf(stderr, "LISTCHECKPOINTS requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "LISTCHECKPOINTS", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        char st_lookup_listcp[32]={0}; (void)json_get_string_field(resp, "status", st_lookup_listcp, sizeof(st_lookup_listcp));
        if (st_lookup_listcp[0] && strcmp(st_lookup_listcp, "OK") != 0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        if (!ok || dport<=0) { print_human("NM", resp); free(resp); close(fd); return 1; }
        free(resp); close(fd);
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "LISTCHECKPOINTS", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send LISTCHECKPOINTS"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv LISTCHECKPOINTS"); close(sfd); return 1; }
        print_human("SS", r); free(r); close(sfd); return 0;
    } else if (CMDEQ(cmd, "VIEWCHECKPOINT")) {
        if (argc < 6) { fprintf(stderr, "VIEWCHECKPOINT requires <file> <name>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *name = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "VIEWCHECKPOINT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
    free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "ERROR: LOOKUP failed (no storage server available)\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "VIEWCHECKPOINT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        json_put_string_field(req, sizeof(req), "name", name, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send VIEWCHECKPOINT"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv VIEWCHECKPOINT"); close(sfd); return 1; }
        print_human("SS", r); free(r); close(sfd); return 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        close(fd); return 1;
    }

    if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }

    char *resp = NULL; uint32_t rlen = 0;
    if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
    print_human("NM", resp);
    free(resp);

    close(fd);
    return 0;
}
