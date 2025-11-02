// Feature test macro for getaddrinfo on some libc implementations
#define _POSIX_C_SOURCE 200112L
#include "net_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; // unexpected
        off += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, p + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; // EOF
        off += (size_t)n;
    }
    return 0;
}

int send_msg(int fd, const void *buf, uint32_t len) {
    uint32_t be = htonl(len);
    if (write_all(fd, &be, sizeof(be)) < 0) return -1;
    if (len == 0) return 0;
    return write_all(fd, buf, len);
}

int recv_msg(int fd, char **out_buf, uint32_t *out_len) {
    uint32_t be = 0;
    if (read_all(fd, &be, sizeof(be)) < 0) return -1;
    uint32_t len = ntohl(be);
    char *buf = NULL;
    if (len > 0) {
        buf = (char *)malloc(len + 1);
        if (!buf) return -1;
        if (read_all(fd, buf, len) < 0) { free(buf); return -1; }
        buf[len] = '\0';
    }
    *out_buf = buf;
    if (out_len) *out_len = len;
    return 0;
}

int tcp_listen(uint16_t port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    if (listen(fd, backlog) < 0) { close(fd); return -1; }
    return fd;
}

int tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int find_key(const char *json, const char *key, const char **val_start, size_t *val_len, int *is_string) {
    // Very naive JSON key finder, expects formatting like: "key": ...
    const char *k = strstr(json, key);
    if (!k) return -1;
    // ensure it is a string key: preceding char '"'
    const char *q = k;
    while (q > json && *q != '"') q--;
    if (*q != '"') return -1;
    const char *colon = strchr(k, ':');
    if (!colon) return -1;
    const char *p = colon + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        *is_string = 1;
        p++;
        const char *end = strchr(p, '"');
        if (!end) return -1;
        *val_start = p;
        *val_len = (size_t)(end - p);
        return 0;
    } else {
        *is_string = 0;
        const char *end = p;
        while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
        *val_start = p;
        *val_len = (size_t)(end - p);
        return 0;
    }
}

int json_get_string_field(const char *json, const char *key, char *out, size_t out_sz) {
    const char *vs; size_t vl; int is_str;
    if (find_key(json, key, &vs, &vl, &is_str) < 0 || !is_str) return -1;
    if (vl + 1 > out_sz) return -1;
    memcpy(out, vs, vl);
    out[vl] = '\0';
    return 0;
}

int json_get_int_field(const char *json, const char *key, int *out) {
    const char *vs; size_t vl; int is_str;
    if (find_key(json, key, &vs, &vl, &is_str) < 0) return -1;
    char tmp[64];
    size_t n = vl < sizeof(tmp) - 1 ? vl : sizeof(tmp) - 1;
    memcpy(tmp, vs, n); tmp[n] = '\0';
    *out = atoi(tmp);
    return 0;
}

void json_put_string_field(char *dst, size_t dst_sz, const char *key, const char *val, int first) {
    snprintf(dst + strlen(dst), dst_sz - strlen(dst), "%s\"%s\":\"%s\"", first ? "{" : ",", key, val);
}

void json_put_int_field(char *dst, size_t dst_sz, const char *key, int val, int first) {
    snprintf(dst + strlen(dst), dst_sz - strlen(dst), "%s\"%s\":%d", first ? "{" : ",", key, val);
}
