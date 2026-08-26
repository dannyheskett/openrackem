# openrackem — network play on every platform

## 1. Goal

Online play (the "Play Online" menu, wired to `wss://openrackem-server.fly.dev`)
on **all six** shipped platforms. Today only Linux has it. This plan closes the
gap, preferring each OS's native networking/TLS facilities over vendored crypto
wherever they exist.

Current state:

| Platform | Online today | Why |
|---|---|---|
| Linux | ✅ `wss` + `ws` | `net_posix.c`: from-scratch RFC 6455 over POSIX sockets, TLS via system OpenSSL |
| macOS | ✅ `wss` + `ws` (NW2 done) | `net_apple.mm`: Network.framework (`NWProtocolWebSocket` + native TLS); OpenSSL retired from the mac build, fixing the arm64-only-OpenSSL universal-binary blocker |
| Windows | ❌ none | `net_posix.c` is POSIX-only; Windows needs winsock2. Falls through to `net_stub` → menu hidden |
| Web/WASM | ✅ `wss` (NW1 done) | `net_web.c`: emscripten WebSocket (browser owns framing + TLS) |
| Android | ❌ none | `net_stub` (though Bionic has POSIX sockets — see §6) |
| iOS | ✅ `wss` + `ws` (NW2 done) | `net_apple.mm`: same Network.framework backend as macOS |

Everything above the transport is already shared and tested: the protocol, the
redacted-state client (`netgame.c`), the whole game. **Each platform only needs
to implement the seven `net.h` functions.** This plan is entirely about those
backends.

## 2. The seam (unchanged)

```c
NetConn* net_connect(const char* host, int port, const char* path, bool tls);
void      net_poll(NetConn*);   // pump once per 60 fps frame; never blocks
NetStatus net_status(const NetConn*);
bool      net_send(NetConn*, const char* text, size_t len);  // one WS text frame
int       net_recv(NetConn*, char* buf, size_t cap);         // next COMPLETE msg, or -1
void      net_close(NetConn*);
bool      net_available(void);  // gates the "Play Online" menu
```

Messages are whole JSON text frames (<2 KB). Two backend shapes satisfy this:

- **Raw-transport backends** (Linux/POSIX, Windows, Android): we own the RFC 6455
  framing over a nonblocking byte transport (`t_read`/`t_write`), TLS swapped in
  behind the same shim. Synchronous, no threads — drops straight into the render
  loop's `net_poll`.
- **Native-WebSocket backends** (Web, Apple): the OS/browser owns framing *and*
  TLS, delivering complete messages via async callbacks. These need a small
  **queue bridge** — a fixed ring of complete text messages the callback fills
  and `net_recv` drains — so the game code stays synchronous. On Web the
  callbacks run on the same main thread (lock-free ring); on Apple they arrive on
  a dispatch queue (ring guarded by a mutex / `os_unfair_lock`).

Native-WebSocket backends are dramatically simpler (no hand-written framing, no
crypto, no cert handling) and are the recommended path wherever they exist.

## 3. Prerequisite — factor the shared framing (Phase NW0)

`net_posix.c` already isolates the transport behind `t_read`/`t_write`/`tls_step`,
but the RFC 6455 machinery (base64, the masking RNG, `enqueue_frame`,
`decode_frame`, the message ring `push_msg`/`net_recv`, the HTTP-101 handshake
parse, and the `net_send`/`net_status`/`net_close` queue logic) lives inside it.
The Windows backend reuses **all** of that and only re-implements the byte
transport, so it must be extracted first, or the two raw backends drift.

**NW0:** move the transport-agnostic framing into `src/net_ws.c` (+ `net_ws.h`),
driven only through a tiny transport vtable:

```c
typedef struct {
    int  (*read)(void* t, void* buf, size_t n);   // >0 / 0=closed / T_AGAIN / T_ERR
    int  (*write)(void* t, const void* buf, size_t n);
    int  (*handshake)(void* t);                    // 1 done / 0 pending / -1 err (TLS)
} NetTransport;
```

`net_posix.c` becomes just the POSIX socket + OpenSSL transport bound to that
vtable; `net_win.c` binds the winsock + SChannel transport to the same shared
framing. Android reuses `net_posix.c` unchanged. This is a pure refactor,
regression-guarded by the existing `make test` + `make net-e2e`.

## 4. Web / WASM — Emscripten WebSocket (effort: S) — do first

The browser owns framing and TLS, so `wss` is free and there is no crypto to
write. This is the smallest backend and reaches the widest audience (any
browser, desktop or mobile).

- **Backend:** `src/net_web.c` over `emscripten/websocket.h`. `net_connect`
  builds a `wss://host:port/path` URL, `emscripten_websocket_new` with
  `createOnMainThread=true`, and registers `onopen/onmessage/onclose/onerror`.
- **TLS:** native (browser/OS trust store, SNI + hostname verification). Matches
  the Fly edge; HTTPS pages must use `wss` (mixed-content), which we do.
- **Bridge:** a fixed lock-free ring of complete text messages (callbacks and
  `net_poll`/`net_recv` all run on the main thread, never concurrently).
  `net_poll` is a near no-op.
- **Gotchas (from research):** for text messages `ev->numBytes` includes the
  trailing NUL, so copy `numBytes-1`; keep the ring statically sized (the WEB
  build uses fixed `INITIAL_MEMORY`, no heap growth); callbacks must `return
  true`; the handle is a small positive int, not a pointer; **`-lwebsocket.js`
  must be added to `WEB_LDFLAGS`** or every `emscripten_websocket_*` symbol
  fails to link. No `-pthread`/`PROXY_TO_PTHREAD`.
- **`net_available()`:** `emscripten_websocket_is_supported()`.
- **Testable:** `make web` in CI; live `wss` driven headless with Playwright
  against the Fly server (the same tool used for the WASM smoke test).

## 5. Apple (macOS + iOS) — Network.framework (effort: M)

One `src/net_apple.mm` serves both OS targets and **unblocks macOS `wss`** by
retiring OpenSSL there entirely — `NWProtocolWebSocket` + `NWProtocolTLS` do
framing and TLS natively, sidestepping the arm64-only-OpenSSL universal-binary
problem.

- **Backend:** `nw_connection_t` with `nw_ws_create_options(nw_ws_version_13)`
  prepended to the protocol stack, TLS via
  `nw_parameters_create_secure_tcp(DEFAULT, DEFAULT)` for `wss`
  (`NW_PARAMETERS_DISABLE_PROTOCOL` for plain `ws`).
- **TLS:** native and automatic — server-trust evaluation against the system
  store, SNI/hostname from the endpoint host. Fly's public CA cert validates with
  zero extra code.
- **Bridge:** async, on a private serial dispatch queue, so the ring + status are
  guarded by a mutex/`os_unfair_lock`. Map `nw_connection_state`
  (waiting/preparing→CONNECTING, ready→OPEN, cancelled→CLOSED, failed→ERROR).
  **Re-arm `nw_connection_receive_message` after every delivered message** (the
  #1 pitfall — forgetting stalls all inbound traffic); enqueue only text opcodes;
  `set_auto_reply_ping(true)` handles pings.
- **Build (as shipped):** compile the one `.mm` with `clang++ -fno-objc-arc` —
  the `nw_`/`dispatch_` handles are hand-retained in a plain C struct, so manual
  retain/release is simpler than bridging casts under ARC — and link
  `-framework Network`. macOS selects it with `-DOR_NET_APPLE`, which compiles
  `net_posix` out of `MAC_OBJ`; the seam is declared `extern "C"` in `net.h` so
  the Obj-C++ definitions link against the C callers. iOS drops `net_stub` and
  adds the framework. Min deployment ≥ macOS 10.15 / iOS 13 (already satisfied).
- **Testable (as wired):** macOS CI builds `net_apple.mm` *and* runs it against
  the live Fly server via `make mac-net-test` (Network.framework is on the
  runner); iOS builds + links in CI, on-device online play is manual
  (Simulator/device). Not buildable on Linux, so `net_posix` stays the
  Linux/loopback test path; `make net-live` runs the same live smoke test there.

## 6. Android — reuse net_posix + bundled OpenSSL (effort: M)

Android reuses the **most** existing code: Bionic has every POSIX call
`net_posix.c` uses, so un-gating it gives plain `ws` immediately; `wss` needs a
TLS lib and a trust anchor.

- **Sockets:** widen the `net_posix.c` guard to include `PLATFORM_ANDROID`; drop
  `net_stub` from `ANDROID_SRC`. Plain `ws` then works with no new code, and the
  synchronous `net_poll` model needs no thread/queue.
- **TLS:** bundle a prebuilt **OpenSSL 3.x static lib for arm64-v8a** (you build
  it — OpenSSL upstream doesn't ship Android), staged under
  `third_party/openssl-android/arm64-v8a/{include,lib}` exactly like the
  per-ABI raylib. Compile `net_posix` with `-DOR_TLS`; its OpenSSL shim
  (`SSL_connect`, SNI, `SSL_set1_host`) compiles unchanged. Preferred over
  platform BoringSSL (not a stable NDK API) and over an OkHttp/JNI bridge
  (Gradle-less dexing of Kotlin/okio, a network thread + queue, ~1.5 MB runtime).
- **Trust anchor (the one genuinely Android-specific bit):**
  `SSL_CTX_set_default_verify_paths` finds nothing on Android, and
  `/system/etc/security/cacerts` is incomplete on Android 14+ (roots moved to a
  Conscrypt APEX). Bundle a Mozilla `cacert.pem` (Fly uses Let's Encrypt / ISRG
  Root X1) **compiled into the binary** (`xxd -i` → `BIO_new_mem_buf` →
  `X509_STORE_add_cert`) — the PEM lives inside the APK zip with no real path, so
  loading it from memory is the robust choice.
- **Also required:** add `<uses-permission android:name="android.permission.INTERNET"/>`
  to `AndroidManifest.xml` (currently absent — without it `connect()` gets EPERM).
- **Lifecycle:** the game auto-pauses on focus loss, so `net_poll` stops and no
  keepalive runs; a NAT/edge idle-timeout can silently drop the socket.
  `netgame.c`'s token-based auto-rejoin already recovers on error; additionally
  force `net_close`+reconnect on `APP_CMD_RESUME` after a long pause so a
  stale-but-not-errored socket recovers promptly.
- **Testable:** cross-compiles in CI like the current APK (the prebuilt OpenSSL
  `.a` must match the NDK API level and stay 16 KB-page-aligned —
  `check_elf_align.sh` already enforces this); live `wss` on emulator/device via
  `scripts/devicefarm_run.py`.

## 7. Windows — winsock2 + SChannel (effort: L) — do last

Most from-scratch of the four, and the only one that genuinely needs a Windows
box to functionally verify. Uses the NW0 shared framing; only the transport is
new. **Verified this session that both mingw toolchains link `-lsecur32 -lws2_32`
with `SCHANNEL_CRED`/`SCH_CREDENTIALS`** — so no OpenSSL-for-mingw bundle is
needed; SChannel is the native TLS.

- **Sockets:** `net_win.c` (guard `_WIN32`): `WSAStartup(MAKEWORD(2,2))` once per
  process (refcounted, not per-connection), `getaddrinfo`, nonblocking via
  `ioctlsocket(FIONBIO)`, async connect handling `WSAEWOULDBLOCK`, confirm via
  `getsockopt(SO_ERROR)`. `recv`/`send` map `WSAEWOULDBLOCK` → `T_AGAIN`, exactly
  the existing sentinels.
- **TLS (SChannel/SSPI):** `AcquireCredentialsHandle` (outbound, system store) →
  a `tls_step()` running the `InitializeSecurityContext` streaming loop, which
  maps cleanly onto `net_poll`: `SEC_E_INCOMPLETE_MESSAGE` = want-more (return,
  retry next frame, like `SSL_ERROR_WANT_READ`), `SEC_I_CONTINUE_NEEDED` = emit
  the token and loop, `SEC_E_OK` = done. `QueryContextAttributes(STREAM_SIZES)`
  sizes the record buffers; `EncryptMessage`/`DecryptMessage` carry app bytes;
  stash `SECBUFFER_EXTRA` leftovers across polls before the unchanged
  `decode_frame`.
- **Cert/hostname parity with `SSL_set1_host`:** pass the host as ISC's
  `pszTargetName` for auto-validation, or pull `SECPKG_ATTR_REMOTE_CERT_CONTEXT`
  and call `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL)` with the
  server name; **fail to `NET_ERROR` on mismatch** (never silently proceed).
- **Compatibility:** use legacy `SCHANNEL_CRED` for broadest reach; TLS 1.2 (which
  the Fly edge accepts) avoids TLS 1.3 close_notify token handling on Win11.
- **De-risk:** optionally land plain-`ws` winsock first (`net_connect(tls)` →
  `NET_ERROR`), then add SChannel.
- **Testable:** CI gates compile+link (proven); live `wss` only on a real Windows
  box — wine's SChannel is unreliable.

## 8. The async queue-bridge (shared design for Web + Apple)

Both native-WebSocket backends share one pattern, worth stating once:

```
struct NetConn {
    <native handle>;              // EMSCRIPTEN_WEBSOCKET_T / nw_connection_t
    NetStatus status;            // set by state/lifecycle callbacks
    char   ring[32][2048];       // complete text messages
    int    head, tail;           // net_recv drains, callback fills
    <lock>;                      // none on Web (main thread), mutex on Apple
};
```

`net_send` → native send when OPEN; `net_recv` → pop the ring; `net_poll` → no-op
(status is callback-driven). Overflow drops per the existing `push_msg` policy.
Idempotent `state` snapshots mean a dropped intermediate message just skips an
animation, never desyncs.

## 9. Milestones

Each ends green (builds clean, `make test` + `make net-e2e` pass, CI passes) and
is independently shippable — a platform's client turns on the moment its backend
lands, nothing else changes.

- **NW0 — Shared framing.** Extract `net_ws.c`/`net_ws.h`; `net_posix.c` becomes a
  transport binding. Pure refactor; existing tests are the guard.
- **NW1 — Web. ✅ done.** `net_web.c`; `-lwebsocket.js`; Playwright live-`wss`
  smoke test. Biggest reach, smallest effort.
- **NW2 — Apple. ✅ done.** `net_apple.mm` (Network.framework); unblocks macOS
  `wss` *and* delivers iOS; OpenSSL dropped from the mac build. `net.h` made
  `extern "C"` for the Obj-C++ seam; macOS CI runs `make mac-net-test` live.
- **NW3 — Android.** Prebuilt OpenSSL arm64-v8a + bundled `cacert.pem` + INTERNET
  permission + resume-reconnect; reuse `net_posix`.
- **NW4 — Windows.** `net_win.c` (winsock2 + SChannel) on the NW0 framing;
  optional plain-`ws` first pass.

NW0 (shared-framing refactor) is deferred until NW4 (Windows) is scheduled: it
exists only to stop the Windows and Linux raw backends from drifting, and no
raw backend but Linux has landed yet, so there is nothing to share prematurely.

Recommended order is by value-per-effort: **Web → Apple → Android → Windows**.
Web and Apple are the cheapest and cover the most users (any browser; all Apple
hardware, plus fixing the mac blocker); Windows is the heaviest and needs a
Windows host to sign off.

## 10. Testing strategy

- **Shared logic** (protocol, `netgame.c`, framing) stays covered by `make test`
  (`test_server`, `test_netgame`) and `make net-e2e` (real daemon + two POSIX
  clients over loopback) — unchanged, and the regression guard for NW0.
- **Per backend:** CI gates compile+link on every platform. Live `wss` against
  the Fly server is: automated for **Web** (Playwright) and **macOS** (CI runner);
  Simulator/device for **iOS**/**Android** (`devicefarm_run.py`); a real Windows
  box for **Windows**.
- **Cert verification** must reach parity with the POSIX `SSL_set1_host` on every
  backend that does its own verification (Windows SChannel, Android OpenSSL) —
  reject bad certs, as the desktop client already does (self-signed/expired are
  rejected in tests today).

## 11. Cross-cutting risks

- **Cert-verification parity.** Windows (SChannel) and Android (OpenSSL trust
  store) verify by hand — the easy bug is proceeding on failure. Each must fail
  to `NET_ERROR`, matching Linux.
- **Android trust maintenance.** The bundled `cacert.pem` needs periodic refresh,
  and the prebuilt OpenSSL must be rebuilt per NDK bump and kept 16 KB-aligned.
- **Mobile lifecycle.** Backgrounded apps stop polling; rely on the existing
  token rejoin plus a resume-triggered reconnect so silently-dropped sockets
  recover.
- **Backend divergence.** NW0 must land first so Windows and Linux share one
  framing implementation rather than two that drift.
- **On-device-only verification.** iOS, Android, and Windows `wss` can't be
  fully validated in headless CI; budget device/VM time before their store
  releases.

## 12. What does not change

The server, the protocol, `netgame.c`, the redaction, the renderers, and the
whole game. This plan adds four files (`net_ws.c`, `net_web.c`, `net_apple.mm`,
`net_win.c`), one refactor of `net_posix.c`, one vendored prebuilt (Android
OpenSSL) + a bundled `cacert.pem`, a manifest permission, and Makefile/CI
wiring. Nothing above the transport seam is touched.
