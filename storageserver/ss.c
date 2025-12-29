#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/net.h"
#include "../common/jsonl.h"
#include "../common/proto.h"
#include "../common/log.h"
#include "ss_files.h"

#define SS_LOGFILE "storageserver/ss.log"

static const char *NM_HOST = "192.168.1.102";
static int NM_PORT = 5050;
static const char *SS_ID = "ss-1";
static int SS_CLIENT_PORT = 6001;
static int SS_NM_PORT = 6000;

static void append_file_to_list(char *file_list, size_t file_list_size, const char *entry, int *first)
{
  if (!file_list || !entry || file_list_size == 0 || !first)
    return;

  if (!*first)
  {
    size_t remaining = file_list_size - strlen(file_list) - 1;
    if (remaining > 0)
      strncat(file_list, ",", remaining);
  }

  size_t remaining = file_list_size - strlen(file_list) - 1;
  if (remaining > 0)
    strncat(file_list, entry, remaining);

  *first = 0;
}

static void scan_dir(const char *base_path, const char *rel_path, char *file_list, size_t file_list_size, int *first)
{
  char full_path[512];
  snprintf(full_path, sizeof(full_path), "%s%s%s", base_path,
           (rel_path && rel_path[0]) ? "/" : "", rel_path ? rel_path : "");

  DIR *dir = opendir(full_path);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
  {
    if (entry->d_name[0] == '.' || strcmp(entry->d_name, ".gitkeep") == 0)
      continue;

    char item_path[512];
    snprintf(item_path, sizeof(item_path), "%s/%s", full_path, entry->d_name);

    struct stat st;
    if (stat(item_path, &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode))
    {
      char new_rel_path[512];
      snprintf(new_rel_path, sizeof(new_rel_path), "%s%s%s",
               rel_path && rel_path[0] ? rel_path : "",
               (rel_path && rel_path[0]) ? "/" : "",
               entry->d_name);
      scan_dir(base_path, new_rel_path, file_list, file_list_size, first);
    }
    else if (S_ISREG(st.st_mode))
    {
      char entry_name[512];
      if (rel_path && rel_path[0])
        snprintf(entry_name, sizeof(entry_name), "%s/%s", rel_path, entry->d_name);
      else
        snprintf(entry_name, sizeof(entry_name), "%s", entry->d_name);
      append_file_to_list(file_list, file_list_size, entry_name, first);
    }
  }

  closedir(dir);
}

static void register_with_nm(void)
{
  int fd = -1;
  int retries = 10;

  // Retry connection to NM with backoff
  while (retries-- > 0)
  {
    fd = tcp_connect(NM_HOST, NM_PORT);
    if (fd >= 0)
      break;
    printf("[SS] Waiting for Name Server... (retries left: %d)\n", retries);
    sleep(1);
  }

  if (fd < 0)
  {
    fprintf(stderr, "[SS] Failed to connect to Name Server after retries\n");
    exit(1);
  }

  printf("[SS] Connected to Name Server\n");

  // Determine local IP address used for this connection
  char local_ip[INET_ADDRSTRLEN] = "127.0.0.1";
  struct sockaddr_in local_addr;
  socklen_t local_len = sizeof(local_addr);
  if (getsockname(fd, (struct sockaddr *)&local_addr, &local_len) == 0)
  {
    inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof local_ip);
  }

  // Build list of actual files from disk (recursively scan directories)
  char file_list[4096] = "";
  int first = 1;
  scan_dir("storageserver/data/files", "", file_list, sizeof(file_list), &first);

  // Register with NM (send simple comma-separated list, not JSON array)
  char *msg = jsonl_build("{\"op\":\"SS_REGISTER\",\"ss_id\":\"%s\",\"ss_client_port\":%d,\"ss_nm_port\":%d,\"ss_host\":\"%s\",\"files\":\"%s\"}",
                          SS_ID, SS_CLIENT_PORT, SS_NM_PORT, local_ip, file_list);
  send_line(fd, msg);
  char *line = NULL;
  recv_line(fd, &line, 8192);
  if (line)
  {
    printf("[SS] NM reply: %s\n", line);
    free(line);
  }
  close(fd);
}

// Heartbeat thread - sends periodic heartbeats to NM
static void *heartbeat_thread(void *arg)
{
  (void)arg;
  
  while (1)
  {
    sleep(5); // Send heartbeat every 5 seconds
    
    int fd = tcp_connect(NM_HOST, NM_PORT);
    if (fd >= 0)
    {
      char *msg = jsonl_build("{\"op\":\"SS_HEARTBEAT\",\"ss_id\":\"%s\"}", SS_ID);
      send_line(fd, msg);
      
      // Read response (optional, just to clear the buffer)
      char *resp = NULL;
      recv_line(fd, &resp, 1024);
      if (resp)
        free(resp);
      
      close(fd);
    }
  }
  
  return NULL;
}

static void *handle_client_thread(void *arg)
{
  int cfd = *(int *)arg;
  free(arg);

  // Get client address for logging
  struct sockaddr_in addr;
  socklen_t addr_len = sizeof(addr);
  char client_ip[INET_ADDRSTRLEN] = "unknown";
  int client_port = 0;

  if (getpeername(cfd, (struct sockaddr *)&addr, &addr_len) == 0)
  {
    inet_ntop(AF_INET, &addr.sin_addr, client_ip, sizeof(client_ip));
    client_port = ntohs(addr.sin_port);
  }

  for (;;)
  {
    char *line = NULL;
    ssize_t n = recv_line(cfd, &line, 8192);
    if (n <= 0)
    {
      free(line);
      break;
    }

    char op[64], user[64] = "", file[256] = "";
    if (json_get_str(line, "op", op, sizeof op) != 0)
    {
      free(line);
      continue;
    }
    json_get_str(line, "user", user, sizeof user);
    json_get_str(line, "file", file, sizeof file);

    // Log request
    log_message("SS", op, client_ip, client_port, user, file[0] ? file : "N/A");
    log_to_file(SS_LOGFILE, jsonl_build("REQ: %s", line));

    if (!strcmp(op, "READ"))
    {
      char content[8192];
      int rc = ss_files_read(file, user, content, sizeof content);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"op\":\"DATA\",\"status\":0,\"content\":\"%s\"}", content));
      }
      else if (rc == ERR_UNAUTHORIZED)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_UNAUTHORIZED\",\"msg\":\"access denied\"}", ERR_UNAUTHORIZED));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\",\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "WRITE_BEGIN"))
    {
      int sent_idx = 0;
      json_get_int(line, "sentence_idx", &sent_idx);
      int rc = ss_files_write_begin(file, user, sent_idx);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"lock acquired\"}");
      }
      else if (rc == ERR_LOCKED)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_LOCKED\",\"msg\":\"sentence locked\"}", ERR_LOCKED));
      }
      else if (rc == ERR_NOT_FOUND)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\",\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_BAD_REQUEST\",\"msg\":\"invalid request\"}", ERR_BAD_REQUEST));
      }
    }
    else if (!strcmp(op, "WRITE_EDIT"))
    {
      int word_idx = 0;
      char content[1024];
      json_get_int(line, "word_index", &word_idx);
      json_get_str(line, "content", content, sizeof content);
      int rc = ss_files_write_edit(file, user, word_idx, content);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"edit applied\"}");
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_BAD_REQUEST\",\"msg\":\"edit failed\"}", ERR_BAD_REQUEST));
      }
    }
    else if (!strcmp(op, "WRITE_COMMIT"))
    {
      int rc = ss_files_write_commit(file, user);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"committed\"}");
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_INTERNAL\",\"msg\":\"commit failed\"}", ERR_INTERNAL));
      }
    }
    else if (!strcmp(op, "UNDO"))
    {
      int rc = ss_files_undo(file, user);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"undo successful\"}");
      }
      else if (rc == ERR_NOT_FOUND)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\",\"msg\":\"no undo history\"}", ERR_NOT_FOUND));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_INTERNAL\",\"msg\":\"undo failed\"}", ERR_INTERNAL));
      }
    }
    else if (!strcmp(op, "STREAM"))
    {
      char content[8192];
      int rc = ss_files_read(file, user, content, sizeof content);
      if (rc == OK)
      {
        char *word = strtok(content, " .");
        while (word)
        {
          char buf[256];
          snprintf(buf, sizeof buf, "{\"op\":\"TOK\",\"w\":\"%s\"}", word);
          send_line(cfd, buf);
          usleep(100000);
          word = strtok(NULL, " .");
        }
        send_line(cfd, "{\"op\":\"STOP\"}");
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\",\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "NM_CREATE"))
    {
      char owner[64];
      json_get_str(line, "owner", owner, sizeof owner);
      int rc = ss_files_create(file, owner);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"file created\"}");
      }
      else if (rc == ERR_CONFLICT)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_CONFLICT\",\"msg\":\"file exists\"}", ERR_CONFLICT));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_INTERNAL\",\"msg\":\"creation failed\"}", ERR_INTERNAL));
      }
    }
    else if (!strcmp(op, "NM_DELETE"))
    {
      int rc = ss_files_delete(file, user);
      send_line(cfd, rc == OK ? "{\"status\":0,\"msg\":\"deleted\"}" : "{\"status\":6,\"msg\":\"delete failed\"}");
    }
    else if (!strcmp(op, "INFO"))
    {
      char info[2048];
      int rc = ss_files_get_info(file, info, sizeof info);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"op\":\"INFO\",\"status\":0,\"info\":\"%s\"}", info));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "LIST"))
    {
      char flags[16] = "";
      json_get_str(line, "flags", flags, sizeof flags);
      int include_all = (strstr(flags, "a") != NULL);
      int include_details = (strstr(flags, "l") != NULL);

      char list[8192];
      ss_files_list_all(list, sizeof list, include_all, include_details, user);
      send_line(cfd, jsonl_build("{\"op\":\"LIST\",\"status\":0,\"files\":\"%s\"}", list));
    }
    else if (!strcmp(op, "NM_ACCESS"))
    {
      char cmd[16], mode[8], target_user[64];
      json_get_str(line, "cmd", cmd, sizeof cmd);
      json_get_str(line, "mode", mode, sizeof mode);
      json_get_str(line, "target_user", target_user, sizeof target_user);
      char actor[64] = "";
      json_get_str(line, "actor", actor, sizeof actor);

      int rc;
      if (!strcmp(cmd, "ADD"))
      {
        rc = ss_files_add_access(file, actor, target_user, mode);
      }
      else
      {
        rc = ss_files_remove_access(file, actor, target_user);
      }

      send_line(cfd, rc == OK ? "{\"status\":0,\"msg\":\"access updated\"}" : "{\"status\":6,\"msg\":\"failed\"}");
    }
    else if (!strcmp(op, "GET_CONTENT"))
    {
      // For EXEC - return raw content
      char content[8192];
      int rc = ss_files_read(file, user, content, sizeof content);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"content\":\"%s\"}", content));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "CREATEFOLDER"))
    {
      char folder[256];
      json_get_str(line, "folder", folder, sizeof folder);
      int rc = ss_files_create_folder(folder);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"folder created\"}");
      }
      else if (rc == ERR_ALREADY_EXISTS)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"folder already exists\"}", ERR_ALREADY_EXISTS));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed to create folder\"}", ERR_INTERNAL));
      }
    }
    else if (!strcmp(op, "MOVE"))
    {
      char folder[256];
      json_get_str(line, "folder", folder, sizeof folder);
      int rc = ss_files_move_file(file, folder);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"file moved\"}");
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed to move file\"}", rc));
      }
    }
    else if (!strcmp(op, "VIEWFOLDER"))
    {
      char folder[256];
      json_get_str(line, "folder", folder, sizeof folder);
      char files[4096];
      int rc = ss_files_view_folder(folder, files, sizeof files);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"files\":\"%s\"}", files));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"folder not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "CHECKPOINT"))
    {
      char tag[128];
      json_get_str(line, "tag", tag, sizeof tag);
      int rc = ss_files_create_checkpoint(file, tag);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"checkpoint created\"}");
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed to create checkpoint\"}", rc));
      }
    }
    else if (!strcmp(op, "VIEWCHECKPOINT"))
    {
      char tag[128];
      json_get_str(line, "tag", tag, sizeof tag);
      char content[8192];
      int rc = ss_files_view_checkpoint(file, tag, content, sizeof content);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"content\":\"%s\"}", content));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"checkpoint not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "REVERT"))
    {
      char tag[128];
      json_get_str(line, "tag", tag, sizeof tag);
      int rc = ss_files_revert_checkpoint(file, tag);
      if (rc == OK)
      {
        send_line(cfd, "{\"status\":0,\"msg\":\"file reverted\"}");
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed to revert\"}", rc));
      }
    }
    else if (!strcmp(op, "LISTCHECKPOINTS"))
    {
      char checkpoints[4096];
      int rc = ss_files_list_checkpoints(file, checkpoints, sizeof checkpoints);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"checkpoints\":\"%s\"}", checkpoints));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed\"}", rc));
      }
    }
    else
    {
      send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_BAD_REQUEST\",\"msg\":\"unsupported op: %s\"}", ERR_BAD_REQUEST, op));
    }
    free(line);
  }
  close(cfd);
  return NULL;
}

int main(int argc, char **argv)
{
  // parse flags (omitted: set SS_ID / ports from args)
  (void)argc;
  (void)argv;

  // Initialize file subsystem
  printf("[SS] Initializing file system...\n");
  if (ss_files_init() != OK)
  {
    fprintf(stderr, "[SS] Failed to initialize file system\n");
    return 1;
  }

  register_with_nm();
  
  // Start heartbeat thread
  pthread_t hb_thread;
  if (pthread_create(&hb_thread, NULL, heartbeat_thread, NULL) != 0)
  {
    fprintf(stderr, "[SS] Failed to create heartbeat thread\n");
    return 1;
  }
  pthread_detach(hb_thread);
  printf("[SS] Heartbeat thread started\n");
  
  int lfd = tcp_listen(NULL, SS_CLIENT_PORT, 128);
  if (lfd < 0)
  {
    perror("ss listen");
    return 1;
  }
  printf("[SS] Listening on :%d\n", SS_CLIENT_PORT);

  for (;;)
  {
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
    {
      perror("accept");
      continue;
    }

    int *cfd_ptr = malloc(sizeof(int));
    if (!cfd_ptr)
    {
      perror("malloc");
      close(cfd);
      continue;
    }
    *cfd_ptr = cfd;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&thread, &attr, handle_client_thread, cfd_ptr) != 0)
    {
      perror("pthread_create");
      free(cfd_ptr);
      close(cfd);
    }

    pthread_attr_destroy(&attr);
  }
}
