/* Real sockets for the engine (LAN multiplayer), Android/bionic ABI -> libnx.
 *
 * Off by default: without the marker file `lan` in switch/sixguns/ every call
 * keeps failing like before (the port runs offline). With it, the BSD socket
 * service is initialised and the engine's calls are translated:
 *   - struct sockaddr_in: Linux {u16 family, u16 port, u32 addr, 8 zero} vs
 *     BSD {u8 len, u8 family, u16 port, u32 addr, 8 zero};
 *   - setsockopt/getsockopt levels and names (SOL_SOCKET 1 vs 0xffff,
 *     SO_REUSEADDR 2 vs 4, SO_BROADCAST 6 vs 0x20, ...);
 *   - send/recv flags (MSG_DONTWAIT 0x40 vs 0x80, MSG_NOSIGNAL dropped);
 *   - fcntl O_NONBLOCK (0x800 vs newlib's);
 *   - fd_set (bionic 1024 bits) vs newlib's FD_SETSIZE;
 *   - errno values that differ (EINPROGRESS, EADDRINUSE, ENOTCONN, ...).
 * Name lookups only accept IP literals, so nothing reaches Gameloft's dead
 * servers. What the engine uses (tools/decomp_mp/7CSocket.c, 4CUdp.c, 4CTcp.c):
 * socket(AF_INET, STREAM/DGRAM), bind, connect, listen, accept, send/recv,
 * sendto/recvfrom, getsockname, select, setsockopt(SO_REUSEADDR, SO_BROADCAST,
 * TCP_NODELAY), fcntl(F_GETFL/F_SETFL O_NONBLOCK), gethostbyname, inet_ntoa.
 * The LAN listen port is UDP 7513. */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

long gethostid(void); /* libnx: the console's IPv4 address */
#include "config.h"
#include "util.h"
#include "so_util.h"
so_module *port_engine_module(void);

int net_on;
static uint8_t is_sock[1024], is_nb[1024], is_udp[1024];
static int test_recv_pause;
static u64 t_start(void) { return armGetSystemTick(); }
static long t_ms(u64 t0) { return (long)(armTicksToNs(armGetSystemTick() - t0) / 1000000); }

/* Beyond the local network (groundwork for player-hosted servers; all optional, in switch/sixguns/):
 *   mp_servers.txt    IPv4 addresses, one per line: every LAN search broadcast (UDP 7513) is also sent to
 *                     each, so a game can find a host that is not on the local network.
 *   mp_advertise.txt  the IPv4 a host tells joiners to connect to (default: its local address). A host
 *                     behind a router puts its public address here and forwards UDP 7513 + the room port.
 * (A fixed room port for forwarding was tried: binding the room socket to a fixed port crashed the host
 *  when it received its own search packet; the room port stays random. Next step for hosting.)
 * Without them nothing changes: plain LAN play. */
static uint32_t mp_servers[32]; /* mp_servers.txt + peers added at run time (server browser) */
static int n_mp_servers;
static uint32_t mp_advertise;
static void net_load_server_config(void) {
  char line[128];
  FILE *f = fopen(PORT_ROOT "/mp_servers.txt", "r");
  while (f && fgets(line, sizeof(line), f) && n_mp_servers < 32) {
    struct in_addr a;
    line[strcspn(line, "\r\n #")] = 0;
    if (line[0] && inet_aton(line, &a)) { mp_servers[n_mp_servers++] = a.s_addr; debugPrintf("[net] server: %s\n", line); }
  }
  if (f) fclose(f);
  f = fopen(PORT_ROOT "/mp_advertise.txt", "r");
  if (f && fgets(line, sizeof(line), f)) {
    struct in_addr a;
    line[strcspn(line, "\r\n #")] = 0;
    if (inet_aton(line, &a)) { mp_advertise = a.s_addr; debugPrintf("[net] hosting: advertising %s\n", line); }
  }
  if (f) fclose(f);
}

void net_init(void) {
  FILE *m = fopen(PORT_ROOT "/lan", "r");
  if (!m) return;
  fclose(m);
  SocketInitConfig cfg = *socketGetDefaultInitConfig();
  Result rc = socketInitialize(&cfg);
  if (R_FAILED(rc)) {
    debugPrintf("[net] socketInitialize failed 0x%x: staying offline\n", rc);
    return;
  }
  net_on = 1;
  struct in_addr a = { .s_addr = gethostid() };
  debugPrintf("[net] LAN networking on (local address %s)\n", inet_ntoa(a));
  net_load_server_config();
}

int net_is_socket(int fd) { return fd >= 0 && fd < 1024 && is_sock[fd]; }
void net_forget(int fd) { if (fd >= 0 && fd < 1024) is_sock[fd] = is_nb[fd] = is_udp[fd] = 0; }

/* newlib errno -> Linux errno for the values that differ */
static void lin_errno(void) {
  switch (errno) {
    case EINPROGRESS: errno = 115; break;
    case EALREADY: errno = 114; break;
    case EADDRINUSE: errno = 98; break;
    case EADDRNOTAVAIL: errno = 99; break;
    case ENOTCONN: errno = 107; break;
    case EISCONN: errno = 106; break;
    case ENOTSOCK: errno = 88; break;
    case EMSGSIZE: errno = 90; break;
    case ECONNABORTED: errno = 103; break;
    case ETIMEDOUT: errno = 110; break;
    case EHOSTUNREACH: errno = 113; break;
    case ENETUNREACH: errno = 101; break;
    case ENOBUFS: errno = 105; break;
    default: break; /* EAGAIN/EWOULDBLOCK 11, ECONNREFUSED 111, ECONNRESET 104: same */
  }
}
#define FAIL_IF(r) do { if ((r) < 0) lin_errno(); } while (0)

/* Linux sockaddr (as the engine passes it) -> BSD sockaddr_in */
static int to_bsd(const void *lin, unsigned len, struct sockaddr_in *out) {
  memset(out, 0, sizeof(*out));
  if (!lin || len < 8) return -1;
  const uint8_t *p = lin;
  uint16_t fam = p[0] | (p[1] << 8);
  if (fam != 2) return -1; /* AF_INET only */
  out->sin_len = sizeof(*out);
  out->sin_family = AF_INET;
  memcpy(&out->sin_port, p + 2, 2);
  memcpy(&out->sin_addr, p + 4, 4);
  return 0;
}
static void to_lin(const struct sockaddr_in *in, void *lin, unsigned *len) {
  if (!lin || !len) return;
  uint8_t tmp[16] = {0};
  tmp[0] = 2; /* AF_INET, little-endian u16 */
  memcpy(tmp + 2, &in->sin_port, 2);
  memcpy(tmp + 4, &in->sin_addr, 4);
  unsigned n = *len < 16 ? *len : 16;
  memcpy(lin, tmp, n);
  *len = 16;
}

static int map_level(int l) { return l == 1 ? SOL_SOCKET : l; }
static int map_opt(int level, int o) {
  if (level != 1) return o; /* IPPROTO_TCP TCP_NODELAY 1, IPPROTO_IP: same numbers */
  switch (o) {
    case 2: return SO_REUSEADDR;
    case 4: return SO_ERROR;
    case 6: return SO_BROADCAST;
    case 7: return SO_SNDBUF;
    case 8: return SO_RCVBUF;
    case 9: return SO_KEEPALIVE;
    case 13: return SO_LINGER;
    case 15: return SO_REUSEPORT;
    case 20: return SO_RCVTIMEO;
    case 21: return SO_SNDTIMEO;
    default: return -1;
  }
}
static int map_msg(int f) {
  int r = 0;
  if (f & 0x1) r |= MSG_OOB;
  if (f & 0x2) r |= MSG_PEEK;
  if (f & 0x40) r |= MSG_DONTWAIT;
  if (f & 0x100) r |= MSG_WAITALL;
  return r; /* MSG_NOSIGNAL (0x4000) has no meaning here */
}

int n_socket(int domain, int type, int proto) {
  if (!net_on) { errno = 97; return -1; } /* EAFNOSUPPORT: offline */
  /* IPv4 only: something probes AF_INET6 (Linux 10) at startup, and Ryubing
   * aborts the whole emulator on an address family it doesn't know. */
  if (domain != 2) {
    static int logged;
    if (logged++ < 4) debugPrintf("[net] socket(domain %d): only IPv4 is supported\n", domain);
    errno = 97; /* EAFNOSUPPORT */
    return -1;
  }
  int nonblock = type & 0x800;
  type &= 0xf; /* drop SOCK_NONBLOCK / SOCK_CLOEXEC */
  int fd = socket(AF_INET, type, proto);
  if (fd < 0) { lin_errno(); return -1; }
  if (fd < 1024) { is_sock[fd] = 1; is_udp[fd] = type == SOCK_DGRAM; }
  if (nonblock && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0 && fd < 1024) is_nb[fd] = 1;
  static int logged;
  if (logged++ < 8) debugPrintf("[net] socket(%d, %d, %d) = %d\n", domain, type, proto, fd);
  return fd;
}

int n_bind(int fd, const void *addr, unsigned len) {
  struct sockaddr_in a;
  if (!net_on || to_bsd(addr, len, &a) < 0) { errno = 22; return -1; }
  /* Fixed UDP ports (the LAN discovery port 7513) are shareable, so two copies of the game on one machine
   * (two emulators; a host and a player later) both get the broadcasts: SO_REUSEADDR before the first bind
   * of that port in this process. Not on the game's own second bind of 7513: it copes with that one
   * failing, and sharing it doubled every packet and crashed the game (emulator 2026-11-14). Two
   * Switches never need it (each has its own address). */
  static uint16_t mine[16];
  static int nmine;
  int udp = fd >= 0 && fd < 1024 && is_udp[fd] && a.sin_port;
  int own = 0;
  /* mine[] holds the ports as the engine asked for them (before the testing offset below), so the game's
   * own second bind of 7513 is recognised even when lan_port_offset.txt moved the first one. */
  uint16_t asked = a.sin_port;
  for (int i = 0; i < nmine; i++) own |= mine[i] == asked;
  if (udp && !own) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); }
  /* Broadcast receive on fixed ports: a Switch got no broadcasts at all on 7513 (not even its own; the
   * emulator does), so it never saw LAN rooms (2026-11-14). SO_BROADCAST is harmless for receiving and some
   * stacks need it; try it. */
  if (udp) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)); }
  /* Testing two copies on one machine (two emulators on a Mac): marker lan_port_offset.txt = N moves this
   * copy's own LAN discovery listener 7513 to 7513+N, so the other copy alone owns 7513 and receives this
   * copy's searches (sent to the peers in mp_servers.txt, e.g. 127.0.0.1). Never needed between consoles. */
  {
    static int off = -1;
    if (off < 0) {
      off = 0;
      FILE *f = fopen(PORT_ROOT "/lan_port_offset.txt", "r");
      if (f) { if (fscanf(f, "%d", &off) != 1 || off < 0 || off > 1000) off = 0; fclose(f); }
      if (off) debugPrintf("[net] testing: LAN discovery listener moved to %d\n", 7513 + off);
    }
    if (off && ntohs(a.sin_port) == 7513) a.sin_port = htons(7513 + off);
  }
  int r = bind(fd, (struct sockaddr *)&a, sizeof(a));
  if (r == 0 && udp && !own && nmine < 16) mine[nmine++] = asked;
  debugPrintf("[net] bind(%d, %s:%u) = %d%s\n", fd, inet_ntoa(a.sin_addr), ntohs(a.sin_port), r,
              r < 0 ? strerror(errno) : "");
  FAIL_IF(r);
  return r;
}

int n_connect(int fd, const void *addr, unsigned len) {
  struct sockaddr_in a;
  if (!net_on || to_bsd(addr, len, &a) < 0) { errno = 22; return -1; }
  int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
  debugPrintf("[net] connect(%d, %s:%u) = %d\n", fd, inet_ntoa(a.sin_addr), ntohs(a.sin_port), r);
  FAIL_IF(r);
  return r;
}

int n_listen(int fd, int backlog) {
  if (!net_on) return -1;
  int r = listen(fd, backlog);
  FAIL_IF(r);
  return r;
}

int n_accept(int fd, void *addr, unsigned *len) {
  if (!net_on) return -1;
  struct sockaddr_in a;
  socklen_t al = sizeof(a);
  if (fd >= 0 && fd < 1024 && !is_nb[fd]) debugPrintf("[net] accept(%d) on a BLOCKING socket ...\n", fd);
  int r = accept(fd, (struct sockaddr *)&a, &al);
  if (r < 0) { lin_errno(); return -1; }
  if (r < 1024) is_sock[r] = 1;
  to_lin(&a, addr, len);
  debugPrintf("[net] accept(%d) = %d from %s:%u\n", fd, r, inet_ntoa(a.sin_addr), ntohs(a.sin_port));
  return r;
}

long n_send(int fd, const void *buf, size_t n, int flags) {
  if (!net_on) return -1;
  long r = send(fd, buf, n, map_msg(flags));
  FAIL_IF(r);
  return r;
}

long n_recv(int fd, void *buf, size_t n, int flags) {
  if (!net_on) return -1;
  int blocking = fd >= 0 && fd < 1024 && !is_nb[fd] && !(flags & 0x40);
  if (blocking) debugPrintf("[net] recv(%d) on a BLOCKING socket ...\n", fd);
  long r = recv(fd, buf, n, map_msg(flags));
  if (blocking) debugPrintf("[net] recv(%d) -> %ld\n", fd, r);
  FAIL_IF(r);
  return r;
}

long n_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, unsigned len) {
  if (!net_on) return -1;
  if (!addr) { long r = send(fd, buf, n, map_msg(flags)); FAIL_IF(r); return r; }
  struct sockaddr_in a;
  if (to_bsd(addr, len, &a) < 0) { errno = 22; return -1; }
  long r = sendto(fd, buf, n, map_msg(flags), (struct sockaddr *)&a, sizeof(a));
  if (n_mp_servers && a.sin_addr.s_addr == INADDR_BROADCAST && ntohs(a.sin_port) == 7513)
    for (int i = 0; i < n_mp_servers; i++) {
      struct sockaddr_in s2 = a;
      s2.sin_addr.s_addr = mp_servers[i];
      sendto(fd, buf, n, map_msg(flags), (struct sockaddr *)&s2, sizeof(s2));
    }
  static unsigned logged;
  if (logged < 20 || r < 0) {
    logged++;
    debugPrintf("[net] sendto(%d, %zu bytes, %s:%u) = %ld%s\n", fd, n, inet_ntoa(a.sin_addr), ntohs(a.sin_port), r,
                r < 0 ? strerror(errno) : "");
  }
  FAIL_IF(r);
  return r;
}

long n_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, unsigned *len) {
  if (!net_on) return -1;
  struct sockaddr_in a;
  socklen_t al = sizeof(a);
  memset(&a, 0, sizeof(a));
  int blocking = fd >= 0 && fd < 1024 && !is_nb[fd] && !(flags & 0x40);
  if (blocking) debugPrintf("[net] recvfrom(%d) on a BLOCKING socket ...\n", fd);
  u64 t0 = t_start();
  /* UDP: Linux (and the Switch) truncate a datagram bigger than the buffer; a Windows host under
   * Ryubing fails the receive with EMSGSIZE instead, and the 181-byte room message on a 17-byte read
   * hung the lobby after PLAY. Receive into a full-size buffer and truncate like Linux. */
  /* Per-call storage: two network threads must not overwrite each other's datagrams.
   * Engine pthread stacks are at least 1 MB; this adds 16 KB without per-packet allocation. */
  char big[16384]; /* < 64 KB: larger buffers overflowed Ryubing's IPC descriptor size field */
  int udp = fd >= 0 && fd < 1024 && is_udp[fd] && n < sizeof(big);
  long r = recvfrom(fd, udp ? big : buf, udp ? sizeof(big) : n, map_msg(flags), (struct sockaddr *)&a, &al);
  if(test_recv_pause && r>0) svcSleepThread(1000000);
  if (udp && r > 0) {
    if ((size_t)r > n) {
      static unsigned tl;
      if (__atomic_fetch_add(&tl,1,__ATOMIC_RELAXED) < 10) debugPrintf("[net] recvfrom(%d): %ld-byte datagram truncated to %zu\n", fd, r, n);
      r = (long)n;
    }
    memcpy(buf, big, (size_t)r);
  }
  if (blocking || t_ms(t0) > 200) debugPrintf("[net] recvfrom(%d) -> %ld after %ld ms\n", fd, r, t_ms(t0));
  if (r < 0) { lin_errno(); return r; }
  to_lin(&a, addr, len);
  char address[INET_ADDRSTRLEN]="?";inet_ntop(AF_INET,&a.sin_addr,address,sizeof(address));
  {
    /* the first packet from every new sender (address:port), past the 20-line cap below: who this
     * console hears from (diagnosing LAN discovery on hardware) */
    static uint64_t senders[64];
    static int nsenders;
    static Mutex senders_mtx;
    mutexLock(&senders_mtx);
    uint64_t k = ((uint64_t)a.sin_addr.s_addr << 16) | a.sin_port;
    int seen = 0;
    for (int i = 0; i < nsenders && !seen; i++) seen = senders[i] == k;
    if (!seen && nsenders < 64) {
      senders[nsenders++] = k;
      debugPrintf("[net] first packet from %s:%u on fd %d (%ld bytes)\n", address, ntohs(a.sin_port), fd, r);
    }
    mutexUnlock(&senders_mtx);
  }
  static unsigned logged;
  if (__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED) < 20)
    debugPrintf("[net] recvfrom(%d) = %ld bytes from %s:%u\n", fd, r, address, ntohs(a.sin_port));
  return r;
}

int n_shutdown(int fd, int how) {
  if (!net_on) return -1;
  int r = shutdown(fd, how);
  FAIL_IF(r);
  return r;
}

int n_setsockopt(int fd, int level, int name, const void *val, unsigned len) {
  if (!net_on) return -1;
  int o = map_opt(level, name);
  if (o < 0) { debugPrintf("[net] setsockopt(%d, %d, %d): unknown option, ignored\n", fd, level, name); return 0; }
  int r = setsockopt(fd, map_level(level), o, val, len);
  if (r < 0) debugPrintf("[net] setsockopt(%d, %d, %d) failed: %s\n", fd, level, name, strerror(errno));
  FAIL_IF(r);
  return r;
}

int n_getsockopt(int fd, int level, int name, void *val, unsigned *len) {
  if (!net_on) return -1;
  int o = map_opt(level, name);
  if (o < 0) { errno = 92; return -1; } /* ENOPROTOOPT */
  socklen_t l = len ? *len : 0;
  int r = getsockopt(fd, map_level(level), o, val, &l);
  if (len) *len = l;
  FAIL_IF(r);
  return r;
}

int n_getsockname(int fd, void *addr, unsigned *len) {
  if (!net_on) return -1;
  struct sockaddr_in a;
  socklen_t al = sizeof(a);
  int r = getsockname(fd, (struct sockaddr *)&a, &al);
  if (r < 0) { lin_errno(); return r; }
  to_lin(&a, addr, len);
  return r;
}

int n_getpeername(int fd, void *addr, unsigned *len) {
  if (!net_on) return -1;
  struct sockaddr_in a;
  socklen_t al = sizeof(a);
  int r = getpeername(fd, (struct sockaddr *)&a, &al);
  if (r < 0) { lin_errno(); return r; }
  to_lin(&a, addr, len);
  return r;
}

/* bionic fd_set: 1024 bits as 16 u64 */
static void set_in(const uint64_t *lin, fd_set *nx, int nfds) {
  FD_ZERO(nx);
  if (!lin) return;
  for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++)
    if (lin[fd / 64] & (1ull << (fd % 64))) FD_SET(fd, nx);
}
static void set_out(uint64_t *lin, const fd_set *nx, int nfds) {
  if (!lin) return;
  memset(lin, 0, 128);
  for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++)
    if (FD_ISSET(fd, nx)) lin[fd / 64] |= 1ull << (fd % 64);
}

int n_select(int nfds, uint64_t *rd, uint64_t *wr, uint64_t *ex, struct timeval *tv) {
  if (!net_on) return 0;
  fd_set r, w, e;
  int lim = nfds > FD_SETSIZE ? FD_SETSIZE : nfds;
  set_in(rd, &r, lim); set_in(wr, &w, lim); set_in(ex, &e, lim);
  /* nfds = highest fd in the sets + 1: the engine passes 1024 (bionic's
   * FD_SETSIZE) and libnx/Ryubing answer EINVAL to anything that large, which
   * made CUdp::Receive spin forever (DataAvailable said yes, ReceiveFrom's own
   * select failed). */
  int n = 0;
  for (int fd = 0; fd < lim; fd++)
    if ((rd && FD_ISSET(fd, &r)) || (wr && FD_ISSET(fd, &w)) || (ex && FD_ISSET(fd, &e))) n = fd + 1;
  /* UDP-only sets: answer from a non-blocking 1-byte MSG_PEEK per socket
   * instead of select, which takes ~50 ms per call in Ryubing even with a zero
   * timeout (the match ran at 8 fps). Datagram sockets are always writable. */
  {
    int udp_only = n > 0 && !ex;
    for (int fd = 0; fd < n && udp_only; fd++)
      if (((rd && FD_ISSET(fd, &r)) || (wr && FD_ISSET(fd, &w))) && !(fd < 1024 && is_udp[fd])) udp_only = 0;
    if (udp_only) {
      int ready = 0;
      u64 deadline = tv ? armGetSystemTick() + armNsToTicks((u64)tv->tv_sec * 1000000000ull + (u64)tv->tv_usec * 1000ull) : 0;
      for (;;) {
        fd_set ro;
        FD_ZERO(&ro);
        for (int fd = 0; fd < n; fd++) {
          if (!rd || !FD_ISSET(fd, &r)) continue;
          char b;
          /* data, or a pending error (e.g. ICMP port unreachable): readable,
           * so the engine's recvfrom consumes it; EAGAIN: nothing yet */
          static char pk[16384]; /* full size: a short peek is EMSGSIZE on a Windows host */
          (void)b;
          if (recv(fd, pk, sizeof(pk), MSG_PEEK | MSG_DONTWAIT) >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            FD_SET(fd, &ro);
            ready++;
          }
        }
        if (wr) for (int fd = 0; fd < n; fd++) if (FD_ISSET(fd, &w)) ready++;
        if (ready || (tv && armGetSystemTick() >= deadline)) {
          if (rd) { r = ro; set_out(rd, &r, lim); }
          if (wr) set_out(wr, &w, lim);
          return ready;
        }
        svcSleepThread(1000000); /* 1 ms */
      }
    }
  }
  if (!tv) debugPrintf("[net] select(%d) with NO timeout ...\n", n);
  u64 t0 = t_start();
  int res = select(n, rd ? &r : NULL, wr ? &w : NULL, ex ? &e : NULL, tv);
  long ms = t_ms(t0);
  if (ms > 200 || !tv) debugPrintf("[net] select(%d) -> %d after %ld ms\n", n, res, ms);
  if (res < 0) { lin_errno(); return res; }
  set_out(rd, &r, lim); set_out(wr, &w, lim); set_out(ex, &e, lim);
  return res;
}

int n_poll(struct pollfd *fds, unsigned long n, int timeout) {
  if (!net_on) return 0;
  int r = poll(fds, n, timeout); /* same struct layout and POLLIN/POLLOUT values */
  FAIL_IF(r);
  return r;
}

/* fcntl: F_GETFL (3) / F_SETFL (4) with O_NONBLOCK on sockets; everything else
 * keeps the old behaviour (0). */
int n_fcntl(int fd, int cmd, long arg) {
  if (!net_is_socket(fd)) return 0;
  if (cmd == 3) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) { lin_errno(); return f; }
    return (f & O_NONBLOCK) ? 0x802 : 0x2; /* O_RDWR | O_NONBLOCK in Linux values */
  }
  if (cmd == 4) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) f = 0;
    f = (arg & 0x800) ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK);
    int r = fcntl(fd, F_SETFL, f);
    if (r == 0 && fd < 1024) is_nb[fd] = (arg & 0x800) != 0;
    debugPrintf("[net] fcntl(%d, F_SETFL, %s) = %d%s\n", fd, (arg & 0x800) ? "O_NONBLOCK" : "blocking", r, r < 0 ? strerror(errno) : "");
    FAIL_IF(r);
    return r;
  }
  return 0;
}

/* Name lookups: IP literals only (no DNS: the game's servers are gone, and
 * LAN play needs none). hostent has the same layout in bionic and newlib. */
void *n_gethostbyname(const char *name) {
  static struct hostent h;
  static struct in_addr a;
  static char *list[2];
  if (!name || !inet_aton(name, &a)) {
    debugPrintf("[net] gethostbyname(%s): only IP addresses are resolved\n", name ? name : "");
    return NULL;
  }
  h.h_name = (char *)name;
  h.h_aliases = NULL;
  h.h_addrtype = AF_INET;
  h.h_length = 4;
  list[0] = (char *)&a;
  list[1] = NULL;
  h.h_addr_list = list;
  return &h;
}

unsigned n_inet_addr(const char *s) { return s ? inet_addr(s) : 0xffffffff; }
char *n_inet_ntoa(unsigned a) { struct in_addr x = { .s_addr = a }; return inet_ntoa(x); }
const char *n_inet_ntop(int af, const void *src, char *dst, unsigned n) {
  return inet_ntop(af == 2 ? AF_INET : af, src, dst, n);
}

/* The game asks acp_utils::PackageUtils::GetConnectionType() before multiplayer
 * ("No Internet connection available" otherwise): 1, 6, 7, 8 count as connected.
 * Android reported it through JNIBridge.SetConnectionType, which nothing calls
 * here. With LAN on, report Wi-Fi (1) once the engine runs (after its static
 * initialisers, which would reset it). Called every frame from main.c. */
static void hide_dead_remote_players(void);
void net_frame(void) {
  static int done;
  if (net_on) hide_dead_remote_players();
  if (!net_on || done) return;
  done = 1;
  void (*set_type)(void *, void *, int) = (void *)so_symbol(
      port_engine_module(), "Java_com_gameloft_android_ANMP_GloftSXHM_PackageUtils_JNIBridge_SetConnectionType");
  if (set_type) set_type(NULL, NULL, 1);
  debugPrintf("[net] connection type reported to the game: Wi-Fi%s\n", set_type ? "" : " (symbol missing!)");
}

/* LAN host address fix (2026-11-12, docs/re/subsystems/multiplayer.md).
 * CSocket::GetLocalAddress (0xd21310) is `mov x0, xzr; ret` in this build, so
 * CUdp::GetLocalNetworkId (0xd3bfd4) builds the host's advertised id from
 * 0.0.0.0, and the LAN discovery reply (tServerResponseMsg) tells other
 * consoles to connect to 0.0.0.0: a second device can never join. The
 * replacement keeps the engine's own port lookup and CNetworkId constructor
 * and uses the console's real IPv4 (gethostid). Same calls as the original:
 *   GetLocalPort(this + 0x18); CNetworkId::CNetworkId(result, in_addr, port). */
static unsigned (*csocket_get_local_port)(void *);
static void (*cnetworkid_ctor)(void *, uint32_t, unsigned);
void net_udp_local_network_id(void *self, void *result) {
  unsigned port = csocket_get_local_port((char *)self + 0x18) & 0xffff;
  cnetworkid_ctor(result, mp_advertise ? mp_advertise : (uint32_t)gethostid(), port);
}
void net_udp_local_network_id_hook(void);

/* Friendly fire between players (Player::IsFriendlyTo, only used by Player::OnHit to drop the hit).
 * The host's RoundSession::Update gives teams 1/2 only to players marked ReadyForNextMatch and team 0 to
 * the rest. A LAN host that started alone and was joined mid-match never ran that round flow, so both
 * players stayed on team 0: every bullet between them was dropped as friendly fire (owner, Switch host +
 * emulator, 2026-11-15). Team 0 now means "no team yet" (hostile); real teams 1/2 are still friendly. */
static int (*player_get_team)(void *);
static int is_friendly_hook(void *self, void *other) {
  if (!other || other == self || !player_get_team) return 0;
  int a = player_get_team(self), b = player_get_team(other);
  return a != 0 && a == b;
}

/* GS_GamePlay::StartDying(entity) starts the LOCAL death sequence whoever died: PlayerCtrl::OnStartDying
 * on the local controller, sub-state 7, the "DyingCamera" cinematic. In single player only the player dies.
 * In a LAN match the host also runs Player::Kill for a remote player's copy, and the host then sat in a
 * death camera while alive ("dead, no animation, standing still": owner, Switch host, 2026-11-15). For a
 * remote player's death the remote console runs its own sequence; here it is skipped. The host's own
 * player is skipped as well (see start_dying_hook). */
extern const uint32_t start_dying_orig_head[4];
extern void start_dying_orig(void *gameplay, void *entity);
uintptr_t start_dying_orig_cont;
static void *(*sd_get_mgr)(void);
static int (*sd_count)(void *);
static void *(*sd_info_at)(void *, unsigned);
static void *(*sd_info_player)(void *);
static int (*sd_info_local)(void *);
static int (*sd_mp_enabled)(void *);
static void *(*sd_mp_inst)(void);
static void (*sd_set_state_label)(void *, int);
static void start_dying_hook(void *gameplay, void *entity) {
  void *mp = entity && sd_mp_inst ? sd_mp_inst() : NULL;
  if (mp && (sd_mp_enabled(mp) & 1)) {
    void *mgr = sd_get_mgr();
    int n = mgr ? sd_count(mgr) : 0;
    for (int i = 0; i < n && i < 16; i++) {
      void *info = sd_info_at(mgr, (unsigned)i);
      if (info && sd_info_player(info) == entity) {
        /* The host's own player too: a client never calls StartDying for its player (the host kills it),
         * it only notices the death and Player::Update -> ShowDeathScreen pushes Menu_MP_DeathScreen
         * (sub-state 0 -> 10, respawn -> 0). On the host, StartDying took sub-state 0 -> 7: the single-player
         * dying camera and its "Just die" popup, and the MP respawn did not work afterwards (owner,
         * hardware 2026-11-15; mp_debug logs). Both now take the client's path. */
        int local = sd_info_local(info) & 1;
        /* What StartDying does for a dying actor that is not the local player, minus the local camera:
         * its death animation (state label 0x58). Skipping it left a killed remote player standing on the
         * host's screen (owner, hardware 2026-11-15). */
        if (!local && sd_set_state_label) sd_set_state_label(entity, 0x58);
        debugPrintf("[net] %s player died: single-player death sequence skipped (MP death screen)\n", local ? "local" : "remote");
        return;
      }
    }
  }
  start_dying_orig(gameplay, entity);
}

/* Multiplayer::RevivePlayer ("Spawn now" on Menu_MP_DeathScreen) only works on a client: it sends CMsgRevive
 * to the host, whose Multiplayer update then calls Player::NetRebirth on that dead player. On the host it
 * returned at once (!IsClient), so the host could never respawn (owner, hardware 2026-11-15). The host now
 * does for itself what it does for a client's message. */
extern const uint32_t revive_orig_head[4];
extern void revive_orig(void *mp, int paid);
uintptr_t revive_orig_cont;
static int (*rv_is_client)(void *);
static void *(*rv_local_info)(void *);
static void (*rv_rebirth)(void *);
static int (*rv_is_dead)(void *);
static void (*rv_leave_cover)(void *, int), (*rv_col_remove)(void *, void *), (*rv_col_add)(void *, void *), (*rv_reload)(void *);
static void *(*rv_scene_mgr)(void), *(*rv_cam_mgr)(void *), *(*rv_scene_cam)(void *);
static void (*rv_set_camera)(void *, void *), (*rv_set_health)(void *, float);
static float (*rv_max_health)(void *);
static void revive_hook(void *mp, int paid) {
  if (mp && !(rv_is_client(mp) & 1) && sd_get_mgr) {
    void *mgr = sd_get_mgr();
    void *info = mgr ? rv_local_info(mgr) : NULL;
    void *pl = info ? sd_info_player(info) : NULL;
    if (pl && (rv_is_dead(pl) & 1)) {
      /* NetRebirth's own "get up" part (LeaveCover, alive state label 1, collision re-add, reload, camera,
       * full health) runs only when NOT on the host: it is written for a client's own player, while the
       * host only moves a remote player to a spawn point (NetRespawn). For the host's own player that left
       * it lying on the floor with full health until the pause menu refreshed it (owner, 2026-11-15). */
      uint8_t *p8 = pl;
      if (rv_leave_cover) rv_leave_cover(pl, 1);
      if (sd_set_state_label) sd_set_state_label(pl, 1);
      if (rv_col_remove && rv_col_add && *(void **)(p8 + 0x550) && *(void **)(p8 + 0x558)) {
        rv_col_remove(*(void **)(p8 + 0x550), *(void **)(p8 + 0x558));
        rv_col_add(*(void **)(p8 + 0x550), *(void **)(p8 + 0x558));
      }
      if (rv_reload) rv_reload(p8 + 0x1b8);
      if (rv_scene_mgr && rv_set_camera && rv_cam_mgr && rv_scene_cam) {
        void *cm = rv_cam_mgr(pl), *sm = rv_scene_mgr();
        void *cam = cm ? rv_scene_cam(cm) : NULL;
        if (sm && cam) rv_set_camera(sm, cam);
      }
      if (rv_set_health && rv_max_health) rv_set_health(p8 + 0x360, rv_max_health(p8 + 0x360));
      debugPrintf("[net] host respawn (Spawn now): local get-up + Player::NetRebirth on the host's own player\n");
      rv_rebirth(pl);
    }
    return;
  }
  revive_orig(mp, paid);
}

/* TEMPORARY LAN armor cap (owner's request, 2026-11-15). Player::GetArmor sums the worn outfit's armor per
 * damage type, capped at 100, and DoDamage removes that percentage (minimum 1 point): with top outfits on
 * both players every bullet did 1 damage. In a match the armor is capped at lan_armor_cap (default 50;
 * file switch/sixguns/lan_armor_cap.txt holds a number 0..100, 100 = the game's own rule). Single player
 * is unchanged. To remove: delete this block and armor_orig in net_hooks.s. */
extern const uint32_t armor_orig_head[4];
extern int armor_orig(void *player, int type);
uintptr_t armor_orig_cont;
static int lan_armor_cap = 50;
static int armor_hook(void *player, int type) {
  int a = armor_orig(player, type);
  void *mp = sd_mp_inst ? sd_mp_inst() : NULL;
  if (mp && (sd_mp_enabled(mp) & 1) && a > lan_armor_cap) a = lan_armor_cap;
  return a;
}

/* PlayerCtrl::UpdateFreeCamera, reached only while JoystickHW is connected (the port feeds it; Android never
 * did), uses its player's camera manager (MultiplayerPlayerInfo::GetCameraManager) without a NULL check. A
 * remote player's controller created on the host while the host was still loading had none: the host crashed
 * in CameraMgr::GetCurrentController when the emulator joined too early (owner, Switch log 2026-11-15). */
extern const uint32_t free_cam_orig_head[4];
extern void free_cam_orig(void *ctrl);
uintptr_t free_cam_orig_cont;
static void *(*fc_cam_mgr)(void *);
static void free_cam_hook(void *ctrl) {
  void *info = ctrl ? *(void **)((uint8_t *)ctrl + 0x3c8) : NULL;
  if (!info || !fc_cam_mgr || !fc_cam_mgr(info)) return;
  free_cam_orig(ctrl);
}

/* A remote player killed on the host stayed standing: its Actor is dead on the host, but its animation comes
 * from the network. The joining console simply hides a dead remote player; the host now does the same: hidden
 * while dead, shown again on respawn (owner's suggestion, 2026-11-15). Per frame from net_frame. */
static void (*hv_set_visible)(void *, int);
static int (*hv_is_dead)(void *);
static void hide_dead_remote_players(void) {
  /* Tracked by player, not by list index: when another player left while this one was hidden, the list
   * shifted and the respawned player was never shown again (audit 2026-11-17). Entries whose player is no
   * longer in the list are dropped. */
  static void *hidden[16];
  if (!sd_mp_inst || !sd_mp_enabled || !hv_set_visible || !hv_is_dead || !sd_get_mgr) return;
  void *mp = sd_mp_inst();
  if (!mp || !(sd_mp_enabled(mp) & 1)) { memset(hidden, 0, sizeof(hidden)); return; }
  void *mgr = sd_get_mgr();
  int n = mgr ? sd_count(mgr) : 0;
  void *present[16];
  int np = 0;
  for (int i = 0; i < n && i < 16; i++) {
    void *info = sd_info_at(mgr, (unsigned)i);
    void *pl = info ? sd_info_player(info) : NULL;
    if (!pl || (sd_info_local(info) & 1)) continue;
    present[np++] = pl;
    int dead = hv_is_dead(pl) & 1, slot = -1, free_slot = -1;
    for (int k = 0; k < 16; k++) {
      if (hidden[k] == pl) slot = k;
      else if (!hidden[k] && free_slot < 0) free_slot = k;
    }
    if (dead && slot < 0 && free_slot >= 0) { hv_set_visible(pl, 0); hidden[free_slot] = pl; }
    else if (!dead && slot >= 0) { hv_set_visible(pl, 1); hidden[slot] = NULL; }
  }
  for (int k = 0; k < 16; k++) {
    if (!hidden[k]) continue;
    int still = 0;
    for (int i = 0; i < np && !still; i++) still = present[i] == hidden[k];
    if (!still) hidden[k] = NULL; /* left the match: never touch its (possibly freed) actor again */
  }
}

void net_patch(so_module *mod) {
  if (!net_on) return;
  {
    uintptr_t a = so_symbol(mod, "_ZN10PlayerCtrl16UpdateFreeCameraEv");
    fc_cam_mgr = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo16GetCameraManagerEv");
    hv_set_visible = (void *)so_symbol(mod, "_ZN5Actor15SetVisibleActorEb");
    hv_is_dead = (void *)so_symbol(mod, "_ZNK5Actor6IsDeadEv");
    int ok = a && mod->image && fc_cam_mgr && memcmp(mod->image + (a - mod->base), free_cam_orig_head, 16) == 0;
    if (ok) {
      free_cam_orig_cont = a + 16;
      ok = so_hook_function(mod, "_ZN10PlayerCtrl16UpdateFreeCameraEv", (void *)free_cam_hook) == 0;
    }
    debugPrintf("[net] early-join camera crash fix %s\n", ok ? "applied" : "NOT applied");
  }
  {
    FILE *f = fopen(PORT_ROOT "/lan_armor_cap.txt", "r");
    int v;
    if (f) { if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100) lan_armor_cap = v; fclose(f); }
    uintptr_t a = so_symbol(mod, "_ZN6Player8GetArmorEi");
    int ok = a && mod->image && memcmp(mod->image + (a - mod->base), armor_orig_head, 16) == 0;
    if (ok) {
      armor_orig_cont = a + 16;
      ok = so_hook_function(mod, "_ZN6Player8GetArmorEi", (void *)armor_hook) == 0;
    }
    debugPrintf("[net] LAN armor cap %s: %d%% in matches (temporary)\n", ok ? "on" : "NOT applied", lan_armor_cap);
  }
  {
    uintptr_t a = so_symbol(mod, "_ZN11Multiplayer12RevivePlayerEb");
    rv_is_client = (void *)so_symbol(mod, "_ZN11Multiplayer8IsClientEv");
    rv_local_info = (void *)so_symbol(mod, "_ZN24MultiplayerPlayerManager18GetLocalPlayerInfoEv");
    rv_rebirth = (void *)so_symbol(mod, "_ZN6Player10NetRebirthEv");
    rv_is_dead = (void *)so_symbol(mod, "_ZNK5Actor6IsDeadEv");
    sd_set_state_label = (void *)so_symbol(mod, "_ZN5Actor17SetStateFromLabelEi");
    rv_leave_cover = (void *)so_symbol(mod, "_ZN6Player10LeaveCoverEb");
    rv_col_remove = (void *)so_symbol(mod, "_ZN12CollisionMgr15RemoveCollisionEP13CollisionNode");
    rv_col_add = (void *)so_symbol(mod, "_ZN12CollisionMgr12AddCollisionEP13CollisionNode");
    rv_reload = (void *)so_symbol(mod, "_ZN9AttackMgr8DoReloadEv");
    rv_scene_mgr = (void *)so_symbol(mod, "_ZN3pig5scene8SceneMgr11GetSceneMgrEv");
    rv_set_camera = (void *)so_symbol(mod, "_ZN3pig5scene8SceneMgr9SetCameraEPNS0_6CameraE");
    rv_cam_mgr = (void *)so_symbol(mod, "_ZN6Player12GetCameraMgrEv");
    rv_scene_cam = (void *)so_symbol(mod, "_ZN9CameraMgr14GetSceneCameraEv");
    rv_set_health = (void *)so_symbol(mod, "_ZN9DamageMgr9SetHealthEf");
    rv_max_health = (void *)so_symbol(mod, "_ZN9DamageMgr12GetMaxHealthEv");
    int ok = a && mod->image && rv_is_client && rv_local_info && rv_rebirth && rv_is_dead &&
             memcmp(mod->image + (a - mod->base), revive_orig_head, 16) == 0;
    if (ok) {
      revive_orig_cont = a + 16;
      ok = so_hook_function(mod, "_ZN11Multiplayer12RevivePlayerEb", (void *)revive_hook) == 0;
    }
    debugPrintf("[net] host respawn fix %s\n", ok ? "applied" : "NOT applied");
  }
  {
    uintptr_t a = so_symbol(mod, "_ZN11GS_GamePlay10StartDyingEP10GameEntity");
    sd_get_mgr = (void *)so_symbol(mod, "_Z27GetMultiplayerPlayerManagerv");
    sd_count = (void *)so_symbol(mod, "_ZNK21CNetPlayerInfoManager18GetPlayerInfoCountEv");
    sd_info_at = (void *)so_symbol(mod, "_ZN24MultiplayerPlayerManager22GetPlayerInfoFromIndexEj");
    sd_info_player = (void *)so_symbol(mod, "_ZN21MultiplayerPlayerInfo9GetPlayerEv");
    sd_info_local = (void *)so_symbol(mod, "_ZNK14CNetPlayerInfo7IsLocalEv");
    sd_mp_inst = (void *)so_symbol(mod, "_ZN11Multiplayer11GetInstanceEv");
    sd_mp_enabled = (void *)so_symbol(mod, "_ZN11Multiplayer9IsEnabledEv");
    sd_set_state_label = (void *)so_symbol(mod, "_ZN5Actor17SetStateFromLabelEi");
    int ok = a && mod->image && sd_get_mgr && sd_count && sd_info_at && sd_info_player && sd_info_local && sd_mp_inst &&
             sd_mp_enabled && memcmp(mod->image + (a - mod->base), start_dying_orig_head, 16) == 0;
    if (ok) {
      start_dying_orig_cont = a + 16;
      ok = so_hook_function(mod, "_ZN11GS_GamePlay10StartDyingEP10GameEntity", (void *)start_dying_hook) == 0;
    }
    debugPrintf("[net] remote-death fix %s\n", ok ? "applied" : "NOT applied");
  }
  player_get_team = (void *)so_symbol(mod, "_ZNK6Player7GetTeamEv");
  if (player_get_team && so_hook_function(mod, "_ZNK6Player12IsFriendlyToEPS_", (void *)is_friendly_hook) == 0)
    debugPrintf("[net] friendly fire: players without a team (0) can hit each other\n");
  else
    debugPrintf("[net] friendly-fire fix NOT applied\n");
  csocket_get_local_port = (void *)so_symbol(mod, "_ZNK7CSocket12GetLocalPortEv");
  cnetworkid_ctor = (void *)so_symbol(mod, "_ZN10CNetworkIdC2E7in_addrt");
  if (!csocket_get_local_port || !cnetworkid_ctor ||
      so_hook_function(mod, "_ZNK4CUdp17GetLocalNetworkIdEv", (void *)net_udp_local_network_id_hook) != 0) {
    debugPrintf("[net] LAN address fix NOT applied (symbols missing)\n");
    return;
  }
  struct in_addr a = { .s_addr = gethostid() };
  debugPrintf("[net] LAN host address fix: advertising %s instead of 0.0.0.0\n", inet_ntoa(a));
}

/* ---------------------------------------------------------------- server browser
 * The port's own discovery, next to the game's (which broadcasts on UDP 7513 and needs every console to
 * receive broadcasts: a Switch did not, owner 2026-11-14). Replies are always unicast.
 *   Host: a game that is hosting a match (ui_menus.c menus_mp_host_info) answers "SGNXQ1" queries on UDP
 *         7520 with "SGNXR1" + {mode, map, name} to the asker, and adds the asker as a peer (its own 7513
 *         broadcasts then also go straight to it).
 *   Browser: while the lobby's server list is open, a query goes out every second from its own socket (an
 *         ephemeral port, so two copies on one machine don't take each other's replies) to the broadcast
 *         address, the /24 subnet broadcast and every peer (mp_servers.txt + servers added in the game).
 *   Joining: picking a server adds it as a peer, then the game's own search (PLAY) reaches it directly.
 * Online later: a server list from the internet only has to fill the same peer list / entries. */
#define SB_PORT 7520
static int sb_srv = -1, sb_cli = -1, sb_browsing;
typedef struct { uint32_t ip; int mode, map; char name[24]; u64 seen; } sb_entry;
static sb_entry sb_list[16];
static int sb_n;

int net_add_peer(uint32_t ip, int save) {
  if (!ip || ip == INADDR_BROADCAST || ip == (uint32_t)gethostid()) return 0;
  for (int i = 0; i < n_mp_servers; i++) if (mp_servers[i] == ip) return 0;
  if (n_mp_servers >= (int)(sizeof(mp_servers) / sizeof(*mp_servers))) return 0;
  mp_servers[n_mp_servers++] = ip;
  struct in_addr a = { .s_addr = ip };
  debugPrintf("[net] peer added: %s%s\n", inet_ntoa(a), save ? " (saved)" : "");
  if (save) {
    FILE *f = fopen(PORT_ROOT "/mp_servers.txt", "a");
    if (f) { fprintf(f, "%s\n", inet_ntoa(a)); fclose(f); }
  }
  return 1;
}

static int sb_socket(unsigned port) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
  int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) { close(s); return -1; }
  return s;
}

void net_browser_set(int on) {
  if (on && !sb_browsing) { sb_n = 0; debugPrintf("[net] server browser on\n"); }
  sb_browsing = on;
}
int net_browser_count(void) { return sb_n; }
int net_browser_get(int i, uint32_t *ip, int *mode, int *map, const char **name) {
  if (i < 0 || i >= sb_n) return 0;
  *ip = sb_list[i].ip; *mode = sb_list[i].mode; *map = sb_list[i].map; *name = sb_list[i].name;
  return 1;
}

/* every frame (main.c) */
void net_browser_frame(void) {
  if (!net_on) return;
  static u64 frame;
  frame++;
  if ((sb_srv < 0 || sb_cli < 0) && frame % 120 == 1) {
    if (sb_srv < 0) sb_srv = sb_socket(SB_PORT);
    if (sb_cli < 0) sb_cli = sb_socket(0);
    debugPrintf("[net] server browser sockets: %d %d\n", sb_srv, sb_cli);
  }
  if (sb_srv < 0) return;
  extern int menus_mp_host_info(int *mode, int *map, char *name, int n);
  uint8_t buf[256];
  struct sockaddr_in from;
  socklen_t fl;
  /* host: answer queries */
  for (int k = 0; k < 8; k++) {
    fl = sizeof(from);
    long r = recvfrom(sb_srv, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (r < 0) break;
    if (r != 6) continue;
    if (memcmp(buf, "SGNXQ1", 6)) continue;
    int mode, map;
    char name[24] = {0};
    if (!menus_mp_host_info(&mode, &map, name, sizeof(name))) continue;
    uint8_t rep[6 + 2 + 24] = { 'S', 'G', 'N', 'X', 'R', '1' };
    rep[6] = (uint8_t)mode;
    rep[7] = (uint8_t)map;
    memcpy(rep + 8, name, 24);
    sendto(sb_srv, rep, sizeof(rep), 0, (struct sockaddr *)&from, sizeof(from));
    if (from.sin_addr.s_addr != (uint32_t)gethostid()) net_add_peer(from.sin_addr.s_addr, 0);
    static unsigned logged;
    if (logged++ < 10) debugPrintf("[net] browser query from %s: answered\n", inet_ntoa(from.sin_addr));
  }
  /* browser: send queries, collect answers */
  if (!sb_browsing || sb_cli < 0) return;
  if (frame % 60 == 2) {
    static const uint8_t q[6] = { 'S', 'G', 'N', 'X', 'Q', '1' };
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(SB_PORT) };
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(sb_cli, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
    to.sin_addr.s_addr = (uint32_t)gethostid() | htonl(0xff); /* the /24 subnet broadcast */
    sendto(sb_cli, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
    to.sin_addr.s_addr = (uint32_t)gethostid(); /* another copy on this machine */
    sendto(sb_cli, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
    for (int i = 0; i < n_mp_servers; i++) {
      to.sin_addr.s_addr = mp_servers[i];
      sendto(sb_cli, q, sizeof(q), 0, (struct sockaddr *)&to, sizeof(to));
    }
  }
  for (int k = 0; k < 16; k++) {
    fl = sizeof(from);
    long r = recvfrom(sb_cli, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (r < 0) break;
    if (r != 6 + 2 + 24) continue;
    if (memcmp(buf, "SGNXR1", 6)) continue;
    int i;
    for (i = 0; i < sb_n && sb_list[i].ip != from.sin_addr.s_addr; i++) {}
    if (i == sb_n) {
      if (sb_n >= 16) continue;
      sb_n++;
      debugPrintf("[net] server found: %s\n", inet_ntoa(from.sin_addr));
    }
    sb_list[i].ip = from.sin_addr.s_addr;
    sb_list[i].mode = buf[6];
    sb_list[i].map = buf[7];
    memcpy(sb_list[i].name, buf + 8, 23);
    sb_list[i].name[23] = 0;
    sb_list[i].seen = frame;
  }
  /* forget servers not heard from for 5 s */
  for (int i = 0; i < sb_n; i++)
    if (frame - sb_list[i].seen > 300) { sb_list[i] = sb_list[--sb_n]; i--; }
}

/* Emulator-only concurrency test. Pause between receive and copy to make a shared-buffer race
 * deterministic. Packets go only to ephemeral loopback ports, with no engine or save data. */
typedef struct {int fd,received,bad;unsigned char value;} recv_test;
static void recv_test_thread(void *arg) {
  recv_test *t=arg;
  for(int tries=0;tries<4000 && t->received<32;tries++) {
    unsigned char out[17];long n=n_recvfrom(t->fd,out,sizeof(out),0x40,NULL,NULL);
    if(n<0) {svcSleepThread(100000);continue;}
    if(n!=sizeof(out)) t->bad=1;
    else for(int i=0;i<sizeof(out);i++) if(out[i]!=t->value) t->bad=1;
    t->received++;
  }
}
int net_selftest(void) {
  if(!net_on) {debugPrintf("[nettest] FAIL LAN sockets unavailable\n");return 1;}
  recv_test contexts[2]={{.fd=-1,.value='A'},{.fd=-1,.value='B'}};
  Thread threads[2];int created[2]={0},started[2]={0},failed=0;
  struct sockaddr_in addresses[2];memset(addresses,0,sizeof(addresses));
  int sender=n_socket(2,2|0x800,0);if(sender<0) failed=1;
  test_recv_pause=1;
  for(int i=0;i<2 && !failed;i++) {
    int fd=contexts[i].fd=n_socket(2,2|0x800,0);
    addresses[i].sin_family=AF_INET;addresses[i].sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    socklen_t len=sizeof(addresses[i]);
    if(fd<0 || bind(fd,(struct sockaddr *)&addresses[i],len) || getsockname(fd,(struct sockaddr *)&addresses[i],&len)) {failed=1;break;}
    Result rc=threadCreate(&threads[i],recv_test_thread,&contexts[i],NULL,0x10000,0x2c,i+1);
    if(R_SUCCEEDED(rc)) {created[i]=1;rc=threadStart(&threads[i]);}
    if(R_FAILED(rc)) {failed=1;break;}started[i]=1;
  }
  if(!failed) for(int p=0;p<32;p++) for(int i=0;i<2;i++) {
    unsigned char message[128];memset(message,contexts[i].value,sizeof(message));
    if(sendto(sender,message,sizeof(message),0,(struct sockaddr *)&addresses[i],sizeof(addresses[i]))!=sizeof(message)) failed=1;
  }
  for(int i=0;i<2;i++) {
    if(started[i]) {threadWaitForExit(&threads[i]);failed |= contexts[i].bad || contexts[i].received!=32;}
    if(created[i]) threadClose(&threads[i]);
    if(contexts[i].fd>=0) {net_forget(contexts[i].fd);close(contexts[i].fd);}
  }
  test_recv_pause=0;if(sender>=0) {net_forget(sender);close(sender);}
  debugPrintf("[nettest] %s concurrent truncated datagrams: %d + %d packets, isolated payloads\n",
              failed ? "FAIL":"PASS",contexts[0].received,contexts[1].received);
  return failed;
}
