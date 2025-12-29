#ifndef SS_ACL_H
#define SS_ACL_H

// Access Control List management
// To be implemented by Saharsh

int ss_acl_init(void);
int ss_acl_check_read(const char *file, const char *user);
int ss_acl_check_write(const char *file, const char *user);
int ss_acl_add_permission(const char *file, const char *user, const char *mode);
int ss_acl_remove_permission(const char *file, const char *user);

#endif

