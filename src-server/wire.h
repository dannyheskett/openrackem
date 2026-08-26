#ifndef OPENRACKEM_WIRE_H
#define OPENRACKEM_WIRE_H

#include <stdbool.h>
#include <stddef.h>

// The daemon's wire format: JSON, but only the sliver of it clients ever
// send — one flat object of string keys mapping to non-negative integers or
// short identifier strings. Anything else (nesting, arrays, escapes, floats,
// negatives, literals) is a parse error and the connection is closed. A
// deliberately tiny grammar is a deliberately tiny attack surface.
//
// Outbound messages are built with the cap-checked writer; an overflowing
// message is a bug and is dropped whole rather than sent truncated.

#define WIRE_MAX_KV    8
#define WIRE_KEY_MAX   15
#define WIRE_STR_MAX   39

typedef struct {
    char key[WIRE_KEY_MAX + 1];
    bool is_str;
    char sval[WIRE_STR_MAX + 1];   // valid when is_str
    long ival;                     // valid when !is_str
} WireKV;

// Parse one flat object. Returns the number of pairs (0..WIRE_MAX_KV) or -1
// on any deviation from the grammar.
int wire_parse(const char* json, size_t len, WireKV kv[WIRE_MAX_KV]);

// Lookups over a parsed message.
long        wire_int(const WireKV* kv, int n, const char* key, long absent);
const char* wire_str(const WireKV* kv, int n, const char* key);   // NULL if absent

// --- Writer ------------------------------------------------------------------
typedef struct {
    char*  buf;
    size_t cap, len;
    bool   overflow;
} JW;

void jw_init(JW* w, char* buf, size_t cap);
// printf-append; sets overflow (and stops growing) if it would not fit.
void jw_f(JW* w, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#endif // OPENRACKEM_WIRE_H
