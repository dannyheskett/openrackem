// No-op net.h backend, kept as the fallback for any future platform that ships
// without an online client. No platform currently selects it — Linux/Android use
// net_posix.c (OpenSSL), Web uses net_web.c (browser WebSocket), macOS/iOS use
// net_apple.mm (Network.framework), Windows uses net_win.c (winsock2 + SChannel).
// Guarded off (OR_NET_STUB is never defined) so it stays an empty translation
// unit everywhere and never collides with a real backend's symbols.
#include "net.h"

#if defined(OR_NET_STUB)

NetConn* net_connect(const char* host, int port, const char* path, bool tls) {
    (void)host; (void)port; (void)path; (void)tls;
    return 0;
}
void      net_poll(NetConn* c) { (void)c; }
NetStatus net_status(const NetConn* c) { (void)c; return NET_ERROR; }
bool      net_send(NetConn* c, const char* text, size_t len) {
    (void)c; (void)text; (void)len; return false;
}
int       net_recv(NetConn* c, char* buf, size_t cap) {
    (void)c; (void)buf; (void)cap; return -1;
}
void      net_close(NetConn* c) { (void)c; }
bool      net_available(void) { return false; }

#endif
