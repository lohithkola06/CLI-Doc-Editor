#ifndef NM_ACCESS_REQ_H
#define NM_ACCESS_REQ_H

#include <stddef.h>

// Access request management
typedef struct
{
    char file[256];
    char requester[64];
    char owner[64];
    int pending; // 1 if pending, 0 if resolved
} AccessRequest;

// Initialize access request system
void nm_access_req_init(void);

// User requests access to a file
int nm_access_req_request(const char *file, const char *requester, const char *owner);

// Owner views pending requests for their files
int nm_access_req_list_pending(const char *owner, char *buf, size_t buflen);

// Owner approves/denies a request
int nm_access_req_respond(const char *file, const char *requester, const char *owner, int approve);

#endif
