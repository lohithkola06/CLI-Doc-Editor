#include "nm_state.h"
#include "nm_search.h"
#include <string.h>
#include <stdio.h>

#define MAX_SS 128
#define MAX_MAP 1024

typedef struct
{
  char file[256];
  char ss_id[64];
} FileMap;

static SSInfo g_ss[MAX_SS];
static int g_ss_n = 0;
static FileMap g_map[MAX_MAP];
static int g_map_n = 0;
static char g_users[4096] = "";

int nm_state_init(void)
{
  g_ss_n = 0;
  g_map_n = 0;
  g_users[0] = 0;
  nm_search_init(); // Initialize efficient search
  return 0;
}

int nm_state_add_ss(const char *ss_id, const char *host, int client_port)
{
  if (g_ss_n >= MAX_SS)
    return -1;
  SSInfo *s = &g_ss[g_ss_n++];
  strncpy(s->ss_id, ss_id, sizeof s->ss_id);
  strncpy(s->host, host, sizeof s->host);
  s->client_port = client_port;
  return 0;
}

static SSInfo *find_ss(const char *ss_id)
{
  for (int i = 0; i < g_ss_n; i++)
    if (!strcmp(g_ss[i].ss_id, ss_id))
      return &g_ss[i];
  return NULL;
}

int nm_state_map_file(const char *file, const char *ss_id)
{
  for (int i = 0; i < g_map_n; i++)
  {
    if (!strcmp(g_map[i].file, file))
    {
      strncpy(g_map[i].ss_id, ss_id, sizeof g_map[i].ss_id - 1);
      g_map[i].ss_id[sizeof g_map[i].ss_id - 1] = '\0';
      nm_search_add_entry(file, ss_id);
      return 0;
    }
  }

  if (g_map_n >= MAX_MAP)
    return -1;

  strncpy(g_map[g_map_n].file, file, sizeof g_map[g_map_n].file - 1);
  g_map[g_map_n].file[sizeof g_map[g_map_n].file - 1] = '\0';
  strncpy(g_map[g_map_n].ss_id, ss_id, sizeof g_map[g_map_n].ss_id - 1);
  g_map[g_map_n].ss_id[sizeof g_map[g_map_n].ss_id - 1] = '\0';
  g_map_n++;

  nm_search_add_entry(file, ss_id);

  return 0;
}

int nm_state_get_route(const char *file, char *host_out, int *port_out)
{
  // Try efficient search first (O(1) average)
  char ss_id[64];
  if (nm_search_lookup(file, ss_id, sizeof(ss_id)) == 0)
  {
    SSInfo *s = find_ss(ss_id);
    if (s)
    {
      strncpy(host_out, s->host, 64);
      *port_out = s->client_port;
      return 0;
    }
  }

  // Fallback to linear search (shouldn't happen in normal operation)
  for (int i = 0; i < g_map_n; i++)
  {
    if (!strcmp(g_map[i].file, file))
    {
      SSInfo *s = find_ss(g_map[i].ss_id);
      if (!s)
        return -1;
      strncpy(host_out, s->host, 64);
      *port_out = s->client_port;
      return 0;
    }
  }
  return -1;
}

int nm_state_get_any_ss(char *host_out, int *port_out)
{
  if (g_ss_n > 0)
  {
    strncpy(host_out, g_ss[0].host, 64);
    *port_out = g_ss[0].client_port;
    return 0;
  }
  return -1;
}

int nm_state_get_ss_id_by_endpoint(const char *host, int port, char *ss_id_out, size_t buflen)
{
  if (!host || !ss_id_out)
    return -1;

  for (int i = 0; i < g_ss_n; i++)
  {
    if (strcmp(g_ss[i].host, host) == 0 && g_ss[i].client_port == port)
    {
      strncpy(ss_id_out, g_ss[i].ss_id, buflen - 1);
      ss_id_out[buflen - 1] = '\0';
      return 0;
    }
  }
  return -1;
}

int nm_state_add_user(const char *user)
{
  // Check if user already exists
  if (g_users[0])
  {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", g_users);
    char *tok = strtok(tmp, ",");
    while (tok)
    {
      // Trim spaces
      while (*tok == ' ')
        tok++;
      char *end = tok + strlen(tok) - 1;
      while (end > tok && *end == ' ')
        end--;
      *(end + 1) = 0;

      if (strcmp(tok, user) == 0)
      {
        return 0; // User already exists, don't add again
      }
      tok = strtok(NULL, ",");
    }
  }

  // Add new user
  if (strlen(g_users) + strlen(user) + 2 >= sizeof g_users)
    return -1;
  if (g_users[0])
    strcat(g_users, ",");
  strcat(g_users, user);
  return 0;
}

int nm_state_list_users(char *buf, size_t buflen)
{
  snprintf(buf, buflen, "%s", g_users[0] ? g_users : "");
  return 0;
}

int nm_state_remove_user(const char *user)
{
  if (!user || !g_users[0])
    return -1;

  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", g_users);

  char new_list[4096] = "";
  char *tok = strtok(tmp, ",");
  int first = 1;
  int removed = 0;

  while (tok)
  {
    while (*tok == ' ')
      tok++;
    char *end = tok + strlen(tok) - 1;
    while (end > tok && *end == ' ')
      end--;
    *(end + 1) = 0;

    if (strcmp(tok, user) != 0)
    {
      if (!first)
        strncat(new_list, ",", sizeof new_list - strlen(new_list) - 1);
      strncat(new_list, tok, sizeof new_list - strlen(new_list) - 1);
      first = 0;
    }
    else
    {
      removed = 1;
    }

    tok = strtok(NULL, ",");
  }

  if (removed)
  {
    snprintf(g_users, sizeof g_users, "%s", new_list);
    return 0;
  }

  return -1;
}

int nm_state_view_all(char *buf, size_t buflen)
{
  // MVP: just list mapped filenames
  buf[0] = 0;
  for (int i = 0; i < g_map_n; i++)
  {
    char line[320];
    snprintf(line, sizeof line, "%s\n", g_map[i].file);
    if (strlen(buf) + strlen(line) + 1 < buflen)
      strcat(buf, line);
  }
  return 0;
}

int nm_state_rename_file(const char *old_file, const char *new_file)
{
  if (!old_file || !new_file)
    return -1;

  for (int i = 0; i < g_map_n; i++)
  {
    if (!strcmp(g_map[i].file, old_file))
    {
      char ss_id[64];
      strncpy(ss_id, g_map[i].ss_id, sizeof ss_id - 1);
      ss_id[sizeof ss_id - 1] = '\0';

      nm_search_remove_entry(old_file);

      strncpy(g_map[i].file, new_file, sizeof g_map[i].file - 1);
      g_map[i].file[sizeof g_map[i].file - 1] = '\0';

      nm_search_add_entry(new_file, ss_id);
      return 0;
    }
  }
  return -1;
}
