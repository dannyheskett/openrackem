// Shared RFC 6455 client framing — see net_ws.h. Extracted verbatim from
// net_posix.c's original private helpers so the POSIX and Windows backends share
// one wire implementation; the existing make test / net-e2e / net-live cover it.
#include "net_ws.h"

#include <stdio.h>
#include <string.h>

uint64_t nw_rng_next(uint64_t* s) {
    uint64_t x = *s ? *s : 0x2545F4914F6CDD1DULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

void nw_b64(const uint8_t* in, size_t len, char* out) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

int nw_build_handshake(char* req, size_t cap, const char* host,
                       const char* path, uint64_t* rng) {
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(nw_rng_next(rng) >> 24);
    char key64[25];
    nw_b64(key, 16, key64);
    int n = snprintf(req, cap,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     path && path[0] ? path : "/", host, key64);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

long nw_parse_handshake(const uint8_t* buf, size_t len) {
    const uint8_t* end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) { end = buf + i + 4; break; }
    }
    if (!end) return 0;                                   // header not complete yet
    if (len < 12 || memcmp(buf, "HTTP/1.1 101", 12) != 0) return -1;   // not an upgrade
    return (long)(end - buf);
}

int nw_encode_frame(uint8_t* out, size_t* out_len, size_t cap, int opcode,
                    const uint8_t* data, size_t len, uint64_t* rng) {
    // Client frames are masked. Header is 2 or 4 bytes + 4 mask bytes.
    size_t hdr = (len < 126) ? 2 : 4;
    size_t need = hdr + 4 + len;
    if (*out_len + need > cap) return -1;
    uint8_t* p = out + *out_len;
    p[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (len < 126) {
        p[1] = (uint8_t)(0x80 | len);
    } else {
        p[1] = (uint8_t)(0x80 | 126);
        p[2] = (uint8_t)(len >> 8);
        p[3] = (uint8_t)len;
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(nw_rng_next(rng) >> 17);
    memcpy(p + hdr, mask, 4);
    for (size_t i = 0; i < len; i++) p[hdr + 4 + i] = data[i] ^ mask[i & 3];
    *out_len += need;
    return 0;
}

int nw_decode_frame(uint8_t* buf, size_t len, size_t in_cap, int* op,
                    uint8_t** pl, size_t* pn) {
    if (len < 2) return 0;
    int fin = buf[0] & 0x80, rsv = buf[0] & 0x70, o = buf[0] & 0x0F;
    int masked = buf[1] & 0x80;
    size_t l = buf[1] & 0x7F;
    if (rsv || masked || !fin || o == 0) return -1;   // server must not mask/fragment
    size_t hdr = 2;
    if (l == 127) return -1;
    if (l == 126) {
        if (len < 4) return 0;
        l = (size_t)buf[2] << 8 | buf[3];
        hdr = 4;
    }
    if (hdr + l > in_cap) return -1;
    if (len < hdr + l) return 0;
    *op = o; *pl = buf + hdr; *pn = l;
    return (int)(hdr + l);
}
