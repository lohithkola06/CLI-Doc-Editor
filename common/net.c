#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

static int tcp_socket_addr(const char *host, const char *port, int passive, struct addrinfo **res) {
  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  int rc = getaddrinfo(host, port, &hints, res);
  if (rc != 0) { fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc)); return -1; }
  return 0;
}

int tcp_listen(const char *host, int port, int backlog) {
  char pbuf[16]; snprintf(pbuf, sizeof pbuf, "%d", port);
  struct addrinfo *ai = NULL; if (tcp_socket_addr(host, pbuf, 1, &ai) != 0) return -1;
  int fd=-1;
  for (struct addrinfo *p=ai; p; p=p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd<0) continue;
    int yes=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    if (bind(fd, p->ai_addr, p->ai_addrlen)==0 && listen(fd, backlog)==0) break;
    close(fd); fd=-1;
  }
  freeaddrinfo(ai);
  return fd;
}

int tcp_connect(const char *host, int port) {
  char pbuf[16]; snprintf(pbuf, sizeof pbuf, "%d", port);
  struct addrinfo *ai=NULL; if (tcp_socket_addr(host, pbuf, 0, &ai)!=0) return -1;
  int fd=-1;
  for (struct addrinfo *p=ai; p; p=p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd<0) continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen)==0) break;
    close(fd); fd=-1;
  }
  freeaddrinfo(ai);
  return fd;
}

int send_all(int fd, const void *buf, size_t len) {
  const char *p = (const char*)buf;
  size_t sent=0;
  while (sent<len) {
    ssize_t n = send(fd, p+sent, len-sent, 0);
    if (n<0) { if (errno==EINTR) continue; return -1; }
    if (n==0) return -1;
    sent += (size_t)n;
  }
  return 0;
}

int send_line(int fd, const char *line) {
  size_t len = strlen(line);
  if (send_all(fd, line, len) < 0) return -1;
  return send_all(fd, "\n", 1);
}

ssize_t recv_line(int fd, char **out, size_t max) {
  size_t cap = max ? max : 8192;
  char *buf = malloc(cap+1);
  if (!buf) return -1;
  size_t n=0;
  while (n<cap) {
    char c; ssize_t r = recv(fd, &c, 1, 0);
    if (r<0) { if (errno==EINTR) continue; free(buf); return -1; }
    if (r==0) { if (n==0) { free(buf); return 0; } break; }
    buf[n++] = c;
    if (c=='\n') break;
  }
  buf[n]=0;
  *out = buf;
  return (ssize_t)n;
}

int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags<0) return -1;
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

