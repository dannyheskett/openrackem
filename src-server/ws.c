#include "ws.h"
#include <string.h>
#include <stdio.h>

// --- SHA-1 (RFC 3174) -------------------------------------------------------
// Needed only for the handshake accept token; a compact, allocation-free
// implementation over one message kept well under a block-boundary edge case
// hunt: the input here is always key(<=64) + GUID(36) bytes.

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t bitlen = (uint64_t)len * 8;

    // Process the message plus padding in 64-byte chunks without allocating:
    // total padded length = len + 1 (0x80) + zeros + 8 (length), rounded up.
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t chunk = 0; chunk < total; chunk += 64) {
        uint8_t block[64];
        for (size_t i = 0; i < 64; i++) {
            size_t p = chunk + i;
            if (p < len) block[i] = msg[p];
            else if (p == len) block[i] = 0x80;
            else if (p >= total - 8) block[i] = (uint8_t)(bitlen >> (8 * (total - 1 - p)));
            else block[i] = 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
                   (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(h[i]);
    }
}

static size_t b64_encode(const uint8_t* in, size_t len, char* out, size_t cap) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (len + 2) / 3 * 4;
    if (need + 1 > cap) return 0;
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
    return o;
}

// --- Handshake ---------------------------------------------------------------
// Case-insensitive header search, tolerant of arbitrary other headers. The
// key is copied out with a hard length cap; anything longer is rejected.
static int find_ws_key(const char* req, size_t len, char* key, size_t key_cap) {
    static const char NAME[] = "sec-websocket-key:";
    size_t nlen = sizeof NAME - 1;
    for (size_t i = 0; i + nlen < len; i++) {
        // Header names start after a newline (or at the very start).
        if (i != 0 && req[i - 1] != '\n') continue;
        size_t j = 0;
        while (j < nlen) {
            char c = req[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != NAME[j]) break;
            j++;
        }
        if (j != nlen) continue;
        size_t v = i + nlen;
        while (v < len && (req[v] == ' ' || req[v] == '\t')) v++;
        size_t e = v;
        while (e < len && req[e] != '\r' && req[e] != '\n') e++;
        if (e - v == 0 || e - v >= key_cap) return -1;
        memcpy(key, req + v, e - v);
        key[e - v] = '\0';
        return 0;
    }
    return -1;
}

int ws_handshake(const char* req, size_t req_len, char* out, size_t out_cap) {
    char key[64];
    if (find_ws_key(req, req_len, key, sizeof key) != 0) return -1;

    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[64 + sizeof GUID];
    int n = snprintf(cat, sizeof cat, "%s%s", key, GUID);
    if (n < 0 || (size_t)n >= sizeof cat) return -1;

    uint8_t digest[20];
    sha1((const uint8_t*)cat, (size_t)n, digest);
    char accept[32];
    if (b64_encode(digest, 20, accept, sizeof accept) == 0) return -1;

    int m = snprintf(out, out_cap,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    if (m < 0 || (size_t)m >= out_cap) return -1;
    return m;
}

// --- Frames ------------------------------------------------------------------
int ws_decode(uint8_t* buf, size_t len, int* opcode, uint8_t** payload, size_t* plen) {
    if (len < 2) return 0;

    uint8_t b0 = buf[0], b1 = buf[1];
    int fin = b0 & 0x80;
    int rsv = b0 & 0x70;
    int op  = b0 & 0x0F;
    int masked = b1 & 0x80;
    size_t l = b1 & 0x7F;

    if (rsv) return -1;                 // no extensions negotiated
    if (!masked) return -1;             // clients MUST mask (RFC 6455 §5.1)
    if (!fin || op == 0x0) return -1;   // fragmentation unsupported by design
    if (op != WS_TEXT && op != WS_CLOSE && op != WS_PING && op != WS_PONG) return -1;
    if ((op & 0x8) && l > 125) return -1;   // control frames are short

    size_t hdr = 2;
    if (l == 127) return -1;            // 64-bit length: far past our cap
    if (l == 126) {
        if (len < 4) return 0;
        l = (size_t)buf[2] << 8 | buf[3];
        hdr = 4;
        if (l < 126) return -1;         // non-minimal length encoding
    }
    if (l > WS_MAX_CLIENT_PAYLOAD) return -1;

    if (len < hdr + 4) return 0;
    const uint8_t* mask = buf + hdr;
    size_t total = hdr + 4 + l;
    if (len < total) return 0;

    uint8_t* p = buf + hdr + 4;
    for (size_t i = 0; i < l; i++) p[i] ^= mask[i & 3];

    *opcode = op;
    *payload = p;
    *plen = l;
    return (int)total;
}

size_t ws_encode_header(uint8_t out[4], int opcode, size_t plen) {
    out[0] = (uint8_t)(0x80 | (opcode & 0x0F));
    if (plen < 126) {
        out[1] = (uint8_t)plen;
        return 2;
    }
    out[1] = 126;
    out[2] = (uint8_t)(plen >> 8);
    out[3] = (uint8_t)plen;
    return 4;
}
