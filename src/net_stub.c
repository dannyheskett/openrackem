// No-op net.h backend for platforms that don't ship an online client yet
// (Windows/winsock, web, Android, iOS). net_available() returns false, so
// main.c hides "Play Online" there. Guarded as the exact inverse of
// net_posix.c, so precisely one backend provides these symbols per platform
// and the shared source list links everywhere.
#include "net.h"

#if defined(PLATFORM_WEB) || defined(PLATFORM_ANDROID) || \
    defined(PLATFORM_IOS) || defined(_WIN32)

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
