#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>

#include "../common/net_proto.h"

// Forward declaration for reuse in REPL
static int client_handle_oneshot(int argc, char **argv, const char *username);

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

static void print_human(const char *who, const char *json) {
    if (!json) { fprintf(stderr, "%s: (no response)\n", who); return; }
    char status[64]; status[0]='\0';
    (void)json_get_string_field(json, "status", status, sizeof(status));
    if (strcmp(status, "OK") == 0) {
        // Common OK shapes: may include body/versions/etc.
        char body[8192];
        if (json_get_string_field(json, "body", body, sizeof(body)) == 0) {
            // READ-like
            printf("%s\n", body);
            return;
        }
        // HISTORY: versions list
        if (strstr(json, "\"versions\":")) {
            printf("OK: versions listed\n");
            return;
        }
        // VIEWREQUESTS: requests list
        if (strstr(json, "\"requests\":[")) {
            printf("Requests:\n");
            const char *p = strstr(json, "["); if (!p) { printf("(none)\n"); return; }
            p++;
            int count = 0;
            while (*p && *p!=']') {
                while (*p==' '||*p=='\n'||*p=='\t'||*p==',') p++;
                if (*p=='"') {
                    p++; const char *s=p; while(*p && *p!='"') p++; size_t n=(size_t)(p-s);
                    char name[256]; if (n>=sizeof(name)) n=sizeof(name)-1; memcpy(name,s,n); name[n]='\0';
                    printf("- %s\n", name); count++;
                    if (*p=='"') p++;
                } else break;
            }
            if (count==0) printf("(none)\n");
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
            printf("OK: checkpoints listed\n");
            return;
        }
        // EXEC output
        {
            char out[8192];
            if (json_get_string_field(json, "output", out, sizeof(out)) == 0) {
                printf("%s\n", out);
                return;
            }
        }
        // INFO pretty print
        {
            char fname[256]; char owner[128]; int size=0, words=0, chars=0, mtime=0, atime=0; int got=0;
            if (json_get_string_field(json, "file", fname, sizeof(fname)) == 0) got=1;
            (void)json_get_string_field(json, "owner", owner, sizeof(owner));
            (void)json_get_int_field(json, "size", &size);
            (void)json_get_int_field(json, "words", &words);
            (void)json_get_int_field(json, "chars", &chars);
            (void)json_get_int_field(json, "mtime", &mtime);
            (void)json_get_int_field(json, "atime", &atime);
            if (got) {
                printf("--> File: %s\n", fname);
                printf("--> Owner: %s\n", owner[0]?owner:"-");
                printf("--> Created: %d\n", mtime);
                printf("--> Last Modified: %d\n", mtime);
                printf("--> Size: %d bytes\n", size);
                printf("--> Access: ");
                char acc[1024]; if (json_get_string_field(json, "access", acc, sizeof(acc))==0) printf("%s\n", acc); else printf("-\n");
                printf("--> Last Accessed: %d\n", atime);
                return;
            }
        }
        if (strstr(json, "\"files\":[")) {
            // VIEW basic list
            const char *p = strstr(json, "["); if (!p) { printf("OK\n"); return; }
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
            printf("---------------------------------------------------------\n");
            printf("|  Filename  | Words | Chars | Last Access Time | Owner |\n");
            printf("|------------|-------|-------|------------------|-------|\n");
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
                // print row; time as epoch for simplicity
                printf("| %-10s | %5d | %5d | %16d | %-5s |\n", name, words, chars, atime, owner[0]?owner:"-");
                while (*p && *p!='}') p++;
                if (*p=='}') p++;
                while (*p && *p!=',' && *p!=']') p++;
                if (*p==',') p++;
            }
            printf("---------------------------------------------------------\n");
            return;
        }
        // RENAME/MIGRATE/UNDO/CREATE/DELETE generic success
        printf("OK\n");
        return;
    }
    // Errors: map to human messages
    if (strcmp(status, "ERR_NOTFOUND") == 0) {
        printf("ERROR: not found\n"); return;
    } else if (strcmp(status, "ERR_NOAUTH") == 0) {
        printf("ERROR: permission denied\n"); return;
    } else if (strcmp(status, "ERR_CONFLICT") == 0) {
        printf("ERROR: conflict\n"); return;
    } else if (strcmp(status, "ERR_LOCKED") == 0) {
        printf("ERROR: locked\n"); return;
    } else if (strcmp(status, "ERR_UNAVAILABLE") == 0) {
        printf("ERROR: service unavailable\n"); return;
    } else if (strcmp(status, "ERR_BADREQ") == 0) {
        printf("ERROR: bad request\n"); return;
    } else if (strcmp(status, "ERR_INTERNAL") == 0) {
        printf("ERROR: internal server error\n"); return;
    }
    // Fallback
    printf("%s\n", json);
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
    fprintf(stdout, "Enter username: "); fflush(stdout);
    if (!fgets(username, sizeof(username), stdin)) { fprintf(stderr, "Failed to read username\n"); return 1; }
    rstrip(username);
    // Say hello once
    int hfd = tcp_connect(nm_host, nm_port);
    if (hfd >= 0) {
        char hello[256]; hello[0]='\0'; json_put_string_field(hello, sizeof(hello), "type", "CLIENT_HELLO", 1); json_put_string_field(hello, sizeof(hello), "user", username, 0); strncat(hello, "}", sizeof(hello)-strlen(hello)-1);
        (void)send_msg(hfd, hello, (uint32_t)strlen(hello)); char *hr=NULL; uint32_t hrl=0; (void)recv_msg(hfd, &hr, &hrl); free(hr); close(hfd);
    } else {
        fprintf(stderr, "[CLI] Could not connect to NM at %s:%u (will try per command)\n", nm_host, (unsigned)nm_port);
    }
    printf("Welcome to Docs++ shell. Connected to %s:%u as %s. Type 'help' or 'exit'.\n", nm_host, (unsigned)nm_port, username);
    char line[2048];
    hist_t hist = {0};
    for (;;) {
        char prompt[256]; snprintf(prompt, sizeof(prompt), "%s@docs> ", username[0]?username:"user");
        if (read_line_tty(prompt, line, sizeof(line), &hist) != 0) break;
        rstrip(line);
        if (!line[0]) continue;
        if (strcmp(line, "exit")==0 || strcmp(line, "quit")==0) break;
        if (strcmp(line, "help")==0) {
            printf("Commands: VIEW [-a] [-l] | READ <file> | CREATE <file> [-r] [-w] | WRITE <file> <sentenceIndex> | UNDO <file> | INFO <file> | DELETE <file> | STREAM <file> | LIST | ADDACCESS -R|-W <file> <user> | REMACCESS <file> <user> | REQUEST_ACCESS <file> [-R|-W] | VIEWREQUESTS <file> | APPROVE_ACCESS <file> -R|-W <user> | DENY_ACCESS <file> <user> | STATS | EXEC <file>\n");
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
    if (fd < 0) { perror("connect NM"); return 1; }

    char payload[512]; payload[0] = '\0';
    if (strcmp(cmd, "hello") == 0) {
        // no-op, already sent
        json_put_string_field(payload, sizeof(payload), "type", "LIST_USERS", 1);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "list-users") == 0 || strcmp(cmd, "list") == 0) {
        json_put_string_field(payload, sizeof(payload), "type", "LIST_USERS", 1);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "view") == 0 || strcmp(cmd, "VIEW") == 0) {
        json_put_string_field(payload, sizeof(payload), "type", "VIEW", 1);
        // collect flags from argv[4..]
        char flags[32] = {0};
        for (int i=4;i<argc && strlen(flags)+4<sizeof(flags);++i){ if (argv[i][0]=='-'){ if (flags[0]) strncat(flags, "", sizeof(flags)-strlen(flags)-1); strncat(flags, argv[i], sizeof(flags)-strlen(flags)-1);} }
        if (flags[0]) {
            json_put_string_field(payload, sizeof(payload), "flags", flags, 0);
        }
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "INFO") == 0 || strcmp(cmd, "info") == 0) {
        if (argc < 5) { fprintf(stderr, "INFO requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "INFO", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);
    } else if (strcmp(cmd, "EXEC") == 0 || strcmp(cmd, "exec") == 0) {
        if (argc < 5) { fprintf(stderr, "EXEC requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "EXEC", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload)-strlen(payload)-1);
    } else if (strcmp(cmd, "dir-set") == 0) {
        if (argc < 6) { fprintf(stderr, "dir-set requires <file> <ssId>\n"); close(fd); return 1; }
        const char *file = argv[4]; int ssid = atoi(argv[5]);
        json_put_string_field(payload, sizeof(payload), "type", "DIR_SET", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_int_field(payload, sizeof(payload), "ssId", ssid, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "lookup-read") == 0) {
        if (argc < 5) { fprintf(stderr, "lookup-read requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "READ", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "read") == 0 || strcmp(cmd, "READ") == 0) {
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
    if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
    fprintf(stderr, "[CLI] LOOKUP response: %.*s\n", rlen, resp ? resp : "");
        // Parse ssDataPort
    int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
    int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd);
        if (!ok || dport <= 0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
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
    } else if (strcmp(cmd, "STREAM") == 0 || strcmp(cmd, "stream") == 0) {
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
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0}; int ok = (json_get_int_field(resp, "ssDataPort", &dport)==0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr))==0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket))==0); free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "STREAM", 1); json_put_string_field(req, sizeof(req), "file", file, 0); json_put_string_field(req, sizeof(req), "ticket", ticket, 0); strncat(req, "}", sizeof(req)-strlen(req)-1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) != 0) { perror("send STREAM"); close(sfd); return 1; }
        // receive frames until STOP
        int first=1; for (;;) {
            char *fr=NULL; uint32_t fl=0; if (recv_msg(sfd, &fr, &fl) != 0) { fprintf(stderr, "\nERROR: streaming interrupted\n"); close(sfd); return 1; }
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
    } else if (strcmp(cmd, "read-noticket") == 0) {
        if (argc < 5) { fprintf(stderr, "read-noticket requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        // Use LOOKUP to learn SS endpoint, but intentionally omit ticket in READ
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "READ", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0;
        if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        fprintf(stderr, "[CLI] LOOKUP response: %.*s\n", rlen, resp ? resp : "");
        int dport = 0; char ssaddr[64] = {0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0);
        free(resp); close(fd);
        if (!ok || dport <= 0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        // Connect to SS and READ without ticket
        int sfd = tcp_connect(ssaddr, (uint16_t)dport);
        if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "READ", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send READ"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2len = 0;
        if (recv_msg(sfd, &r2, &r2len) < 0) { perror("recv READ"); close(sfd); return 1; }
    print_human("SS", r2);
        free(r2); close(sfd);
        return 0;
    } else if (strcmp(cmd, "create") == 0 || strcmp(cmd, "CREATE") == 0) {
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
    } else if (strcmp(cmd, "delete") == 0 || strcmp(cmd, "DELETE") == 0) {
        if (argc < 5) { fprintf(stderr, "delete requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "DELETE", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "WRITE") == 0) {
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
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0}; int ok=(json_get_int_field(resp, "ssDataPort", &dport)==0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr))==0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket))==0); free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
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
            char areq[1200]; areq[0]='\0'; json_put_string_field(areq, sizeof(areq), "type", "APPLY", 1); json_put_int_field(areq, sizeof(areq), "wordIndex", (int)widx, 0); json_put_string_field(areq, sizeof(areq), "content", end, 0); strncat(areq, "}", sizeof(areq)-strlen(areq)-1);
            if (send_msg(sfd, areq, (uint32_t)strlen(areq)) != 0) { perror("send APPLY"); break; }
            char *ar=NULL; uint32_t al=0; if (recv_msg(sfd, &ar, &al) != 0) { perror("recv APPLY"); break; } print_human("SS", ar); free(ar);
        }
        // end write
        char ereq[64]; ereq[0]='\0'; json_put_string_field(ereq, sizeof(ereq), "type", "END_WRITE", 1); strncat(ereq, "}", sizeof(ereq)-strlen(ereq)-1);
        if (send_msg(sfd, ereq, (uint32_t)strlen(ereq)) == 0) { char *er=NULL; uint32_t el=0; if (recv_msg(sfd, &er, &el) == 0) print_human("SS", er); free(er);} close(sfd); return 0;
    } else if (strcmp(cmd, "write") == 0) {
        if (argc < 8) { fprintf(stderr, "write requires <file> <sentenceIndex> <wordIndex> <content>\n"); close(fd); return 1; }
        const char *file = argv[4];
        int sidx = atoi(argv[5]);
        int widx = atoi(argv[6]);
        const char *content = argv[7];
        // LOOKUP for WRITE
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "WRITE", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0;
    if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
    fprintf(stderr, "[CLI] LOOKUP response: %.*s\n", rlen, resp ? resp : "");
    int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
    int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd);
        if (!ok || dport <= 0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport);
        if (sfd < 0) { perror("connect SS"); return 1; }
        // BEGIN_WRITE
        char req[512]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "BEGIN_WRITE", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_int_field(req, sizeof(req), "sentenceIndex", sidx, 0);
    json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send BEGIN_WRITE"); close(sfd); return 1; }
        char *r1 = NULL; uint32_t r1l = 0; if (recv_msg(sfd, &r1, &r1l) < 0) { perror("recv BEGIN_WRITE"); close(sfd); return 1; }
    fprintf(stderr, "[CLI] BEGIN_WRITE response: %.*s\n", r1l, r1 ? r1 : "");
    if (!r1 || !strstr(r1, "\"status\":\"OK\"")) { free(r1); close(sfd); return 1; }
        free(r1);
        // APPLY single change
        req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "APPLY", 1);
        json_put_int_field(req, sizeof(req), "wordIndex", widx, 0);
        json_put_string_field(req, sizeof(req), "content", content, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send APPLY"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2l = 0; if (recv_msg(sfd, &r2, &r2l) < 0) { perror("recv APPLY"); close(sfd); return 1; }
    fprintf(stderr, "[CLI] APPLY response: %.*s\n", r2l, r2 ? r2 : "");
    if (!r2 || !strstr(r2, "\"status\":\"OK\"")) { free(r2); close(sfd); return 1; }
        free(r2);
        // END_WRITE
        req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "END_WRITE", 1);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send END_WRITE"); close(sfd); return 1; }
    char *r3 = NULL; uint32_t r3l = 0; if (recv_msg(sfd, &r3, &r3l) < 0) { perror("recv END_WRITE"); close(sfd); return 1; }
    fprintf(stderr, "[CLI] END_WRITE response: %.*s\n", r3l, r3 ? r3 : "");
    printf("[CLI] SS response: %.*s\n", r3l, r3 ? r3 : "");
        free(r3); close(sfd);
        return 0;
    } else if (strcmp(cmd, "rename") == 0) {
        if (argc < 6) { fprintf(stderr, "rename requires <old> <new>\n"); close(fd); return 1; }
        const char *of = argv[4]; const char *nf = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "RENAME", 1);
        json_put_string_field(payload, sizeof(payload), "file", of, 0);
        json_put_string_field(payload, sizeof(payload), "newFile", nf, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "migrate") == 0) {
        if (argc < 6) { fprintf(stderr, "migrate requires <file> <targetSsId>\n"); close(fd); return 1; }
        const char *file = argv[4]; int target = atoi(argv[5]);
        json_put_string_field(payload, sizeof(payload), "type", "MIGRATE", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_int_field(payload, sizeof(payload), "targetSsId", target, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "put-direct") == 0) {
        if (argc < 7) { fprintf(stderr, "put-direct requires <ssDataPort> <file> <body>\n"); close(fd); return 1; }
        int dport = atoi(argv[4]); const char *file = argv[5]; const char *body = argv[6];
        // Close NM fd; we will connect to SS directly
        close(fd);
        int sfd = tcp_connect("127.0.0.1", (uint16_t)dport);
        if (sfd < 0) { perror("connect SS"); return 1; }
        char req[1024]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "PUT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "body", body, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send PUT"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv PUT"); close(sfd); return 1; }
        printf("[CLI] SS response: %.*s\n", rl, r?r:""); free(r); close(sfd); return 0;
    } else if (strcmp(cmd, "undo") == 0) {
        if (argc < 5) { fprintf(stderr, "undo requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        // LOOKUP for UNDO
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "UNDO", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0;
        if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        fprintf(stderr, "[CLI] LOOKUP response: %.*s\n", rlen, resp ? resp : "");
        int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd);
        if (!ok || dport <= 0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
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
    } else if (strcmp(cmd, "history") == 0) {
        if (argc < 5) { fprintf(stderr, "history requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "HISTORY", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        fprintf(stderr, "[CLI] LOOKUP response: %.*s\n", rlen, resp ? resp : "");
        int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd);
        if (!ok || dport <= 0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "HISTORY", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send HISTORY"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2l = 0; if (recv_msg(sfd, &r2, &r2l) < 0) { perror("recv HISTORY"); close(sfd); return 1; }
    print_human("SS", r2);
        free(r2); close(sfd);
        return 0;
    } else if (strcmp(cmd, "revert") == 0) {
        if (argc < 6) { fprintf(stderr, "revert requires <file> <version>\n"); close(fd); return 1; }
        const char *file = argv[4]; int ver = atoi(argv[5]);
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "REVERT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp = NULL; uint32_t rlen = 0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        fprintf(stderr, "[CLI] LOOKUP response: %.*s\n", rlen, resp ? resp : "");
        int dport = 0; char ssaddr[64] = {0}; char ticket[256] = {0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd);
        if (!ok || dport <= 0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0] = '\0';
        json_put_string_field(req, sizeof(req), "type", "REVERT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_int_field(req, sizeof(req), "version", ver, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send REVERT"); close(sfd); return 1; }
        char *r2 = NULL; uint32_t r2l = 0; if (recv_msg(sfd, &r2, &r2l) < 0) { perror("recv REVERT"); close(sfd); return 1; }
    print_human("SS", r2);
        free(r2); close(sfd);
        return 0;
    } else if (strcmp(cmd, "ADDACCESS") == 0) {
        if (argc < 7) { fprintf(stderr, "ADDACCESS requires -R|-W <file> <user>\n"); close(fd); return 1; }
        const char *flag = argv[4]; const char *file = argv[5]; const char *user = argv[6]; const char *mode = (strcmp(flag, "-W")==0?"RW":"R");
        json_put_string_field(payload, sizeof(payload), "type", "ADDACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", user, 0);
        json_put_string_field(payload, sizeof(payload), "mode", mode, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "addaccess") == 0) {
        if (argc < 7) { fprintf(stderr, "addaccess requires <file> <user> <R|W|RW>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *user = argv[5]; const char *mode = argv[6];
        json_put_string_field(payload, sizeof(payload), "type", "ADDACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", user, 0);
        json_put_string_field(payload, sizeof(payload), "mode", mode, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "REMACCESS") == 0 || strcmp(cmd, "remaccess") == 0) {
        if (argc < 6) { fprintf(stderr, "remaccess requires <file> <user>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *user = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "REMACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", user, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 5) { fprintf(stderr, "mkdir requires <path>\n"); close(fd); return 1; }
        const char *path = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "CREATEFOLDER", 1);
        json_put_string_field(payload, sizeof(payload), "path", path, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "lsf") == 0) {
        if (argc < 5) { fprintf(stderr, "lsf requires <path>\n"); close(fd); return 1; }
        const char *path = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "VIEWFOLDER", 1);
        json_put_string_field(payload, sizeof(payload), "path", path, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "mv") == 0) {
        if (argc < 6) { fprintf(stderr, "mv requires <src> <dst>\n"); close(fd); return 1; }
        const char *src = argv[4]; const char *dst = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "MOVE", 1);
        json_put_string_field(payload, sizeof(payload), "src", src, 0);
        json_put_string_field(payload, sizeof(payload), "dst", dst, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "REQUEST_ACCESS") == 0 || strcmp(cmd, "request-access") == 0) {
        if (argc < 5) { fprintf(stderr, "REQUEST_ACCESS requires <file> [ -R | -W ]\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *mode = "R";
        if (argc >= 6) {
            if (strcmp(argv[5], "-W") == 0) mode = "W";
            else if (strcmp(argv[5], "-R") == 0) mode = "R";
        }
        json_put_string_field(payload, sizeof(payload), "type", "REQUEST_ACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        json_put_string_field(payload, sizeof(payload), "mode", mode, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "VIEWREQUESTS") == 0 || strcmp(cmd, "viewrequests") == 0) {
        if (argc < 5) { fprintf(stderr, "VIEWREQUESTS requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "VIEWREQUESTS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "APPROVE_ACCESS") == 0 || strcmp(cmd, "approve-access") == 0) {
        if (argc < 7) { fprintf(stderr, "APPROVE_ACCESS requires <file> -R|-W <user>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *flag = argv[5]; const char *user = argv[6]; const char *mode = (strcmp(flag, "-W")==0?"W":"R");
        json_put_string_field(payload, sizeof(payload), "type", "APPROVE_ACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "target", user, 0);
        json_put_string_field(payload, sizeof(payload), "mode", mode, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "DENY_ACCESS") == 0 || strcmp(cmd, "deny-access") == 0) {
        if (argc < 6) { fprintf(stderr, "DENY_ACCESS requires <file> <user>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *user = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "DENY_ACCESS", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "target", user, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "STATS") == 0 || strcmp(cmd, "stats") == 0) {
        json_put_string_field(payload, sizeof(payload), "type", "STATS", 1);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
    } else if (strcmp(cmd, "checkpoint") == 0) {
        if (argc < 6) { fprintf(stderr, "checkpoint requires <file> <name>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *name = argv[5];
        // LOOKUP for CHECKPOINT
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "CHECKPOINT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "CHECKPOINT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        json_put_string_field(req, sizeof(req), "name", name, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send CHECKPOINT"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv CHECKPOINT"); close(sfd); return 1; }
        print_human("SS", r); free(r); close(sfd); return 0;
    } else if (strcmp(cmd, "list-checkpoints") == 0) {
        if (argc < 5) { fprintf(stderr, "list-checkpoints requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "LISTCHECKPOINTS", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[256]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "LISTCHECKPOINTS", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send LISTCHECKPOINTS"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv LISTCHECKPOINTS"); close(sfd); return 1; }
        print_human("SS", r); free(r); close(sfd); return 0;
    } else if (strcmp(cmd, "view-checkpoint") == 0) {
        if (argc < 6) { fprintf(stderr, "view-checkpoint requires <file> <name>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *name = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "VIEWCHECKPOINT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "VIEWCHECKPOINT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        json_put_string_field(req, sizeof(req), "name", name, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send VIEWCHECKPOINT"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv VIEWCHECKPOINT"); close(sfd); return 1; }
        print_human("SS", r); free(r); close(sfd); return 0;
    } else if (strcmp(cmd, "revertc") == 0) {
        if (argc < 6) { fprintf(stderr, "revertc requires <file> <name>\n"); close(fd); return 1; }
        const char *file = argv[4]; const char *name = argv[5];
        json_put_string_field(payload, sizeof(payload), "type", "LOOKUP", 1);
        json_put_string_field(payload, sizeof(payload), "op", "REVERT", 0);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        strncat(payload, "}", sizeof(payload) - strlen(payload) - 1);
        if (send_msg(fd, payload, (uint32_t)strlen(payload)) < 0) { perror("send"); close(fd); return 1; }
        char *resp=NULL; uint32_t rlen=0; if (recv_msg(fd, &resp, &rlen) < 0) { perror("recv"); close(fd); return 1; }
        int dport=0; char ssaddr[64]={0}; char ticket[256]={0};
        int ok = (json_get_int_field(resp, "ssDataPort", &dport) == 0 && json_get_string_field(resp, "ssAddr", ssaddr, sizeof(ssaddr)) == 0 && json_get_string_field(resp, "ticket", ticket, sizeof(ticket)) == 0);
        free(resp); close(fd); if (!ok || dport<=0) { fprintf(stderr, "[CLI] LOOKUP failed\n"); return 1; }
        int sfd = tcp_connect(ssaddr, (uint16_t)dport); if (sfd < 0) { perror("connect SS"); return 1; }
        char req[512]; req[0]='\0'; json_put_string_field(req, sizeof(req), "type", "REVERT", 1);
        json_put_string_field(req, sizeof(req), "file", file, 0);
        json_put_string_field(req, sizeof(req), "ticket", ticket, 0);
        json_put_string_field(req, sizeof(req), "name", name, 0);
        strncat(req, "}", sizeof(req) - strlen(req) - 1);
        if (send_msg(sfd, req, (uint32_t)strlen(req)) < 0) { perror("send REVERT"); close(sfd); return 1; }
        char *r=NULL; uint32_t rl=0; if (recv_msg(sfd, &r, &rl) < 0) { perror("recv REVERT"); close(sfd); return 1; }
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
