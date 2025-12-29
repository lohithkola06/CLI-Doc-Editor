#include "ss_acl.h"
#include <string.h>

// Placeholder implementation
// To be implemented by Saharsh

int ss_acl_init(void) {
  return 0;
}

int ss_acl_check_read(const char *file, const char *user) {
  (void)file;
  (void)user;
  // MVP: Allow all reads
  return 0;
}

int ss_acl_check_write(const char *file, const char *user) {
  (void)file;
  (void)user;
  // MVP: Allow all writes
  return 0;
}

int ss_acl_add_permission(const char *file, const char *user, const char *mode) {
  (void)file;
  (void)user;
  (void)mode;
  // TODO: Add permission to metadata
  return 0;
}

int ss_acl_remove_permission(const char *file, const char *user) {
  (void)file;
  (void)user;
  // TODO: Remove permission from metadata
  return 0;
}

