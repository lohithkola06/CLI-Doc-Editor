#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

void log_message(const char *component, const char *operation, const char *ip, int port, const char *user, const char *details)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);

  time_t now = tv.tv_sec;
  struct tm *tm_info = localtime(&now);

  char timestamp[64];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

  long millis = tv.tv_usec / 1000;

  // Print to console
  printf("[%s.%03ld] [%s] %s | IP:%s Port:%d User:%s | %s\n",
         timestamp, millis, component, operation,
         ip ? ip : "N/A", port, user ? user : "N/A", details ? details : "");
  fflush(stdout);
}

void log_to_file(const char *logfile, const char *message)
{
  FILE *fp = fopen(logfile, "a");
  if (!fp)
    return;

  struct timeval tv;
  gettimeofday(&tv, NULL);

  time_t now = tv.tv_sec;
  struct tm *tm_info = localtime(&now);

  char timestamp[64];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

  long millis = tv.tv_usec / 1000;
  fprintf(fp, "[%s.%03ld] %s\n", timestamp, millis, message);
  fclose(fp);
}
