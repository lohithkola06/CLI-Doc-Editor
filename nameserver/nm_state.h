#ifndef NM_STATE_H
#define NM_STATE_H
#include <stddef.h>

typedef struct {
  char ss_id[64];
  char host[64];
  int  client_port;
} SSInfo;

int nm_state_init(void);
int nm_state_add_ss(const char *ss_id, const char *host, int client_port);
int nm_state_map_file(const char *file, const char *ss_id);
int nm_state_get_route(const char *file, char *host_out, int *port_out);
int nm_state_get_any_ss(char *host_out, int *port_out);
int nm_state_get_ss_id_by_endpoint(const char *host, int port, char *ss_id_out, size_t buflen);
int nm_state_rename_file(const char *old_file, const char *new_file);
int nm_state_add_user(const char *user);
int nm_state_list_users(char *buf, size_t buflen);
int nm_state_remove_user(const char *user);
int nm_state_view_all(char *buf, size_t buflen);
#endif

