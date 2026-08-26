#ifndef OPENRACKEM_WS_H
#define OPENRACKEM_WS_H

#include <stddef.h>
#include <stdint.h>

// Minimal RFC 6455 WebSocket server codec — exactly the subset a tiny
// turn-based game daemon needs, written here so the repo vendors nothing and
// stays MIT-clean. Pure functions over caller buffers (no sockets, no
// allocation), so the whole surface is unit-testable, including against the
// RFC's own handshake vector.
//
// Deliberate restrictions (each is a protocol-error close, not a crash):
//   - client frames must be masked (the RFC requires it)
//   - no fragmentation: FIN must be set, opcode CONT rejected (browsers do
//     not fragment kilobyte-scale messages)
//   - text frames only for data; payloads capped at WS_MAX_CLIENT_PAYLOAD
//   - RSV bits must be zero (no extensions are negotiated)

#define WS_MAX_CLIENT_PAYLOAD 1024

enum {
    WS_TEXT  = 0x1,
    WS_BIN   = 0x2,
    WS_CLOSE = 0x8,
    WS_PING  = 0x9,
    WS_PONG  = 0xA,
};

// Build the 101 Switching Protocols response for a complete HTTP upgrade
// request (caller has already seen the terminating blank line). Returns the
// response length, or -1 if the request is not an acceptable WebSocket
// upgrade (missing/oversized Sec-WebSocket-Key).
int ws_handshake(const char* req, size_t req_len, char* out, size_t out_cap);

// Decode one client frame from buf[0..len). On success returns the bytes
// consumed and sets *opcode, *payload (into buf, unmasked in place), *plen.
// Returns 0 when the buffer holds an incomplete frame, -1 on protocol error
// (caller closes the connection).
int ws_decode(uint8_t* buf, size_t len, int* opcode, uint8_t** payload, size_t* plen);

// Write a server frame header (unmasked, FIN set) for a payload of plen
// bytes into out[0..4). Returns the header length (2 or 4). plen must be
// under 64 KiB, which everything this daemon sends is.
size_t ws_encode_header(uint8_t out[4], int opcode, size_t plen);

#endif // OPENRACKEM_WS_H
