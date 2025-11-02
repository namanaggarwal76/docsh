#ifndef NET_PROTO_H
#define NET_PROTO_H

#include <stdint.h>
#include <stddef.h>

// Unified error codes
// Keep in sync across components

typedef enum {
    OK = 0,
    ERR_NOAUTH = 1,
    ERR_NOTFOUND = 2,
    ERR_LOCKED = 3,
    ERR_BADREQ = 4,
    ERR_CONFLICT = 5,
    ERR_UNAVAILABLE = 6,
    ERR_INTERNAL = 7
} error_code_t;

// Length-prefixed message framing (uint32 big-endian length)
// send_msg: write 4-byte length then the buffer
// recv_msg: allocates *out_buf (caller must free)
int send_msg(int fd, const void *buf, uint32_t len);
int recv_msg(int fd, char **out_buf, uint32_t *out_len);

// Helper: create, bind, listen on a TCP port (SO_REUSEADDR enabled)
// Returns listening fd on success, -1 on error
int tcp_listen(uint16_t port, int backlog);

// Helper: connect to host:port
// Returns connected fd on success, -1 on error
int tcp_connect(const char *host, uint16_t port);

// Small JSON helpers (very minimal for bootstrap)
// Note: These are NOT general JSON parsers; they are sufficient for simple key extraction.
// Returns 0 on success, -1 on failure
int json_get_string_field(const char *json, const char *key, char *out, size_t out_sz);
int json_get_int_field(const char *json, const char *key, int *out);

// Compose tiny JSONs
// dst must have enough space
void json_put_string_field(char *dst, size_t dst_sz, const char *key, const char *val, int first);
void json_put_int_field(char *dst, size_t dst_sz, const char *key, int val, int first);

#endif // NET_PROTO_H
