#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "../common/net.h"
#include "../common/jsonl.h"

static const char *NM_HOST = "192.168.1.102";
static int NM_PORT = 5050;

extern int cli_readline(char *, int);

static volatile sig_atomic_t sigint_requested = 0;
static char current_user[64];
static int user_registered = 0;
static int user_deregistered = 0;

static void handle_sigint(int signo)
{
  (void)signo;
  sigint_requested = 1;
}

static int nm_request(const char *line, char *out, int outlen)
{
  int fd = tcp_connect(NM_HOST, NM_PORT);
  if (fd < 0)
  {
    perror("cli connect nm");
    return -1;
  }
  send_line(fd, line);
  char *resp = NULL;
  if (recv_line(fd, &resp, 8192) <= 0)
  {
    close(fd);
    return -1;
  }
  snprintf(out, outlen, "%s", resp);
  free(resp);
  close(fd);
  return 0;
}

static void unescape_json_inplace(char *s)
{
  if (!s)
    return;
  char *dst = s;
  for (char *src = s; *src; src++)
  {
    if (*src == '\\' && src[1])
    {
      src++;
      switch (*src)
      {
      case 'n':
        *dst++ = '\n';
        break;
      case 'r':
        *dst++ = '\r';
        break;
      case 't':
        *dst++ = '\t';
        break;
      case '\\':
        *dst++ = '\\';
        break;
      case '"':
        *dst++ = '"';
        break;
      default:
        *dst++ = *src;
        break;
      }
    }
    else
    {
      *dst++ = *src;
    }
  }
  *dst = 0;
}

static int ss_request(const char *host, int port, const char *line, char *out, int outlen)
{
  int fd = tcp_connect(host, port);
  if (fd < 0)
  {
    perror("cli connect ss");
    return -1;
  }
  send_line(fd, line);
  char *resp = NULL;
  if (recv_line(fd, &resp, 8192) <= 0)
  {
    close(fd);
    return -1;
  }
  snprintf(out, outlen, "%s", resp);
  free(resp);
  close(fd);
  return 0;
}

static void send_deregister(void)
{
  if (!user_registered || user_deregistered || !current_user[0])
    return;

  char out[8192];
  if (nm_request(jsonl_build("{\"op\":\"CLI_DEREGISTER\",\"user\":\"%s\"}", current_user), out, sizeof out) == 0)
  {
    printf("[NM] %s\n", out);
  }
  else
  {
    fprintf(stderr, "[WARN] Failed to notify NM about disconnect\n");
  }
  user_deregistered = 1;
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  
  char user[64];
  printf("Username: ");
  fflush(stdout);
  if (!fgets(user, sizeof user, stdin))
    return 0;
  size_t n = strlen(user);
  if (n && user[n - 1] == '\n')
    user[n - 1] = 0;
  strncpy(current_user, user, sizeof current_user - 1);
  current_user[sizeof current_user - 1] = 0;

  char out[8192];
  nm_request(jsonl_build("{\"op\":\"CLI_REGISTER\",\"user\":\"%s\"}", user), out, sizeof out);
  printf("[NM] %s\n", out);
  user_registered = 1;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_sigint;
  sigaction(SIGINT, &sa, NULL);

  char line[1024];
  printf("\nAvailable commands:\n");
  printf("  VIEW [-a] [-l]            - List files\n");
  printf("  READ <file>               - Read file\n");
  printf("  WRITE <file> <sent_idx>   - Edit sentence (use ETIRW to commit)\n");
  printf("  CREATE <file>             - Create file\n");
  printf("  DELETE <file>             - Delete file\n");
  printf("  INFO <file>               - Show file details\n");
  printf("  UNDO <file>               - Undo last edit\n");
  printf("  STREAM <file>             - Stream content\n");
  printf("  LIST                      - List users\n");
  printf("  ADDACCESS -R/-W <file> <user> - Grant access\n");
  printf("  REMACCESS <file> <user>   - Remove access\n");
  printf("  REQUESTACCESS <file> <owner> - Request access to file\n");
  printf("  VIEWREQUESTS              - View pending access requests\n");
  printf("  APPROVE <file> <requester> - Approve access request\n");
  printf("  DENY <file> <requester>   - Deny access request\n");
  printf("  CREATEFOLDER <folder>     - Create a folder\n");
  printf("  MOVE <file> <folder>      - Move file into folder\n");
  printf("  VIEWFOLDER <folder>       - List files in a folder\n");
  printf("  CHECKPOINT <file> <tag>   - Create checkpoint\n");
  printf("  VIEWCHECKPOINT <file> <tag> - View checkpoint\n");
  printf("  REVERT <file> <tag>       - Revert to checkpoint\n");
  printf("  LISTCHECKPOINTS <file>    - List checkpoints\n");
  printf("  EXEC <file>               - Execute file as commands\n");
  printf("  EXIT                      - Quit\n\n");
  
  int should_exit = 0;
  while (!should_exit && cli_readline(line, sizeof line))
  {
    if (sigint_requested)
    {
      printf("\nReceived Ctrl+C, exiting...\n");
      break;
    }

    if (!strcmp(line, "EXIT") || !strcmp(line, "QUIT"))
    {
      should_exit = 1;
      break;
    }

    // Check for VIEW command - must not be VIEWFOLDER, VIEWCHECKPOINT, or VIEWREQUESTS
    if (!strncmp(line, "VIEW", 4) && strncmp(line, "VIEWFOLDER", 10) && strncmp(line, "VIEWCHECKPOINT", 14) && strncmp(line, "VIEWREQUESTS", 12))
    {
      char flags[16] = "";
      if (strlen(line) > 4)
      {
        // Parse flags: -a, -l, -al, -la
        const char *flag_start = line + 4;
        while (*flag_start == ' ')
          flag_start++; // Skip spaces
        if (*flag_start == '-')
        {
          strncpy(flags, flag_start + 1, sizeof flags - 1); // Skip the '-'
        }
      }
      
      // Route through NM to get SS
      nm_request(jsonl_build("{\"op\":\"VIEW_ROUTE\",\"user\":\"%s\"}", user), out, sizeof out);
      char host[64] = "";
      int port = 0;
      json_get_str(out, "ss_host", host, sizeof host);
      json_get_int(out, "ss_port", &port);
      
      if (port > 0)
      {
        char resp[8192];
        ss_request(host, port, jsonl_build("{\"op\":\"LIST\",\"flags\":\"%s\",\"user\":\"%s\"}", flags, user), resp, sizeof resp);
        
        char files[8192];
        if (json_get_str(resp, "files", files, sizeof files) == 0)
        {
          if (strlen(files) > 0)
          {
            // Replace ;; delimiter with newlines for display
            printf("\n📋 Files:\n");
            char *p = files;
            char *delim;
            while ((delim = strstr(p, ";;")) != NULL)
            {
              *delim = '\0';
              if (strlen(p) > 0)
                printf("%s\n", p);
              p = delim + 2;
            }
            printf("\n");
          }
          else
          {
            printf("\n📋 No files found.\n\n");
          }
        }
        else
        {
          printf("❌ Error: %s\n\n", resp);
        }
      }
      else
      {
        printf("❌ Error: No storage server available\n\n");
      }
    }
    else if (!strncmp(line, "READ ", 5))
    {
      const char *file = line + 5;
      nm_request(jsonl_build("{\"op\":\"READ_ROUTE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);

      int status = 1;
      json_get_int(out, "status", &status);
      if (status != 0)
      {
        printf("❌ Error: %s\n\n", out);
        continue;
      }
      
      char host[64] = "";
      int port = 0;
      json_get_str(out, "ss_host", host, sizeof host);
      json_get_int(out, "ss_port", &port);
      
      if (port <= 0 || strlen(host) == 0)
      {
        printf("❌ Error: Could not route to storage server\n\n");
        continue;
      }
      
      char resp[8192];
      ss_request(host, port, jsonl_build("{\"op\":\"READ\",\"user\":\"%s\",\"file\":\"%s\"}", user, file), resp, sizeof resp);
      
      char content[8192];
      if (json_get_str(resp, "content", content, sizeof content) == 0)
      {
        printf("\n📄 %s:\n%s\n", file, content);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
      }
      else
      {
        printf("❌ Error: %s\n\n", resp);
      }
    }
    else if (!strncmp(line, "WRITE ", 6))
    {
      char file[256];
      int sent_idx;
      if (sscanf(line + 6, "%s %d", file, &sent_idx) != 2)
      {
        printf("❌ Usage: WRITE <file> <sentence_idx>\n\n");
        continue;
      }
      
      nm_request(jsonl_build("{\"op\":\"WRITE_ROUTE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      
      int route_status = 1;
      json_get_int(out, "status", &route_status);
      if (route_status != 0)
      {
        printf("❌ Error: %s\n\n", out);
        continue;
      }
      
      char host[64] = "";
      int port = 0;
      json_get_str(out, "ss_host", host, sizeof host);
      json_get_int(out, "ss_port", &port);
      
      if (port <= 0 || strlen(host) == 0)
      {
        printf("❌ Error: Could not route to storage server\n\n");
        continue;
      }
      
      int ss_fd = tcp_connect(host, port);
      if (ss_fd < 0)
      {
        printf("❌ Failed to connect to storage server\n\n");
        continue;
      }
      
      send_line(ss_fd, jsonl_build(
        "{\"op\":\"WRITE_BEGIN\",\"user\":\"%s\",\"file\":\"%s\",\"sentence_idx\":%d}",
        user, file, sent_idx));
      
      char *resp = NULL;
      recv_line(ss_fd, &resp, 8192);
      
      int status = 1;
      json_get_int(resp, "status", &status);
      
      if (status != 0)
      {
        printf("❌ Failed to acquire lock: %s\n\n", resp);
        free(resp);
        close(ss_fd);
        continue;
      }
      
      printf("✅ Lock acquired on sentence %d\n", sent_idx);
      printf("\nEnter edits (format: <word_idx> <content>)\n");
      printf("Type ETIRW when done to commit\n\n");
      
      free(resp);
      
      char edit_line[1024];
      int committed = 0;
      while (cli_readline(edit_line, sizeof edit_line))
      {
        if (!strcmp(edit_line, "ETIRW"))
        {
          send_line(ss_fd, jsonl_build("{\"op\":\"WRITE_COMMIT\",\"file\":\"%s\",\"user\":\"%s\"}", file, user));
          recv_line(ss_fd, &resp, 8192);
          printf("✅ Changes committed!\n\n");
          free(resp);
          committed = 1;
          break;
        }
        else
        {
          int word_idx;
          char content[512];
          if (sscanf(edit_line, "%d %[^\n]", &word_idx, content) == 2)
          {
            send_line(ss_fd, jsonl_build(
              "{\"op\":\"WRITE_EDIT\",\"file\":\"%s\",\"word_index\":%d,\"content\":\"%s\",\"user\":\"%s\"}",
              file, word_idx, content, user));
            recv_line(ss_fd, &resp, 8192);
            printf("✏️  Edit applied\n");
            free(resp);
          }
          else
          {
            printf("❌ Format: <word_idx> <content> or ETIRW\n");
          }
        }
      }
      
      close(ss_fd);
      if (committed)
        printf("💡 Use 'READ %s' to see changes\n\n", file);
    }
    else if (!strncmp(line, "CREATE ", 7))
    {
      const char *file = line + 7;
      nm_request(jsonl_build("{\"op\":\"CREATE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      
      int status = 1;
      json_get_int(out, "status", &status);
      printf("%s %s\n\n", status == 0 ? "✅ Created:" : "❌ Error:", out);
    }
    else if (!strncmp(line, "DELETE ", 7))
    {
      const char *file = line + 7;
      nm_request(jsonl_build("{\"op\":\"DELETE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      
      int status = 1;
      json_get_int(out, "status", &status);
      printf("%s %s\n\n", status == 0 ? "✅ Deleted:" : "❌ Error:", out);
    }
    else if (!strncmp(line, "INFO ", 5))
    {
      const char *file = line + 5;
      nm_request(jsonl_build("{\"op\":\"INFO\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      
      char info[2048];
      if (json_get_str(out, "info", info, sizeof info) == 0)
      {
        char info_copy[2048];
        strncpy(info_copy, info, sizeof info_copy - 1);
        info_copy[sizeof info_copy - 1] = '\0';

        char file_field[256] = "";
        char owner_field[128] = "";
        char created_field[64] = "";
        char modified_field[64] = "";
        char size_field[128] = "";
        char access_field[1024] = "";
        char last_access_field[64] = "";
        char last_access_user[64] = "";

        char *token = strtok(info_copy, "||");
        while (token)
        {
          char *sep = strchr(token, ':');
          if (sep)
          {
            *sep = '\0';
            char *key = token;
            char *value = sep + 1;
            while (*value == ' ')
              value++;

            if (!strcmp(key, "File"))
              strncpy(file_field, value, sizeof file_field - 1);
            else if (!strcmp(key, "Owner"))
              strncpy(owner_field, value, sizeof owner_field - 1);
            else if (!strcmp(key, "Created"))
              strncpy(created_field, value, sizeof created_field - 1);
            else if (!strcmp(key, "LastModified"))
              strncpy(modified_field, value, sizeof modified_field - 1);
            else if (!strcmp(key, "Size"))
              strncpy(size_field, value, sizeof size_field - 1);
            else if (!strcmp(key, "Access"))
              strncpy(access_field, value, sizeof access_field - 1);
            else if (!strcmp(key, "LastAccessed"))
              strncpy(last_access_field, value, sizeof last_access_field - 1);
            else if (!strcmp(key, "LastAccessUser"))
              strncpy(last_access_user, value, sizeof last_access_user - 1);
          }
          token = strtok(NULL, "||");
        }

        printf("\n📊 Info for %s:\n", file);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        if (file_field[0])
          printf("File: %s\n", file_field);
        if (owner_field[0])
          printf("Owner: %s\n", owner_field);
        if (created_field[0])
          printf("Created: %s\n", created_field);
        if (modified_field[0])
          printf("Last Modified: %s\n", modified_field);
        if (size_field[0])
          printf("Size: %s\n", size_field);
        if (access_field[0])
          printf("Access: %s\n", access_field);
        if (last_access_field[0])
        {
          printf("Last Accessed: %s", last_access_field);
          if (last_access_user[0])
            printf(" by %s", last_access_user);
          printf("\n");
        }
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
      }
      else
      {
        printf("❌ Error: %s\n\n", out);
      }
    }
    else if (!strcmp(line, "LIST"))
    {
      nm_request(jsonl_build("{\"op\":\"LIST_USERS\",\"user\":\"%s\"}", user), out, sizeof out);
      
      char users[4096];
      if (json_get_str(out, "users", users, sizeof users) == 0)
      {
        printf("\n👥 Users:\n");
        if (strlen(users) == 0)
        {
          printf("(none)\n\n");
        }
        else
        {
          char users_copy[4096];
          strncpy(users_copy, users, sizeof users_copy - 1);
          users_copy[sizeof users_copy - 1] = '\0';
          char *tok = strtok(users_copy, ",");
          int printed = 0;
          while (tok)
          {
            while (*tok == ' ')
              tok++;
            char *end = tok + strlen(tok) - 1;
            while (end >= tok && (*end == ' ' || *end == '\n'))
            {
              *end = '\0';
              end--;
            }
            if (*tok)
            {
              printf("%s\n", tok);
              printed = 1;
            }
            tok = strtok(NULL, ",");
          }
          if (!printed)
            printf("(none)\n");
          printf("\n");
        }
      }
      else
      {
        printf("❌ Error: %s\n\n", out);
      }
    }
    else if (!strncmp(line, "UNDO ", 5))
    {
      const char *file = line + 5;
      
      nm_request(jsonl_build("{\"op\":\"WRITE_ROUTE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      char host[64] = "";
      int port = 0;
      json_get_str(out, "ss_host", host, sizeof host);
      json_get_int(out, "ss_port", &port);
      
      char resp[8192];
      ss_request(host, port, jsonl_build("{\"op\":\"UNDO\",\"user\":\"%s\",\"file\":\"%s\"}", user, file), resp, sizeof resp);
      
      int status = 1;
      json_get_int(resp, "status", &status);
      printf("%s\n\n", status == 0 ? "↩️  Undo successful!" : "❌ Undo failed");
    }
    else if (!strncmp(line, "STREAM ", 7))
    {
      const char *file = line + 7;
      
      nm_request(jsonl_build("{\"op\":\"STREAM_ROUTE\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      char host[64] = "";
      int port = 0;
      json_get_str(out, "ss_host", host, sizeof host);
      json_get_int(out, "ss_port", &port);
      
      int ss_fd = tcp_connect(host, port);
      if (ss_fd < 0)
      {
        printf("❌ Failed to connect\n\n");
        continue;
      }
      
      send_line(ss_fd, jsonl_build("{\"op\":\"STREAM\",\"user\":\"%s\",\"file\":\"%s\"}", user, file));
      
      printf("\n🎬 Streaming %s:\n", file);
      printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
      
      char *resp = NULL;
      while (recv_line(ss_fd, &resp, 8192) > 0)
      {
        char op[64];
        json_get_str(resp, "op", op, sizeof op);
        
        if (!strcmp(op, "STOP"))
        {
          free(resp);
          break;
        }
        
        if (!strcmp(op, "TOK"))
        {
          char word[256];
          json_get_str(resp, "w", word, sizeof word);
          printf("%s ", word);
          fflush(stdout);
        }
        
        free(resp);
      }
      
      printf("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
      close(ss_fd);
    }
    else if (!strncmp(line, "ADDACCESS ", 10))
    {
      char mode_flag[8], file[256], target_user[64];
      if (sscanf(line + 10, "%s %s %s", mode_flag, file, target_user) == 3)
      {
        char mode[8] = "";
        if (!strcmp(mode_flag, "-R"))
          strcpy(mode, "R");
        else if (!strcmp(mode_flag, "-W"))
          strcpy(mode, "W");
        else
        {
          printf("❌ Use -R for read or -W for write\n\n");
          continue;
        }
        
        nm_request(jsonl_build("{\"op\":\"ADDACCESS\",\"file\":\"%s\",\"mode\":\"%s\",\"target_user\":\"%s\",\"user\":\"%s\"}",
                               file, mode, target_user, user),
                   out, sizeof out);
        
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s %s access for %s\n\n",
               status == 0 ? "✅ Granted" : "❌ Failed to grant",
               mode_flag, target_user);
      }
      else
      {
        printf("❌ Usage: ADDACCESS -R/-W <file> <user>\n\n");
      }
    }
    else if (!strncmp(line, "REMACCESS ", 10))
    {
      char file[256], target_user[64];
      if (sscanf(line + 10, "%s %s", file, target_user) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"REMACCESS\",\"file\":\"%s\",\"target_user\":\"%s\",\"user\":\"%s\"}",
                               file, target_user, user),
                   out, sizeof out);

        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s access for %s\n\n",
               status == 0 ? "✅ Removed" : "❌ Failed to remove", target_user);
      }
      else
      {
        printf("❌ Usage: REMACCESS <file> <user>\n\n");
      }
    }
    else if (!strncmp(line, "EXEC ", 5))
    {
      const char *file = line + 5;
      nm_request(jsonl_build("{\"op\":\"EXEC\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);
      
      char output[8192];
      if (json_get_str(out, "output", output, sizeof output) == 0)
      {
        unescape_json_inplace(output);
        printf("\n💻 Execution output:\n");
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
        printf("%s", output);
        printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
      }
      else
      {
        printf("❌ Error: %s\n\n", out);
      }
    }
    else if (!strncmp(line, "CREATEFOLDER ", 13))
    {
      const char *folder = line + 13;
      nm_request(jsonl_build("{\"op\":\"CREATEFOLDER\",\"folder\":\"%s\",\"user\":\"%s\"}", folder, user), out, sizeof out);
      int status = 1;
      json_get_int(out, "status", &status);
      printf("%s\n\n", status == 0 ? "✅ Folder created" : "❌ Failed to create folder");
    }
    else if (!strncmp(line, "MOVE ", 5))
    {
      char file[256], folder[256];
      if (sscanf(line + 5, "%s %s", file, folder) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"MOVE\",\"file\":\"%s\",\"folder\":\"%s\",\"user\":\"%s\"}", file, folder, user), out, sizeof out);
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s\n\n", status == 0 ? "✅ File moved" : "❌ Failed to move file");
      }
      else
      {
        printf("❌ Usage: MOVE <filename> <foldername>\n\n");
      }
    }
    else if (!strncmp(line, "VIEWFOLDER ", 11))
    {
      const char *folder = line + 11;
      nm_request(jsonl_build("{\"op\":\"VIEWFOLDER\",\"folder\":\"%s\",\"user\":\"%s\"}", folder, user), out, sizeof out);

      char files[4096];
      if (json_get_str(out, "files", files, sizeof files) == 0)
      {
        printf("\n📁 Files in %s:\n", folder);
        if (strlen(files) == 0)
        {
          printf("(empty)\n\n");
        }
        else
        {
          char *p = files;
          char *delim;
          while ((delim = strstr(p, ";;")) != NULL)
          {
            *delim = '\0';
            if (strlen(p) > 0)
              printf("%s\n", p);
            p = delim + 2;
          }
          if (*p)
            printf("%s\n", p);
          printf("\n");
        }
      }
      else
      {
        printf("❌ Error: %s\n\n", out);
      }
    }
    else if (!strncmp(line, "CHECKPOINT ", 11))
    {
      char file[256], tag[128];
      if (sscanf(line + 11, "%s %s", file, tag) == 2)
      {
      nm_request(jsonl_build("{\"op\":\"CHECKPOINT\",\"file\":\"%s\",\"tag\":\"%s\",\"user\":\"%s\"}", file, tag, user), out, sizeof out);
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s\n\n", status == 0 ? "✅ Checkpoint created" : "❌ Failed to create checkpoint");
      }
      else
      {
        printf("❌ Usage: CHECKPOINT <filename> <tag>\n\n");
      }
    }
    else if (!strncmp(line, "VIEWCHECKPOINT ", 15))
    {
      char file[256], tag[128];
      if (sscanf(line + 15, "%s %s", file, tag) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"VIEWCHECKPOINT\",\"file\":\"%s\",\"tag\":\"%s\",\"user\":\"%s\"}", file, tag, user), out, sizeof out);

        char content[8192];
        if (json_get_str(out, "content", content, sizeof content) == 0)
        {
          unescape_json_inplace(content);
          printf("\n📌 Checkpoint %s:\n%s\n\n", tag, content);
        }
        else
        {
          printf("❌ Error: %s\n\n", out);
        }
      }
      else
      {
        printf("❌ Usage: VIEWCHECKPOINT <filename> <tag>\n\n");
      }
    }
    else if (!strncmp(line, "REVERT ", 7))
    {
      char file[256], tag[128];
      if (sscanf(line + 7, "%s %s", file, tag) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"REVERT\",\"file\":\"%s\",\"tag\":\"%s\",\"user\":\"%s\"}", file, tag, user), out, sizeof out);
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s\n\n", status == 0 ? "✅ File reverted" : "❌ Failed to revert");
      }
      else
      {
        printf("❌ Usage: REVERT <filename> <tag>\n\n");
      }
    }
    else if (!strncmp(line, "LISTCHECKPOINTS ", 16))
    {
      const char *file = line + 16;
      nm_request(jsonl_build("{\"op\":\"LISTCHECKPOINTS\",\"file\":\"%s\",\"user\":\"%s\"}", file, user), out, sizeof out);

      char checkpoints[4096];
      if (json_get_str(out, "checkpoints", checkpoints, sizeof checkpoints) == 0)
      {
        printf("\n📋 Checkpoints for %s:\n%s\n\n", file, checkpoints);
      }
      else
      {
        printf("❌ Error: %s\n\n", out);
      }
    }
    else if (!strncmp(line, "REQUESTACCESS ", 14))
    {
      char file[256], owner[64];
      if (sscanf(line + 14, "%s %s", file, owner) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"REQUESTACCESS\",\"file\":\"%s\",\"owner\":\"%s\",\"user\":\"%s\"}",
                               file, owner, user),
                   out, sizeof out);
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s\n\n", status == 0 ? "✅ Access request sent" : "❌ Failed to send request");
      }
      else
      {
        printf("❌ Usage: REQUESTACCESS <file> <owner>\n\n");
      }
    }
    else if (!strcmp(line, "VIEWREQUESTS"))
    {
      nm_request(jsonl_build("{\"op\":\"VIEWREQUESTS\",\"user\":\"%s\"}", user), out, sizeof out);
      char requests[4096];
      if (json_get_str(out, "requests", requests, sizeof requests) == 0)
      {
        if (strlen(requests) > 0)
        {
          printf("\n📬 Pending Access Requests:\n");
          printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
          // Format: file:requester;;file:requester
          char *p = requests;
          char *delim;
          while ((delim = strstr(p, ";;")) != NULL)
          {
            *delim = '\0';
            printf("  %s\n", p);
            p = delim + 2;
          }
          if (strlen(p) > 0)
            printf("  %s\n", p);
          printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");
        }
        else
        {
          printf("\n📬 No pending access requests\n\n");
        }
      }
      else
      {
        printf("❌ Error: %s\n\n", out);
      }
    }
    else if (!strncmp(line, "APPROVE ", 8))
    {
      char file[256], requester[64];
      if (sscanf(line + 8, "%s %s", file, requester) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"RESPONDREQUEST\",\"file\":\"%s\",\"requester\":\"%s\",\"approve\":1,\"user\":\"%s\"}",
                               file, requester, user),
                   out, sizeof out);
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s\n\n", status == 0 ? "✅ Request approved" : "❌ Failed to approve");
      }
      else
      {
        printf("❌ Usage: APPROVE <file> <requester>\n\n");
      }
    }
    else if (!strncmp(line, "DENY ", 5))
    {
      char file[256], requester[64];
      if (sscanf(line + 5, "%s %s", file, requester) == 2)
      {
        nm_request(jsonl_build("{\"op\":\"RESPONDREQUEST\",\"file\":\"%s\",\"requester\":\"%s\",\"approve\":0,\"user\":\"%s\"}",
                               file, requester, user),
                   out, sizeof out);
        int status = 1;
        json_get_int(out, "status", &status);
        printf("%s\n\n", status == 0 ? "✅ Request denied" : "❌ Failed to deny");
      }
      else
      {
        printf("❌ Usage: DENY <file> <requester>\n\n");
      }
    }
    else if (strlen(line) > 0)
    {
      printf("❌ Unknown command. Type a command or EXIT to quit.\n\n");
    }
  }

  if (sigint_requested)
  {
    printf("\nCleaning up...\n");
  }

  if (sigint_requested || should_exit || !user_deregistered)
  {
    send_deregister();
  }

  return 0;
}
