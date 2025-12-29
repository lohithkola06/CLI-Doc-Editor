#ifndef LOG_H
#define LOG_H

#include <time.h>

// Log a message with timestamp, IP, port, and additional info
void log_message(const char *component, const char *operation, const char *ip, int port, const char *user, const char *details);

// Log to file
void log_to_file(const char *logfile, const char *message);

#endif
