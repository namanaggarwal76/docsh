#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/net_proto.h"

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
    if (argc < 4) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <nm_host> <nm_port> VIEW [-a] [-l]\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> READ <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> CREATE <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> WRITE <file> <sentenceIndex>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> UNDO <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> INFO <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> DELETE <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> STREAM <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> LIST\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> ADDACCESS -R|-W <file> <user>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> REMACCESS <file> <user>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> EXEC <file>\n", argv[0]);
        // debug/bonus helpers kept:
        fprintf(stderr, "  %s <nm_host> <nm_port> dir-set <file> <ssId>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> rename <old> <new>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> read <file>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> read-noticket <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> create <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> delete <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> write <file> <sentenceIndex> <wordIndex> <content>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> undo <file>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> history <file>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> revert <file> <version>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> rename <old> <new>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> migrate <file> <targetSsId>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> addaccess <file> <user> <R|W|RW>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> remaccess <file> <user>\n", argv[0]);
    fprintf(stderr, "  %s <nm_host> <nm_port> put-direct <ssDataPort> <file> <body>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> mkdir <path>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> lsf <path>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> mv <src> <dst>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> checkpoint <file> <name>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> list-checkpoints <file>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> view-checkpoint <file> <name>\n", argv[0]);
        fprintf(stderr, "  %s <nm_host> <nm_port> revertc <file> <name>\n", argv[0]);
        return 1;
    }
    const char *nm_host = argv[1];
    uint16_t nm_port = (uint16_t)atoi(argv[2]);
    const char *cmd = argv[3];
    // Ask username and send CLIENT_HELLO
    char username[128];
    fprintf(stdout, "Enter username: "); fflush(stdout);
    if (!fgets(username, sizeof(username), stdin)) { fprintf(stderr, "Failed to read username\n"); return 1; }
    // strip newline
    size_t ulen = strlen(username); if (ulen && username[ulen-1]=='\n') username[--ulen]='\0';
    int hfd = tcp_connect(nm_host, nm_port);
    if (hfd >= 0) {
        char hello[256]; hello[0]='\0'; json_put_string_field(hello, sizeof(hello), "type", "CLIENT_HELLO", 1); json_put_string_field(hello, sizeof(hello), "user", username, 0); strncat(hello, "}", sizeof(hello)-strlen(hello)-1);
        (void)send_msg(hfd, hello, (uint32_t)strlen(hello)); char *hr=NULL; uint32_t hrl=0; (void)recv_msg(hfd, &hr, &hrl); free(hr); close(hfd);
    }

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
        if (argc < 5) { fprintf(stderr, "create requires <file>\n"); close(fd); return 1; }
        const char *file = argv[4];
        json_put_string_field(payload, sizeof(payload), "type", "CREATE", 1);
        json_put_string_field(payload, sizeof(payload), "file", file, 0);
        json_put_string_field(payload, sizeof(payload), "user", username, 0);
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
