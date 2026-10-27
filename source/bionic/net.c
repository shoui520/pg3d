/*
 * Networking over sceNet, for the private server.
 *
 * ux0:data/pg3d/server.txt names the server ("<ipv4> [http port]"). With it,
 * sockets are real and the game's own hosts (Photon, the Rilisoft backend)
 * resolve to that server; any other name fails to resolve, as offline. Without
 * it the port stays offline: sockets are refused with ENETUNREACH.
 *
 * Sockets take bionic fds from NET_FD_BASE up (below select's 1024), so
 * close/read/write/fcntl/ioctl/poll route here by number. Bionic is Linux:
 * errno values, sockaddr (16-bit family, no length byte), option names and
 * message flags are translated to the BSD ones sceNet uses.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "bionic/bionic.h"
#include "bionic/net.h"
#include "util/log.h"

#define SERVER_FILE "ux0:data/pg3d/server.txt"
#define NET_POOL (1024 * 1024)
#define NET_FDS 64

/* Linux values */
#define L_AF_INET 2
#define L_SOCK_TYPE_MASK 0xf
#define L_SOCK_NONBLOCK 0x800
#define L_O_NONBLOCK 0x800
#define L_SOL_SOCKET 1
#define L_IPPROTO_IP 0
#define L_IPPROTO_TCP 6
#define L_POLLIN 0x1
#define L_POLLPRI 0x2
#define L_POLLOUT 0x4
#define L_POLLERR 0x8
#define L_POLLHUP 0x10
#define L_POLLNVAL 0x20
#define L_FIONREAD 0x541B
#define L_FIONBIO 0x5421
#define L_EBADF 9
#define L_EINVAL 22
#define L_EMFILE 24
#define L_ENOTSOCK 88
#define L_EAFNOSUPPORT 97
#define L_ENETUNREACH 101
#define L_EIO 5
#define EAI_AGAIN_ 2
#define EAI_FAIL_ 4
#define EAI_FAMILY_ 5
#define EAI_MEMORY_ 6
#define EAI_NONAME_ 8
#define EAI_SERVICE_ 9
#define AI_PASSIVE_ 1
#define AI_CANONNAME_ 2
#define NI_NAMEREQD_ 8

typedef struct {
  int used;
  int sid;
  int nonblock;
} nsock;

static nsock socks[NET_FDS];
static SceKernelLwMutexWork lock;
static int enabled;
static uint32_t server_addr; /* network order */
static char server_ip[16];
static int http_port = 8080;

/* ---------- setup ---------- */

void net_init(void) {
  sceKernelCreateLwMutex(&lock, "net", 0, 0, NULL);
  FILE *f = fopen(SERVER_FILE, "r");
  if (!f) {
    LOG("[net] offline (no " SERVER_FILE ")");
    return;
  }
  char ip[64] = "";
  int port = 0;
  int n = fscanf(f, "%63s %d", ip, &port);
  fclose(f);
  if (n < 1 || !b_inet_aton(ip, &server_addr)) {
    LOG("[net] offline: " SERVER_FILE " should hold an IPv4 address (and optionally the HTTP port)");
    return;
  }
  snprintf(server_ip, sizeof(server_ip), "%s", ip);
  if (n == 2 && port > 0 && port < 65536)
    http_port = port;

  int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  if (r < 0) {
    LOG("[net] offline: loading the net module failed: 0x%08X", r);
    return;
  }
  void *pool = malloc(NET_POOL);
  SceNetInitParam p = {pool, NET_POOL, 0};
  r = sceNetInit(&p);
  if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY) {
    LOG("[net] offline: sceNetInit failed: 0x%08X", r);
    free(pool);
    return;
  }
  r = sceNetCtlInit();
  SceNetCtlInfo info;
  if (r >= 0 && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
    LOG("[net] online: this Vita is %s, server %s (http %d)", info.ip_address, server_ip, http_port);
  else
    LOG("[net] online: server %s (http %d); no IP address yet (Wi-Fi off?)", server_ip, http_port);
  enabled = 1;
}

int net_enabled(void) {
  return enabled;
}

const char *net_server_ip(void) {
  return server_ip;
}

int net_http_port(void) {
  return http_port;
}

/* Hosts the game talks to: Photon, the Rilisoft backend and its configs */
static const char *const game_domains[] = {"exitgamescloud.com", "pixelgunserver.com", "pixelgun3d.com",
                                           "rilisoft.info"};

int net_is_game_host(const char *host) {
  size_t n = strlen(host);
  for (size_t i = 0; i < sizeof(game_domains) / sizeof(game_domains[0]); i++) {
    size_t d = strlen(game_domains[i]);
    if (n >= d && strcasecmp(host + n - d, game_domains[i]) == 0 && (n == d || host[n - d - 1] == '.'))
      return 1;
  }
  return 0;
}

/* ---------- translation ---------- */

/* sceNet error (0x80410100 | BSD errno) -> Linux errno */
static int lx_errno(int r) {
  if (((unsigned)r & 0xFFFFFF00u) != 0x80410100u)
    return L_EIO;
  int e = r & 0xFF;
  if (e >= 38 && e <= 60)
    return e + 50; /* ENOTSOCK .. ETIMEDOUT */
  switch (e) {
  case 35: return 11;  /* EAGAIN */
  case 36: return 115; /* EINPROGRESS */
  case 37: return 114; /* EALREADY */
  case 61: return 111; /* ECONNREFUSED */
  case 62: return 40;  /* ELOOP */
  case 63: return 36;  /* ENAMETOOLONG */
  case 64: return 112; /* EHOSTDOWN */
  case 65: return 113; /* EHOSTUNREACH */
  case 11: return 35;  /* EDEADLK */
  default: return e > 0 && e < 35 ? e : L_EIO;
  }
}

static int fail(int r) {
  bionic_set_errno(lx_errno(r));
  return -1;
}

static int err(int e) {
  bionic_set_errno(e);
  return -1;
}

static nsock *get(int fd) {
  if (!net_is_fd(fd))
    return NULL;
  nsock *s = &socks[fd - NET_FD_BASE];
  return s->used ? s : NULL;
}

int net_is_fd(int fd) {
  return fd >= NET_FD_BASE && fd < NET_FD_BASE + NET_FDS;
}

int net_sid(int fd) {
  nsock *s = get(fd);
  return s ? s->sid : -1;
}

/* Linux sockaddr_in: u16 family, port, address, zeros */
static int to_sce(const void *a, int len, SceNetSockaddrIn *out) {
  const uint8_t *p = a;
  if (!a || len < 8)
    return L_EINVAL;
  if ((p[0] | p[1] << 8) != L_AF_INET)
    return L_EAFNOSUPPORT;
  memset(out, 0, sizeof(*out));
  out->sin_len = sizeof(*out);
  out->sin_family = SCE_NET_AF_INET;
  memcpy(&out->sin_port, p + 2, 2);
  memcpy(&out->sin_addr, p + 4, 4);
  return 0;
}

static void from_sce(const SceNetSockaddrIn *in, void *a, int *len) {
  if (!a || !len)
    return;
  uint8_t buf[16] = {L_AF_INET, 0};
  memcpy(buf + 2, &in->sin_port, 2);
  memcpy(buf + 4, &in->sin_addr, 4);
  memcpy(a, buf, *len < 16 ? *len : 16);
  *len = 16;
}

static int msg_flags(int f) {
  return (f & 2 ? SCE_NET_MSG_PEEK : 0) | (f & 0x40 ? SCE_NET_MSG_DONTWAIT : 0) | (f & 0x100 ? SCE_NET_MSG_WAITALL : 0);
}

enum { OPT_PLAIN, OPT_TIMEO, OPT_ERROR, OPT_IGNORE };

/* Linux (level, name) -> sceNet; the kind says how the value converts */
static int map_opt(int lvl, int opt, int *slvl, int *sopt) {
  if (lvl == L_SOL_SOCKET) {
    *slvl = SCE_NET_SOL_SOCKET;
    switch (opt) {
    case 2: *sopt = SCE_NET_SO_REUSEADDR; return OPT_PLAIN;
    case 3: *sopt = SCE_NET_SO_TYPE; return OPT_PLAIN;
    case 4: *sopt = SCE_NET_SO_ERROR; return OPT_ERROR;
    case 6: *sopt = SCE_NET_SO_BROADCAST; return OPT_PLAIN;
    case 7: *sopt = SCE_NET_SO_SNDBUF; return OPT_PLAIN;
    case 8: *sopt = SCE_NET_SO_RCVBUF; return OPT_PLAIN;
    case 9: *sopt = SCE_NET_SO_KEEPALIVE; return OPT_PLAIN;
    case 10: *sopt = SCE_NET_SO_OOBINLINE; return OPT_PLAIN;
    case 13: *sopt = SCE_NET_SO_LINGER; return OPT_PLAIN;
    case 15: *sopt = SCE_NET_SO_REUSEPORT; return OPT_PLAIN;
    case 18: *sopt = SCE_NET_SO_RCVLOWAT; return OPT_PLAIN;
    case 19: *sopt = SCE_NET_SO_SNDLOWAT; return OPT_PLAIN;
    case 20: *sopt = SCE_NET_SO_RCVTIMEO; return OPT_TIMEO;
    case 21: *sopt = SCE_NET_SO_SNDTIMEO; return OPT_TIMEO;
    }
  } else if (lvl == L_IPPROTO_IP) {
    *slvl = SCE_NET_IPPROTO_IP;
    switch (opt) {
    case 1: *sopt = SCE_NET_IP_TOS; return OPT_PLAIN;
    case 2: *sopt = SCE_NET_IP_TTL; return OPT_PLAIN;
    case 32: *sopt = SCE_NET_IP_MULTICAST_IF; return OPT_PLAIN;
    case 33: *sopt = SCE_NET_IP_MULTICAST_TTL; return OPT_PLAIN;
    case 34: *sopt = SCE_NET_IP_MULTICAST_LOOP; return OPT_PLAIN;
    case 35: *sopt = SCE_NET_IP_ADD_MEMBERSHIP; return OPT_PLAIN;
    case 36: *sopt = SCE_NET_IP_DROP_MEMBERSHIP; return OPT_PLAIN;
    }
  } else if (lvl == L_IPPROTO_TCP) {
    *slvl = SCE_NET_IPPROTO_TCP;
    switch (opt) {
    case 1: *sopt = SCE_NET_TCP_NODELAY; return OPT_PLAIN;
    case 2: *sopt = SCE_NET_TCP_MAXSEG; return OPT_PLAIN;
    }
  }
  return OPT_IGNORE;
}

/* ---------- sockets ---------- */

int b_socket(int domain, int type, int proto) {
  if (!enabled) {
    LOG("[net] socket(%d, %d, %d) refused (offline)", domain, type, proto);
    return err(L_ENETUNREACH);
  }
  if (domain != L_AF_INET)
    return err(L_EAFNOSUPPORT);
  int t = type & L_SOCK_TYPE_MASK;
  if (t != SCE_NET_SOCK_STREAM && t != SCE_NET_SOCK_DGRAM)
    return err(93); /* EPROTONOSUPPORT */
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = 0;
  while (i < NET_FDS && socks[i].used)
    i++;
  if (i == NET_FDS) {
    sceKernelUnlockLwMutex(&lock, 1);
    return err(L_EMFILE);
  }
  int sid = sceNetSocket(t == SCE_NET_SOCK_STREAM ? "pg3d_tcp" : "pg3d_udp", SCE_NET_AF_INET, t, proto);
  if (sid < 0) {
    sceKernelUnlockLwMutex(&lock, 1);
    LOG("[net] socket(%d, %d, %d): 0x%08X", domain, type, proto, sid);
    return fail(sid);
  }
  socks[i].used = 1;
  socks[i].sid = sid;
  socks[i].nonblock = 0;
  sceKernelUnlockLwMutex(&lock, 1);
  if (type & L_SOCK_NONBLOCK) {
    int on = 1;
    sceNetSetsockopt(sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
    socks[i].nonblock = 1;
  }
  LOG("[net] socket(%s) = fd %d lr=%p", t == SCE_NET_SOCK_STREAM ? "tcp" : "udp", NET_FD_BASE + i,
      __builtin_return_address(0));
  return NET_FD_BASE + i;
}

int net_close(int fd) {
  sceKernelLockLwMutex(&lock, 1, NULL);
  nsock *s = get(fd);
  if (!s) {
    sceKernelUnlockLwMutex(&lock, 1);
    return err(L_EBADF);
  }
  int sid = s->sid;
  s->used = 0;
  sceKernelUnlockLwMutex(&lock, 1);
  /* wake a thread blocked in recv on it, as closing does on Linux */
  sceNetSocketAbort(sid, 0);
  sceNetSocketClose(sid);
  DLOG("[net] close(fd %d)", fd);
  return 0;
}

#define SOCK(fd)          \
  nsock *s = get(fd);     \
  if (!s)                 \
    return err(net_is_fd(fd) ? L_EBADF : L_ENOTSOCK)

int b_connect(int fd, const void *a, int l) {
  SOCK(fd);
  SceNetSockaddrIn sa;
  int e = to_sce(a, l, &sa);
  if (e)
    return err(e);
  int r = sceNetConnect(s->sid, (SceNetSockaddr *)&sa, sizeof(sa));
  const uint8_t *ip = (const uint8_t *)&sa.sin_addr;
  LOG("[net] connect(fd %d, %u.%u.%u.%u:%u) = 0x%08X", fd, ip[0], ip[1], ip[2], ip[3],
      (unsigned)((sa.sin_port & 0xff) << 8 | sa.sin_port >> 8), r);
  return r < 0 ? fail(r) : 0;
}

int b_bind(int fd, const void *a, int l) {
  SOCK(fd);
  SceNetSockaddrIn sa;
  int e = to_sce(a, l, &sa);
  if (e)
    return err(e);
  int r = sceNetBind(s->sid, (SceNetSockaddr *)&sa, sizeof(sa));
  return r < 0 ? fail(r) : 0;
}

int b_listen(int fd, int n) {
  SOCK(fd);
  int r = sceNetListen(s->sid, n);
  return r < 0 ? fail(r) : 0;
}

int b_accept(int fd, void *a, int *l) {
  SOCK(fd);
  SceNetSockaddrIn sa;
  unsigned int sl = sizeof(sa);
  int sid = sceNetAccept(s->sid, (SceNetSockaddr *)&sa, &sl);
  if (sid < 0)
    return fail(sid);
  sceKernelLockLwMutex(&lock, 1, NULL);
  int i = 0;
  while (i < NET_FDS && socks[i].used)
    i++;
  if (i == NET_FDS) {
    sceKernelUnlockLwMutex(&lock, 1);
    sceNetSocketClose(sid);
    return err(L_EMFILE);
  }
  socks[i].used = 1;
  socks[i].sid = sid;
  socks[i].nonblock = 0;
  sceKernelUnlockLwMutex(&lock, 1);
  from_sce(&sa, a, l);
  return NET_FD_BASE + i;
}

long b_send(int fd, const void *b, size_t n, int f) {
  SOCK(fd);
  int r = sceNetSend(s->sid, b, n, msg_flags(f));
  return r < 0 ? fail(r) : r;
}

long b_sendto(int fd, const void *b, size_t n, int f, const void *a, int l) {
  SOCK(fd);
  if (!a)
    return b_send(fd, b, n, f);
  SceNetSockaddrIn sa;
  int e = to_sce(a, l, &sa);
  if (e)
    return err(e);
  int r = sceNetSendto(s->sid, b, n, msg_flags(f), (SceNetSockaddr *)&sa, sizeof(sa));
  return r < 0 ? fail(r) : r;
}

long b_recv(int fd, void *b, size_t n, int f) {
  SOCK(fd);
  int r = sceNetRecv(s->sid, b, n, msg_flags(f));
  return r < 0 ? fail(r) : r;
}

long b_recvfrom(int fd, void *b, size_t n, int f, void *a, int *l) {
  SOCK(fd);
  SceNetSockaddrIn sa;
  unsigned int sl = sizeof(sa);
  int r = sceNetRecvfrom(s->sid, b, n, msg_flags(f), a ? (SceNetSockaddr *)&sa : NULL, a ? &sl : NULL);
  if (r < 0)
    return fail(r);
  if (a)
    from_sce(&sa, a, l);
  return r;
}

long net_read(int fd, void *b, size_t n) {
  return b_recv(fd, b, n, 0);
}

long net_write(int fd, const void *b, size_t n) {
  return b_send(fd, b, n, 0);
}

int b_shutdown(int fd, int how) {
  SOCK(fd);
  int r = sceNetShutdown(s->sid, how);
  return r < 0 ? fail(r) : 0;
}

int b_setsockopt(int fd, int lvl, int opt, const void *v, int l) {
  SOCK(fd);
  int slvl, sopt;
  int kind = map_opt(lvl, opt, &slvl, &sopt);
  if (kind == OPT_IGNORE || kind == OPT_ERROR) {
    DLOG("[net] setsockopt(fd %d, %d, %d) ignored", fd, lvl, opt);
    return 0;
  }
  int r;
  if (kind == OPT_TIMEO) {
    const int32_t *tv = v; /* struct timeval: seconds, microseconds */
    int us = l >= 8 ? tv[0] * 1000000 + tv[1] : 0;
    r = sceNetSetsockopt(s->sid, slvl, sopt, &us, sizeof(us));
  } else if (l == 1) {
    int x = *(const uint8_t *)v;
    r = sceNetSetsockopt(s->sid, slvl, sopt, &x, sizeof(x));
  } else {
    if (sopt == SCE_NET_IP_ADD_MEMBERSHIP || sopt == SCE_NET_IP_DROP_MEMBERSHIP)
      l = 8; /* ip_mreqn starts like ip_mreq */
    r = sceNetSetsockopt(s->sid, slvl, sopt, v, l);
  }
  if (r < 0)
    LOG("[net] setsockopt(fd %d, %d, %d) = 0x%08X", fd, lvl, opt, r);
  return r < 0 ? fail(r) : 0;
}

int b_getsockopt(int fd, int lvl, int opt, void *v, int *l) {
  SOCK(fd);
  int slvl, sopt;
  int kind = map_opt(lvl, opt, &slvl, &sopt);
  if (kind == OPT_IGNORE) {
    if (v && l && *l >= 4)
      *(int *)v = 0;
    if (l)
      *l = 4;
    return 0;
  }
  int x[2] = {0, 0};
  unsigned int xl = sopt == SCE_NET_SO_LINGER ? 8 : 4;
  int r = sceNetGetsockopt(s->sid, slvl, sopt, x, &xl);
  if (r < 0)
    return fail(r);
  if (kind == OPT_TIMEO) {
    int32_t tv[2] = {x[0] / 1000000, x[0] % 1000000};
    memcpy(v, tv, *l < 8 ? *l : 8);
    *l = 8;
    return 0;
  }
  if (kind == OPT_ERROR && x[0])
    x[0] = lx_errno(0x80410100 | x[0]);
  memcpy(v, x, *l < (int)xl ? *l : (int)xl);
  *l = xl;
  return 0;
}

int b_getsockname(int fd, void *a, int *l) {
  SOCK(fd);
  SceNetSockaddrIn sa;
  unsigned int sl = sizeof(sa);
  int r = sceNetGetsockname(s->sid, (SceNetSockaddr *)&sa, &sl);
  if (r < 0)
    return fail(r);
  from_sce(&sa, a, l);
  return 0;
}

static int set_nonblock(nsock *s, int on) {
  int r = sceNetSetsockopt(s->sid, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &on, sizeof(on));
  if (r < 0)
    return fail(r);
  s->nonblock = on;
  return 0;
}

int net_fcntl(int fd, int cmd, int arg) {
  SOCK(fd);
  switch (cmd) {
  case 1: /* F_GETFD */
  case 2: /* F_SETFD */
    return 0;
  case 3: /* F_GETFL */
    return 2 | (s->nonblock ? L_O_NONBLOCK : 0);
  case 4: /* F_SETFL */
    return set_nonblock(s, (arg & L_O_NONBLOCK) != 0);
  }
  LOG("[net] fcntl(fd %d, %d) unsupported", fd, cmd);
  return err(L_EINVAL);
}

int net_ioctl(int fd, int req, void *arg) {
  SOCK(fd);
  if (req == L_FIONBIO)
    return set_nonblock(s, *(int *)arg != 0);
  if (req == L_FIONREAD) {
    /* no FIONREAD in sceNet: peek at what is queued */
    static char peek[16384];
    int r = sceNetRecv(s->sid, peek, sizeof(peek), SCE_NET_MSG_PEEK | SCE_NET_MSG_DONTWAIT);
    *(int *)arg = r > 0 ? r : 0;
    return 0;
  }
  LOG("[net] ioctl(fd %d, 0x%X) unsupported", fd, req);
  return err(L_EINVAL);
}

/* ---------- poll / select ---------- */

struct b_pollfd {
  int fd;
  short events, revents;
};

#define POLL_MAX 64

int b_poll(struct b_pollfd *fds, unsigned n, int timeout) {
  int ready = 0, nets = 0;
  for (unsigned i = 0; i < n; i++) {
    fds[i].revents = 0;
    if (fds[i].fd < 0)
      continue;
    if (get(fds[i].fd))
      nets++;
    else
      fds[i].revents = L_POLLNVAL; /* pipes and files: not pollable here */
    ready += fds[i].revents != 0;
  }
  if (!nets) {
    if (!ready && timeout > 0)
      sceKernelDelayThread(timeout * 1000);
    return ready;
  }
  int eid = sceNetEpollCreate("bpoll", 0);
  if (eid < 0)
    return fail(eid);
  for (unsigned i = 0; i < n; i++) {
    nsock *s = fds[i].fd >= 0 ? get(fds[i].fd) : NULL;
    if (!s)
      continue;
    SceNetEpollEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = (fds[i].events & (L_POLLIN | L_POLLPRI) ? SCE_NET_EPOLLIN : 0) |
                (fds[i].events & L_POLLOUT ? SCE_NET_EPOLLOUT : 0);
    ev.data.u32 = i;
    if (sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, s->sid, &ev) < 0) {
      fds[i].revents = L_POLLNVAL;
      ready++;
    }
  }
  SceNetEpollEvent evs[POLL_MAX];
  int want = n < POLL_MAX ? (int)n : POLL_MAX;
  int got;
  for (;;) {
    int us = ready || timeout == 0 ? 0 : timeout < 0 || timeout > 1000 ? 1000000 : timeout * 1000;
    got = sceNetEpollWait(eid, evs, want, us);
    if (got < 0 && got != (int)SCE_NET_ERROR_EINTR)
      break;
    if (got > 0 || ready || (timeout >= 0 && timeout <= 1000))
      break;
    if (timeout > 0)
      timeout -= 1000;
  }
  sceNetEpollDestroy(eid);
  if (got < 0 && got != (int)SCE_NET_ERROR_EINTR)
    return fail(got);
  for (int k = 0; k < got; k++) {
    unsigned i = evs[k].data.u32;
    if (i >= n)
      continue;
    short r = 0;
    if (evs[k].events & SCE_NET_EPOLLIN)
      r |= fds[i].events & (L_POLLIN | L_POLLPRI);
    if (evs[k].events & SCE_NET_EPOLLOUT)
      r |= fds[i].events & L_POLLOUT;
    if (evs[k].events & SCE_NET_EPOLLERR)
      r |= L_POLLERR;
    if (evs[k].events & SCE_NET_EPOLLHUP)
      r |= L_POLLHUP;
    if (r && !fds[i].revents)
      ready++;
    fds[i].revents |= r;
  }
  return ready;
}

struct b_timeval {
  int32_t sec, usec;
};

#define FD_ISSET_(fd, set) ((set)[(fd) / 32] & (1u << ((fd) % 32)))

int b_select(int nfds, uint32_t *r, uint32_t *w, uint32_t *e, struct b_timeval *tv) {
  struct b_pollfd p[POLL_MAX];
  int n = 0;
  for (int fd = 0; fd < nfds && fd < 1024 && n < POLL_MAX; fd++) {
    short ev = (r && FD_ISSET_(fd, r) ? L_POLLIN : 0) | (w && FD_ISSET_(fd, w) ? L_POLLOUT : 0) |
               (e && FD_ISSET_(fd, e) ? L_POLLPRI : 0);
    if (ev && get(fd)) {
      p[n].fd = fd;
      p[n].events = ev;
      n++;
    }
  }
  int timeout = tv ? tv->sec * 1000 + tv->usec / 1000 : -1;
  int got = b_poll(p, n, timeout);
  if (got < 0)
    return -1;
  int words = (nfds + 31) / 32;
  if (r)
    memset(r, 0, words * 4);
  if (w)
    memset(w, 0, words * 4);
  if (e)
    memset(e, 0, words * 4);
  int count = 0;
  for (int i = 0; i < n; i++) {
    int fd = p[i].fd;
    short rv = p[i].revents;
    if (r && rv & (L_POLLIN | L_POLLHUP | L_POLLERR)) {
      r[fd / 32] |= 1u << (fd % 32);
      count++;
    }
    if (w && rv & (L_POLLOUT | L_POLLERR)) {
      w[fd / 32] |= 1u << (fd % 32);
      count++;
    }
    if (e && rv & L_POLLPRI) {
      e[fd / 32] |= 1u << (fd % 32);
      count++;
    }
  }
  return count;
}

/* ---------- names ---------- */

/* the game's hosts -> the server; numbers and localhost as they are */
static int resolve(const char *name, uint32_t *addr) {
  if (b_inet_aton(name, addr))
    return 1;
  if (strcasecmp(name, "localhost") == 0) {
    *addr = 0x0100007f;
    return 1;
  }
  if (enabled && net_is_game_host(name)) {
    *addr = server_addr;
    return 1;
  }
  return 0;
}

struct b_addrinfo {
  int flags, family, socktype, protocol;
  uint32_t addrlen;
  char *canonname;
  void *addr;
  struct b_addrinfo *next;
};

static struct b_addrinfo *addrinfo(uint32_t addr, int port, int socktype, int protocol, const char *canon) {
  size_t cl = canon ? strlen(canon) + 1 : 0;
  struct b_addrinfo *ai = calloc(1, sizeof(*ai) + 16 + cl);
  if (!ai)
    return NULL;
  uint8_t *sa = (uint8_t *)(ai + 1);
  sa[0] = L_AF_INET;
  sa[2] = port >> 8;
  sa[3] = port;
  memcpy(sa + 4, &addr, 4);
  ai->family = L_AF_INET;
  ai->socktype = socktype;
  ai->protocol = protocol;
  ai->addrlen = 16;
  ai->addr = sa;
  if (canon) {
    ai->canonname = (char *)sa + 16;
    memcpy(ai->canonname, canon, cl);
  }
  return ai;
}

int b_getaddrinfo(const char *node, const char *service, const void *hints, void **res) {
  const struct b_addrinfo *h = hints;
  *res = NULL;
  if (h && h->family != 0 && h->family != L_AF_INET)
    return EAI_FAMILY_;
  int port = 0;
  if (service && *service) {
    char *end;
    long p = strtol(service, &end, 10);
    if (*end == 0 && p >= 0 && p < 65536)
      port = p;
    else if (strcmp(service, "http") == 0)
      port = 80;
    else if (strcmp(service, "https") == 0)
      port = 443;
    else
      return EAI_SERVICE_;
  }
  uint32_t addr;
  if (!node)
    addr = h && h->flags & AI_PASSIVE_ ? 0 : 0x0100007f;
  else if (!resolve(node, &addr)) {
    LOG("[net] getaddrinfo(%s) -> not found%s", node, enabled ? "" : " (offline)");
    return enabled ? EAI_NONAME_ : EAI_FAIL_;
  }
  const char *canon = node && h && h->flags & AI_CANONNAME_ ? node : NULL;
  int st = h ? h->socktype : 0, pr = h ? h->protocol : 0;
  struct b_addrinfo *first;
  if (st) {
    first = addrinfo(addr, port, st, pr, canon);
  } else {
    first = addrinfo(addr, port, SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP, canon);
    if (first)
      first->next = addrinfo(addr, port, SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP, NULL);
  }
  if (!first)
    return EAI_MEMORY_;
  if (node && !b_inet_aton(node, &(uint32_t){0}))
    LOG("[net] getaddrinfo(%s) -> %s", node, b_inet_ntoa(addr));
  *res = first;
  return 0;
}

void b_freeaddrinfo(void *p) {
  struct b_addrinfo *ai = p;
  while (ai) {
    struct b_addrinfo *next = ai->next;
    free(ai);
    ai = next;
  }
}

int b_getnameinfo(const void *sa, int salen, char *host, int hostlen, char *serv, int servlen, int flags) {
  const uint8_t *p = sa;
  if (!sa || salen < 8 || (p[0] | p[1] << 8) != L_AF_INET)
    return EAI_FAMILY_;
  if (host && hostlen) {
    if (flags & NI_NAMEREQD_)
      return EAI_NONAME_;
    snprintf(host, hostlen, "%u.%u.%u.%u", p[4], p[5], p[6], p[7]);
  }
  if (serv && servlen)
    snprintf(serv, servlen, "%u", p[2] << 8 | p[3]);
  return 0;
}

const char *b_gai_strerror(int e) {
  switch (e) {
  case EAI_AGAIN_: return "Temporary failure in name resolution";
  case EAI_FAIL_: return "Network unavailable (offline)";
  case EAI_FAMILY_: return "ai_family not supported";
  case EAI_MEMORY_: return "Memory allocation failure";
  case EAI_NONAME_: return "No address associated with hostname";
  case EAI_SERVICE_: return "Servname not supported for ai_socktype";
  }
  return "Unknown error";
}

struct b_hostent {
  char *h_name;
  char **h_aliases;
  int h_addrtype, h_length;
  char **h_addr_list;
};

void *b_gethostbyname(const char *name) {
  static struct b_hostent he;
  static char hname[256];
  static uint32_t haddr;
  static char *aliases[1], *addrs[2];
  if (!name || !resolve(name, &haddr)) {
    LOG("[net] gethostbyname(%s) -> not found%s", name ? name : "-", enabled ? "" : " (offline)");
    return NULL;
  }
  snprintf(hname, sizeof(hname), "%s", name);
  addrs[0] = (char *)&haddr;
  addrs[1] = NULL;
  aliases[0] = NULL;
  he.h_name = hname;
  he.h_aliases = aliases;
  he.h_addrtype = L_AF_INET;
  he.h_length = 4;
  he.h_addr_list = addrs;
  return &he;
}

/* ---------- address text helpers (IPv4; network byte order) ---------- */

int b_inet_aton(const char *s, uint32_t *out) {
  unsigned a, b, c, d;
  char tail;
  if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return 0;
  uint8_t *p = (uint8_t *)out;
  p[0] = a;
  p[1] = b;
  p[2] = c;
  p[3] = d;
  return 1;
}

uint32_t b_inet_addr(const char *s) {
  uint32_t a;
  return b_inet_aton(s, &a) ? a : 0xffffffffu;
}

char *b_inet_ntoa(uint32_t a) {
  static char buf[16];
  const uint8_t *p = (const uint8_t *)&a;
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
  return buf;
}

int b_inet_pton(int af, const char *s, void *dst) {
  if (af != L_AF_INET) {
    bionic_set_errno(L_EAFNOSUPPORT);
    return -1;
  }
  return b_inet_aton(s, dst);
}

const char *b_inet_ntop(int af, const void *src, char *dst, int size) {
  if (af != L_AF_INET) {
    bionic_set_errno(L_EAFNOSUPPORT);
    return NULL;
  }
  const uint8_t *p = src;
  snprintf(dst, size, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
  return dst;
}
