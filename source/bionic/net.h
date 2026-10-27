#ifndef PG3D_NET_H
#define PG3D_NET_H

#include <stddef.h>
#include <stdint.h>

/* Socket fds start here, below select's FD_SETSIZE */
#define NET_FD_BASE 512

/* Reads the server config and brings sceNet up; stays offline without it */
void net_init(void);
int net_enabled(void);
const char *net_server_ip(void);
int net_http_port(void);
/* a host of the game's own (Photon, Rilisoft) that goes to the server */
int net_is_game_host(const char *host);

/* fd routing from io.c / libc.c */
int net_is_fd(int fd);
int net_sid(int fd); /* the sceNet socket behind an fd, or -1 */
int net_close(int fd);
long net_read(int fd, void *b, size_t n);
long net_write(int fd, const void *b, size_t n);
int net_fcntl(int fd, int cmd, int arg);
int net_ioctl(int fd, int req, void *arg);

int b_inet_aton(const char *s, uint32_t *out);
char *b_inet_ntoa(uint32_t a);

#endif
