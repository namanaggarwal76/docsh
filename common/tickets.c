#include "tickets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *kSalt = "DOCSPLUS-SALT-2025";

static unsigned long checksum(const char *file, const char *op, int ssid, long exp) {
    unsigned long sum = 5381u;
    for (const char *p = file; p && *p; ++p) sum = ((sum << 5) + sum) + (unsigned char)*p;
    for (const char *p = op; p && *p; ++p) sum = ((sum << 5) + sum) + (unsigned char)*p;
    const char *s = kSalt; while (*s) { sum = ((sum << 5) + sum) + (unsigned char)*s++; }
    sum = ((sum << 5) + sum) + (unsigned long)ssid;
    sum = ((sum << 5) + sum) + (unsigned long)exp;
    return sum;
}

int ticket_build(const char *file, const char *op, int ssid, int ttl_seconds,
                 char *out, size_t out_sz) {
    if (!file || !op || !out || out_sz < 16) return -1;
    time_t now = time(NULL);
    long exp = (long)now + ttl_seconds;
    unsigned long sig = checksum(file, op, ssid, exp);
    int n = snprintf(out, out_sz, "%s|%s|%d|%ld|%lu", file, op, ssid, exp, sig);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

int ticket_validate(const char *ticket, const char *required_file,
                    const char *required_op, int expected_ssid) {
    if (!ticket || !required_file || !required_op) return -1;
    char buf[512];
    size_t n = strlen(ticket);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, ticket, n + 1);
    char *file = strtok(buf, "|");
    char *op = strtok(NULL, "|");
    char *ssid_s = strtok(NULL, "|");
    char *exp_s = strtok(NULL, "|");
    char *sig_s = strtok(NULL, "|");
    if (!file || !op || !ssid_s || !exp_s || !sig_s) return -1;
    int ssid = atoi(ssid_s);
    long exp = atol(exp_s);
    unsigned long sig = strtoul(sig_s, NULL, 10);
    // Basic checks
    if (strcmp(file, required_file) != 0) return -1;
    if (strcmp(op, required_op) != 0) return -1;
    if (ssid != expected_ssid) return -1;
    time_t now = time(NULL);
    if ((long)now > exp) return -1;
    unsigned long expect = checksum(file, op, ssid, exp);
    if (expect != sig) return -1;
    return 0;
}
