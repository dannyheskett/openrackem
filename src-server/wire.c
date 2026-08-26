#include "wire.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- Parser ------------------------------------------------------------------
typedef struct {
    const char* p;
    const char* end;
} Cur;

static void skip_ws(Cur* c) {
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static bool eat(Cur* c, char ch) {
    skip_ws(c);
    if (c->p < c->end && *c->p == ch) { c->p++; return true; }
    return false;
}

// Identifier-safe characters only: room codes, tokens, and message names need
// nothing more, and rejecting everything else means no escape handling ever.
static bool ident_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

static int parse_string(Cur* c, char* out, size_t cap) {
    if (!eat(c, '"')) return -1;
    size_t n = 0;
    while (c->p < c->end && *c->p != '"') {
        if (!ident_char(*c->p) || n + 1 >= cap) return -1;
        out[n++] = *c->p++;
    }
    if (c->p >= c->end) return -1;
    c->p++; // closing quote
    out[n] = '\0';
    return (int)n;
}

static int parse_int(Cur* c, long* out) {
    skip_ws(c);
    if (c->p >= c->end || *c->p < '0' || *c->p > '9') return -1;
    long v = 0;
    int digits = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
        if (++digits > 9) return -1;   // caps every value below 10^9
        v = v * 10 + (*c->p - '0');
        c->p++;
    }
    *out = v;
    return 0;
}

int wire_parse(const char* json, size_t len, WireKV kv[WIRE_MAX_KV]) {
    Cur c = {json, json + len};
    if (!eat(&c, '{')) return -1;

    int n = 0;
    skip_ws(&c);
    if (c.p < c.end && *c.p == '}') { c.p++; goto tail; }

    for (;;) {
        if (n >= WIRE_MAX_KV) return -1;
        if (parse_string(&c, kv[n].key, sizeof kv[n].key) <= 0) return -1;
        if (!eat(&c, ':')) return -1;
        skip_ws(&c);
        if (c.p < c.end && *c.p == '"') {
            kv[n].is_str = true;
            if (parse_string(&c, kv[n].sval, sizeof kv[n].sval) < 0) return -1;
        } else {
            kv[n].is_str = false;
            if (parse_int(&c, &kv[n].ival) != 0) return -1;
        }
        n++;
        if (eat(&c, ',')) continue;
        if (eat(&c, '}')) break;
        return -1;
    }

tail:
    skip_ws(&c);
    if (c.p != c.end) return -1;   // trailing bytes: reject the whole message
    return n;
}

long wire_int(const WireKV* kv, int n, const char* key, long absent) {
    for (int i = 0; i < n; i++) {
        if (!kv[i].is_str && strcmp(kv[i].key, key) == 0) return kv[i].ival;
    }
    return absent;
}

const char* wire_str(const WireKV* kv, int n, const char* key) {
    for (int i = 0; i < n; i++) {
        if (kv[i].is_str && strcmp(kv[i].key, key) == 0) return kv[i].sval;
    }
    return NULL;
}

// --- Writer ------------------------------------------------------------------
void jw_init(JW* w, char* buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = (cap == 0);
    if (cap) buf[0] = '\0';
}

void jw_f(JW* w, const char* fmt, ...) {
    if (w->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->overflow = true;
        w->buf[w->len] = '\0';
        return;
    }
    w->len += (size_t)n;
}
