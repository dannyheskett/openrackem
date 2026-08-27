#ifndef OPENRACKEM_NET_WS_H
#define OPENRACKEM_NET_WS_H

// Transport-agnostic RFC 6455 client framing, shared by the raw-socket backends
// (net_posix.c: POSIX sockets + OpenSSL; net_win.c: winsock2 + SChannel). Each
// backend owns only the byte transport (connect, read, write, TLS handshake) and
// its poll loop; everything correctness-critical here — base64, the masking RNG,
// the client frame bit-layout, server-frame decoding, and the HTTP/1.1 Upgrade
// request/response — lives once, so the two backends can never drift on the wire
// protocol. Pure C, no platform headers; compiles everywhere and is only linked
// against by the two raw backends.

#include <stddef.h>
#include <stdint.h>

// xorshift64* — the source of both the WebSocket key and the per-frame masks.
uint64_t nw_rng_next(uint64_t* s);

// Base64-encode len bytes into out (which must hold 4*ceil(len/3)+1 bytes).
void nw_b64(const uint8_t* in, size_t len, char* out);

// Format the client's HTTP/1.1 Upgrade request into req (cap bytes), generating a
// fresh random Sec-WebSocket-Key from *rng. `path` defaults to "/" when empty;
// the Host header carries no port (matches the Fly edge routing; the daemon
// ignores Host). Returns the request length, or -1 if it doesn't fit.
int nw_build_handshake(char* req, size_t cap, const char* host,
                       const char* path, uint64_t* rng);

// Scan an accumulated response for the end of the header block ("\r\n\r\n").
// Returns the number of bytes to consume (the header length) once complete and
// the status line is "HTTP/1.1 101", 0 if the terminator hasn't arrived yet, or
// -1 if the response is present but not a 101 upgrade.
long nw_parse_handshake(const uint8_t* buf, size_t len);

// Append one masked client frame (opcode over data[0..len)) to out at *out_len,
// bounded by cap, advancing *out_len. Returns 0 on success, -1 if it won't fit.
int nw_encode_frame(uint8_t* out, size_t* out_len, size_t cap, int opcode,
                    const uint8_t* data, size_t len, uint64_t* rng);

// Decode one server->client frame (must be unmasked, un-fragmented, no RSV).
// in_cap bounds the payload the caller can hold. Returns bytes consumed, 0 if
// only a partial frame is buffered, or -1 on a protocol error. On success sets
// *op (opcode), *pl (payload pointer into buf), *pn (payload length).
int nw_decode_frame(uint8_t* buf, size_t len, size_t in_cap, int* op,
                    uint8_t** pl, size_t* pn);

#endif // OPENRACKEM_NET_WS_H
