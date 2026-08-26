// No-op net.h backend for platforms that don't ship an online client yet
// (Windows/winsock, Android, iOS). net_available() returns false, so main.c
// hides "Play Online" there. Web uses net_web.c (native browser WebSocket);
// Linux/macOS use net_posix.c. Exactly one backend provides these symbols per
// platform, so the shared source list links everywhere.
#include "net.h"

#if defined(PLATFORM_ANDROID) || defined(PLATFORM_IOS) || defined(_WIN32)

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
