#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "../common/net.h"
#include "../common/jsonl.h"
#include "../common/proto.h"
#include "../common/log.h"
#include "nm_state.h"
#include "nm_access_req.h"
#include "nm_replication.h"

#define NM_LOGFILE "nameserver/nm.log"

static void json_escape(const char *src, char *dst, size_t dst_size)
{
  if (!src || !dst || dst_size == 0)
    return;
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 1 < dst_size; i++)
  {
    const char *rep = NULL;
    switch (src[i])
    {
    case '\\':
      rep = "\\\\";
      break;
    case '\"':
      rep = "\\\"";
      break;
    case '\n':
      rep = "\\n";
      break;
    case '\r':
      rep = "\\r";
      break;
    case '\t':
      rep = "\\t";
      break;
    default:
      break;
    }
    if (rep)
    {
      while (*rep && j + 1 < dst_size)
      {
        dst[j++] = *rep++;
      }
    }
    else
    {
      dst[j++] = src[i];
    }
  }
  dst[j] = 0;
}

static void handle_client(int cfd, const char *session_user)
{
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
  
  char remembered_user[64] = "";
  if (session_user && session_user[0])
  {
    strncpy(remembered_user, session_user, sizeof remembered_user - 1);
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

    char op[64], user[64] = "", file[256] = "", flags[16] = "";
    if (json_get_str(line, "op", op, sizeof op) != 0)
    {
      free(line);
      continue;
    }
    json_get_str(line, "user", user, sizeof user);
    json_get_str(line, "file", file, sizeof file);
    json_get_str(line, "flags", flags, sizeof flags);

    if (!user[0] && remembered_user[0])
    {
      strncpy(user, remembered_user, sizeof user - 1);
    }
    if (user[0])
    {
      strncpy(remembered_user, user, sizeof remembered_user - 1);
    }
    
    // Log incoming request
    log_message("NM", op, client_ip, client_port, user, file[0] ? file : "N/A");
    log_to_file(NM_LOGFILE, jsonl_build("REQ: %s", line));

    if (!strcmp(op, "CLI_REGISTER"))
    {
      log_message("NM", "CLI_REGISTER", client_ip, client_port, user, "Client connected");
      log_to_file(NM_LOGFILE, jsonl_build("REQUEST: op=%s user=%s from %s:%d", op, user, client_ip, client_port));
      nm_state_add_user(user);
      send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"hello %s\"}", user));
      log_to_file(NM_LOGFILE, jsonl_build("RESPONSE: status=0 msg=\"hello %s\"", user));
    }
    else if (!strcmp(op, "CLI_DEREGISTER"))
    {
      if (user[0])
      {
        if (nm_state_remove_user(user) == 0)
        {
          log_message("NM", "CLI_DEREGISTER", client_ip, client_port, user, "Client disconnected");
          log_to_file(NM_LOGFILE, jsonl_build("DISCONNECT: user=%s from %s:%d", user, client_ip, client_port));
        }
      }
      send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"goodbye\"}"));
    }
    else if (!strcmp(op, "VIEW"))
    {
      char buf[4096];
      nm_state_view_all(buf, sizeof buf);
      send_line(cfd, jsonl_build("{\"status\":0,\"files\":\"%s\"}", buf));
    }
    else if (!strcmp(op, "LIST_USERS"))
    {
      char buf[4096];
      nm_state_list_users(buf, sizeof buf);
      send_line(cfd, jsonl_build("{\"status\":0,\"users\":\"%s\"}", buf));
    }
    else if (!strcmp(op, "VIEW_ROUTE"))
    {
      char host[64];
      int port;
      if (nm_state_get_any_ss(host, &port) == 0)
      {
        send_line(cfd, jsonl_build("{\"op\":\"ROUTE\",\"status\":0,\"ss_host\":\"%s\",\"ss_port\":%d}", host, port));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
      }
    }
    else if (!strcmp(op, "READ_ROUTE") || !strcmp(op, "WRITE_ROUTE") || !strcmp(op, "STREAM_ROUTE"))
    {
      char host[64];
      int port;
      if (nm_state_get_route(file, host, &port) == 0)
      {
        send_line(cfd, jsonl_build("{\"op\":\"ROUTE\",\"status\":0,\"ss_host\":\"%s\",\"ss_port\":%d}", host, port));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\",\"msg\":\"file route not found\"}", ERR_NOT_FOUND));
      }
    }
      else if (!strcmp(op, "CREATE"))
    {
        char host[64];
        int port;
        char ss_id[64] = "";
        if (nm_state_get_any_ss(host, &port) == 0 &&
            nm_state_get_ss_id_by_endpoint(host, port, ss_id, sizeof ss_id) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, jsonl_build("{\"op\":\"NM_CREATE\",\"file\":\"%s\",\"owner\":\"%s\"}", file, user));
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            send_line(cfd, resp);
            int status;
            json_get_int(resp, "status", &status);
              if (status == 0)
              {
                nm_state_map_file(file, ss_id);
                nm_replication_map_file(file, ss_id);
              }
            free(resp);
          }
          close(ss_fd);
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
      }
    }
    else if (!strcmp(op, "DELETE"))
    {
      char host[64];
      int port;
      if (nm_state_get_route(file, host, &port) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, jsonl_build("{\"op\":\"NM_DELETE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            send_line(cfd, resp);
            free(resp);
          }
          close(ss_fd);
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "INFO"))
    {
      char host[64];
      int port;
      if (nm_state_get_route(file, host, &port) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, jsonl_build("{\"op\":\"INFO\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            send_line(cfd, resp);
            free(resp);
          }
          close(ss_fd);
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "ADDACCESS") || !strcmp(op, "REMACCESS"))
    {
      char mode[8] = "", target_user[64] = "";
      json_get_str(line, "mode", mode, sizeof mode);
      json_get_str(line, "target_user", target_user, sizeof target_user);
      
      char host[64];
      int port;
      if (nm_state_get_route(file, host, &port) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, jsonl_build("{\"op\":\"NM_ACCESS\",\"file\":\"%s\",\"cmd\":\"%s\",\"mode\":\"%s\",\"target_user\":\"%s\",\"actor\":\"%s\"}",
                                       file, !strcmp(op, "ADDACCESS") ? "ADD" : "REM", mode, target_user, user));
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            send_line(cfd, resp);
            free(resp);
          }
          close(ss_fd);
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "EXEC"))
    {
      char host[64];
      int port;
      if (nm_state_get_route(file, host, &port) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, jsonl_build("{\"op\":\"GET_CONTENT\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            char content[8192];
            if (json_get_str(resp, "content", content, sizeof content) == 0)
            {
              FILE *fp = popen(content, "r");
              if (fp)
              {
                char output[8192] = {0};
                size_t len = fread(output, 1, sizeof output - 1, fp);
                output[len] = 0;
                pclose(fp);
                char escaped_output[16384];
                json_escape(output, escaped_output, sizeof escaped_output);
                send_line(cfd, jsonl_build("{\"status\":0,\"output\":\"%s\"}", escaped_output));
              }
              else
              {
                send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"exec failed\"}", ERR_INTERNAL));
              }
            }
            else
            {
              send_line(cfd, resp);
            }
            free(resp);
          }
          close(ss_fd);
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "CREATEFOLDER") || !strcmp(op, "VIEWFOLDER"))
    {
      char folder[256];
      json_get_str(line, "folder", folder, sizeof folder);
      char host[64];
      int port;
      if (nm_state_get_any_ss(host, &port) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, line);
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            send_line(cfd, resp);
            free(resp);
          }
          close(ss_fd);
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
      }
    }
      else if (!strcmp(op, "MOVE") || !strcmp(op, "CHECKPOINT") || !strcmp(op, "VIEWCHECKPOINT") || !strcmp(op, "REVERT") || !strcmp(op, "LISTCHECKPOINTS"))
    {
      char host[64];
      int port;
      int is_replica = 0;
      if (nm_replication_get_ss(file, host, &port, &is_replica) == 0)
      {
        int ss_fd = tcp_connect(host, port);
        if (ss_fd >= 0)
        {
          send_line(ss_fd, line);
          char *resp = NULL;
          if (recv_line(ss_fd, &resp, 8192) > 0)
          {
            send_line(cfd, resp);

              int status = 1;
              json_get_int(resp, "status", &status);
              if (!strcmp(op, "MOVE") && status == 0)
              {
                char folder[256];
                json_get_str(line, "folder", folder, sizeof folder);
                char new_name[512];
                if (folder[0])
                  snprintf(new_name, sizeof new_name, "%s/%s", folder, file);
                else
                  snprintf(new_name, sizeof new_name, "%s", file);
                nm_state_rename_file(file, new_name);
                nm_replication_rename_file(file, new_name);
              }

            // If it's a write operation, replicate asynchronously
              if ((!strcmp(op, "MOVE") || !strcmp(op, "CHECKPOINT") || !strcmp(op, "REVERT")) && status == 0)
            {
              nm_replication_async_write(file, line);
            }

            free(resp);
          }
          close(ss_fd);
        }
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "REQUESTACCESS"))
    {
      char target_file[256], owner[64];
      json_get_str(line, "file", target_file, sizeof(target_file));
      json_get_str(line, "owner", owner, sizeof(owner));

      int rc = nm_access_req_request(target_file, user, owner);
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"access request sent\"}"));
      }
      else if (rc == ERR_ALREADY_EXISTS)
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"request already pending\"}", ERR_CONFLICT));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed to request access\"}", rc));
      }
    }
    else if (!strcmp(op, "VIEWREQUESTS"))
    {
      char buf[4096];
      int rc = nm_access_req_list_pending(user, buf, sizeof(buf));
      if (rc == OK)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"requests\":\"%s\"}", buf));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"requests\":\"\"}"));
      }
    }
    else if (!strcmp(op, "RESPONDREQUEST"))
    {
      char target_file[256], requester[64];
      int approve = 0;
      json_get_str(line, "file", target_file, sizeof(target_file));
      json_get_str(line, "requester", requester, sizeof(requester));
      json_get_int(line, "approve", &approve);

      int rc = nm_access_req_respond(target_file, requester, user, approve);
      if (rc == OK && approve)
      {
        // Grant access on the storage server
        char host[64];
        int port;
        int is_replica = 0;
        if (nm_replication_get_ss(target_file, host, &port, &is_replica) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            send_line(ss_fd, jsonl_build("{\"op\":\"NM_ACCESS\",\"file\":\"%s\",\"user\":\"%s\",\"mode\":\"R\",\"cmd\":\"ADD\"}", target_file, requester));
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              free(resp);
            }
            close(ss_fd);
          }
        }
        send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"request approved\"}"));
      }
      else if (rc == ERR_UNAUTHORIZED)
      {
        send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"request denied\"}"));
      }
      else
      {
        send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"request not found\"}", ERR_NOT_FOUND));
      }
    }
    else if (!strcmp(op, "SS_HEARTBEAT"))
    {
      char ssid[64];
      json_get_str(line, "ss_id", ssid, sizeof(ssid));
      nm_replication_heartbeat(ssid);
      send_line(cfd, jsonl_build("{\"status\":0}"));
    }
    else
    {
      send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_BAD_REQUEST\",\"msg\":\"unsupported op %s\"}", ERR_BAD_REQUEST, op));
    }
    free(line);
  }
  close(cfd);
}

int main(void)
{
  nm_state_init();
  nm_access_req_init();
  nm_replication_init();

  // Start heartbeat checker thread
  pthread_t heartbeat_thread;
  extern void *nm_replication_heartbeat_checker(void *);
  pthread_create(&heartbeat_thread, NULL, nm_replication_heartbeat_checker, NULL);
  pthread_detach(heartbeat_thread);

  int lfd = tcp_listen(NULL, 5050, 128);
  if (lfd < 0)
  {
    perror("nm listen");
    return 1;
  }
  printf("[NM] Listening on :5050\n");
  printf("[NM] Fault tolerance enabled - heartbeat monitoring active\n");

  for (;;)
  {
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    int cfd = accept(lfd, (struct sockaddr *)&ss, &slen);
    if (cfd < 0)
    {
      perror("accept");
      continue;
    }
    // Peek first line to decide SS or CLI (blocking but fine for MVP)
    char *line = NULL;
    ssize_t n = recv_line(cfd, &line, 8192);
    if (n <= 0)
    {
      free(line);
      close(cfd);
      continue;
    }
    char op[64];
    if (json_get_str(line, "op", op, sizeof op) != 0)
    {
      free(line);
      close(cfd);
      continue;
    }
    // If SS_REGISTER, handle as SS; else treat as CLI with pre-read line
    if (!strcmp(op, "SS_REGISTER"))
    {
      // Get peer info for logging
      struct sockaddr_in addr;
      socklen_t addr_len = sizeof(addr);
      char ss_ip[INET_ADDRSTRLEN] = "127.0.0.1";
      int ss_port = 0;
      
      if (getpeername(cfd, (struct sockaddr *)&addr, &addr_len) == 0)
      {
        inet_ntop(AF_INET, &addr.sin_addr, ss_ip, sizeof(ss_ip));
        ss_port = ntohs(addr.sin_port);
      }
      
      char ssid[64] = "";
      int client_port = 0, nm_port = 0;
      char files[4096] = "";
      char advertised_host[64] = "";
      json_get_str(line, "ss_id", ssid, sizeof ssid);
      json_get_int(line, "ss_client_port", &client_port);
      json_get_int(line, "ss_nm_port", &nm_port);
      json_get_str(line, "files", files, sizeof files);
      json_get_str(line, "ss_host", advertised_host, sizeof advertised_host);

      const char *register_host = advertised_host[0] ? advertised_host : ss_ip;
      
      log_message("NM", "SS_REGISTER", ss_ip, ss_port, ssid, files);
      log_to_file(NM_LOGFILE, jsonl_build("REQ: %s", line));
      
      // Register with replication system
      nm_replication_register_ss(ssid, register_host, client_port, nm_port);

      // Also add to old state system for compatibility
      nm_state_add_ss(ssid, register_host, client_port);

      // Map files with replication
      char files_copy[4096];
      strncpy(files_copy, files, sizeof(files_copy) - 1);
      char *tok = strtok(files_copy, ", ");
      while (tok)
      {
        nm_state_map_file(tok, ssid);
        nm_replication_map_file(tok, ssid);
        tok = strtok(NULL, ", ");
      }
      
      send_line(cfd, jsonl_build("{\"op\":\"NM_ACK\",\"status\":0}"));
      log_to_file(NM_LOGFILE, jsonl_build("RESP: NM_ACK status=0 to ss_id=%s", ssid));
      
      free(line);
      close(cfd);
    }
    else
    {
      // Get peer info for logging
      struct sockaddr_in addr;
      socklen_t addr_len = sizeof(addr);
      char cli_ip[INET_ADDRSTRLEN] = "unknown";
      int cli_port = 0;
      
      if (getpeername(cfd, (struct sockaddr *)&addr, &addr_len) == 0)
      {
        inet_ntop(AF_INET, &addr.sin_addr, cli_ip, sizeof(cli_ip));
        cli_port = ntohs(addr.sin_port);
      }
      
      // CLI connection - process first line inline, then continue with handler
      char user[64] = "", file[256] = "", flags[16] = "";
      json_get_str(line, "user", user, sizeof user);
      json_get_str(line, "file", file, sizeof file);
      json_get_str(line, "flags", flags, sizeof flags);
      char session_user[64] = "";
      if (user[0])
      {
        strncpy(session_user, user, sizeof session_user - 1);
      }
      
      log_message("NM", op, cli_ip, cli_port, user, file[0] ? file : "N/A");
      log_to_file(NM_LOGFILE, jsonl_build("REQ: %s", line));
      
      // Process the first command
      if (!strcmp(op, "CLI_REGISTER"))
      {
        nm_state_add_user(user);
        send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"hello %s\"}", user));
        log_to_file(NM_LOGFILE, jsonl_build("RESP: status=0 user=%s", user));
      }
      else if (!strcmp(op, "CLI_DEREGISTER"))
      {
        if (user[0])
        {
          if (nm_state_remove_user(user) == 0)
          {
            log_message("NM", "CLI_DEREGISTER", cli_ip, cli_port, user, "Client disconnected");
            log_to_file(NM_LOGFILE, jsonl_build("DISCONNECT: user=%s from %s:%d", user, cli_ip, cli_port));
          }
        }
        send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"goodbye\"}"));
      }
      else if (!strcmp(op, "VIEW"))
      {
        char buf[4096];
        nm_state_view_all(buf, sizeof buf);
        send_line(cfd, jsonl_build("{\"status\":0,\"files\":\"%s\"}", buf));
      }
      else if (!strcmp(op, "LIST_USERS"))
      {
        char buf[4096];
        nm_state_list_users(buf, sizeof buf);
        send_line(cfd, jsonl_build("{\"status\":0,\"users\":\"%s\"}", buf));
      }
      else if (!strcmp(op, "VIEW_ROUTE"))
      {
        char host[64];
        int port;
        if (nm_state_get_any_ss(host, &port) == 0)
        {
          send_line(cfd, jsonl_build("{\"op\":\"ROUTE\",\"status\":0,\"ss_host\":\"%s\",\"ss_port\":%d}", host, port));
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
        }
      }
      else if (!strcmp(op, "READ_ROUTE") || !strcmp(op, "WRITE_ROUTE") || !strcmp(op, "STREAM_ROUTE"))
      {
        char host[64];
        int port;
        if (nm_state_get_route(file, host, &port) == 0)
        {
          send_line(cfd, jsonl_build("{\"op\":\"ROUTE\",\"status\":0,\"ss_host\":\"%s\",\"ss_port\":%d}", host, port));
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"code\":\"ERR_NOT_FOUND\",\"msg\":\"file route not found\"}", ERR_NOT_FOUND));
        }
      }
      else if (!strcmp(op, "CREATE"))
      {
        char host[64];
        int port;
        char ss_id[64] = "";
        if (nm_state_get_any_ss(host, &port) == 0 &&
            nm_state_get_ss_id_by_endpoint(host, port, ss_id, sizeof ss_id) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            send_line(ss_fd, jsonl_build("{\"op\":\"NM_CREATE\",\"file\":\"%s\",\"owner\":\"%s\"}", file, user));
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              send_line(cfd, resp);
              int status;
              json_get_int(resp, "status", &status);
              if (status == 0)
              {
                nm_state_map_file(file, ss_id);
                nm_replication_map_file(file, ss_id);
              }
              free(resp);
            }
            close(ss_fd);
          }
          else
          {
            send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
          }
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
        }
      }
      else if (!strcmp(op, "DELETE") || !strcmp(op, "INFO"))
      {
        char host[64];
        int port;
        if (nm_state_get_route(file, host, &port) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            if (!strcmp(op, "DELETE"))
            {
              send_line(ss_fd, jsonl_build("{\"op\":\"NM_DELETE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
            }
            else
            {
              send_line(ss_fd, jsonl_build("{\"op\":\"INFO\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
            }
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              send_line(cfd, resp);
              free(resp);
            }
            close(ss_fd);
          }
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
        }
      }
      else if (!strcmp(op, "ADDACCESS") || !strcmp(op, "REMACCESS"))
      {
        char mode[8] = "", target_user[64] = "";
        json_get_str(line, "mode", mode, sizeof mode);
        json_get_str(line, "target_user", target_user, sizeof target_user);
        
        char host[64];
        int port;
        if (nm_state_get_route(file, host, &port) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            send_line(ss_fd, jsonl_build("{\"op\":\"NM_ACCESS\",\"file\":\"%s\",\"cmd\":\"%s\",\"mode\":\"%s\",\"target_user\":\"%s\",\"actor\":\"%s\"}",
                                         file, !strcmp(op, "ADDACCESS") ? "ADD" : "REM", mode, target_user, user));
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              send_line(cfd, resp);
              free(resp);
            }
            close(ss_fd);
          }
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
        }
      }
      else if (!strcmp(op, "EXEC"))
      {
        char host[64];
        int port;
        if (nm_state_get_route(file, host, &port) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            send_line(ss_fd, jsonl_build("{\"op\":\"GET_CONTENT\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              char content[8192];
              if (json_get_str(resp, "content", content, sizeof content) == 0)
              {
                FILE *fp = popen(content, "r");
                if (fp)
                {
                  char output[8192] = {0};
                  size_t len = fread(output, 1, sizeof output - 1, fp);
                  output[len] = 0;
                  pclose(fp);
                  char escaped_output[16384];
                  json_escape(output, escaped_output, sizeof escaped_output);
                  send_line(cfd, jsonl_build("{\"status\":0,\"output\":\"%s\"}", escaped_output));
                }
                else
                {
                  send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"exec failed\"}", ERR_INTERNAL));
                }
              }
              else
              {
                send_line(cfd, resp);
              }
              free(resp);
            }
            close(ss_fd);
          }
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
        }
      }
      else if (!strcmp(op, "CREATEFOLDER") || !strcmp(op, "VIEWFOLDER"))
      {
        char folder[256];
        json_get_str(line, "folder", folder, sizeof folder);
        char host[64];
        int port;
        if (nm_state_get_any_ss(host, &port) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            send_line(ss_fd, line);
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              send_line(cfd, resp);
              free(resp);
            }
            close(ss_fd);
          }
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"no SS available\"}", ERR_INTERNAL));
        }
      }
      else if (!strcmp(op, "MOVE") || !strcmp(op, "CHECKPOINT") || !strcmp(op, "VIEWCHECKPOINT") || !strcmp(op, "REVERT") || !strcmp(op, "LISTCHECKPOINTS"))
      {
        char host[64];
        int port;
        int is_replica = 0;
        if (nm_replication_get_ss(file, host, &port, &is_replica) == 0)
        {
          int ss_fd = tcp_connect(host, port);
          if (ss_fd >= 0)
          {
            send_line(ss_fd, line);
            char *resp = NULL;
            if (recv_line(ss_fd, &resp, 8192) > 0)
            {
              send_line(cfd, resp);

              int status = 1;
              json_get_int(resp, "status", &status);

              if (!strcmp(op, "MOVE") && status == 0)
              {
                char folder[256];
                json_get_str(line, "folder", folder, sizeof folder);
                char new_name[512];
                if (folder[0])
                  snprintf(new_name, sizeof new_name, "%s/%s", folder, file);
                else
                  snprintf(new_name, sizeof new_name, "%s", file);
                nm_state_rename_file(file, new_name);
                nm_replication_rename_file(file, new_name);
              }

              // If it's a write operation, replicate asynchronously
              if ((!strcmp(op, "MOVE") || !strcmp(op, "CHECKPOINT") || !strcmp(op, "REVERT")) && status == 0)
              {
                nm_replication_async_write(file, line);
              }

              free(resp);
            }
            close(ss_fd);
          }
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"file not found\"}", ERR_NOT_FOUND));
        }
      }
      else if (!strcmp(op, "REQUESTACCESS"))
      {
        char target_file[256], owner[64];
        json_get_str(line, "file", target_file, sizeof(target_file));
        json_get_str(line, "owner", owner, sizeof(owner));

        int rc = nm_access_req_request(target_file, user, owner);
        if (rc == OK)
        {
          send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"access request sent\"}"));
        }
        else if (rc == ERR_ALREADY_EXISTS)
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"request already pending\"}", ERR_CONFLICT));
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"failed to request access\"}", rc));
        }
      }
      else if (!strcmp(op, "VIEWREQUESTS"))
      {
        printf("[DEBUG] VIEWREQUESTS handler called for user=%s\n", user);
        char buf[4096];
        int rc = nm_access_req_list_pending(user, buf, sizeof(buf));
        printf("[DEBUG] nm_access_req_list_pending returned rc=%d, buf='%s'\n", rc, buf);
        if (rc == OK)
        {
          send_line(cfd, jsonl_build("{\"status\":0,\"requests\":\"%s\"}", buf));
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":0,\"requests\":\"\"}"));
        }
      }
      else if (!strcmp(op, "RESPONDREQUEST"))
      {
        char target_file[256], requester[64];
        int approve = 0;
        json_get_str(line, "file", target_file, sizeof(target_file));
        json_get_str(line, "requester", requester, sizeof(requester));
        json_get_int(line, "approve", &approve);

        int rc = nm_access_req_respond(target_file, requester, user, approve);
        if (rc == OK && approve)
        {
          // Grant access on the storage server
          char host[64];
          int port;
          int is_replica = 0;
          if (nm_replication_get_ss(target_file, host, &port, &is_replica) == 0)
          {
            int ss_fd = tcp_connect(host, port);
            if (ss_fd >= 0)
            {
              send_line(ss_fd, jsonl_build("{\"op\":\"NM_ACCESS\",\"file\":\"%s\",\"user\":\"%s\",\"mode\":\"R\",\"cmd\":\"ADD\"}", target_file, requester));
              char *resp = NULL;
              if (recv_line(ss_fd, &resp, 8192) > 0)
              {
                free(resp);
              }
              close(ss_fd);
            }
          }
          send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"request approved\"}"));
        }
        else if (rc == ERR_UNAUTHORIZED)
        {
          send_line(cfd, jsonl_build("{\"status\":0,\"msg\":\"request denied\"}"));
        }
        else
        {
          send_line(cfd, jsonl_build("{\"status\":%d,\"msg\":\"request not found\"}", ERR_NOT_FOUND));
        }
      }
      else if (!strcmp(op, "SS_HEARTBEAT"))
      {
        char ssid[64];
        json_get_str(line, "ss_id", ssid, sizeof(ssid));
        nm_replication_heartbeat(ssid);
        send_line(cfd, jsonl_build("{\"status\":0}"));
      }
      
      free(line);
      // Continue with the handler for subsequent commands
      handle_client(cfd, session_user);
    }
  }
  return 0;
}
