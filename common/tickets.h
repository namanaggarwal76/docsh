#ifndef TICKETS_H
#define TICKETS_H

#include <stddef.h>

// Minimal ticketing for Docs++
// Ticket format (ASCII): file|op|ssid|exp|sig
// where sig is a simple salted checksum over file,op,ssid,exp.

int ticket_build(const char *file, const char *op, int ssid, int ttl_seconds,
                 char *out, size_t out_sz);

// Validate a ticket for the given op/file and expected ssid. Returns 0 if OK.
int ticket_validate(const char *ticket, const char *required_file,
                    const char *required_op, int expected_ssid);

#endif // TICKETS_H
