#ifndef NET_H
#define NET_H
#include <stddef.h>
#include <sys/types.h>

// Returns fd or -1
int tcp_listen(const char *host, int port, int backlog);
int tcp_connect(const char *host, int port);

// Returns 0 on success, -1 on error
int send_all(int fd, const void *buf, size_t len);
int send_line(int fd, const char *line); // appends '\n'

// Reads one line (up to '\n'), allocates *out (caller free). Returns bytes read, 0 on EOF, -1 on error
ssize_t recv_line(int fd, char **out, size_t max);

// Set CLOEXEC + nonblocking helpers (optional)
int set_cloexec(int fd);
#endif

