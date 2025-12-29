#include "nm_access_req.h"
#include "../common/proto.h"
#include <string.h>
#include <stdio.h>

#define MAX_REQUESTS 1000

static AccessRequest g_requests[MAX_REQUESTS];
static int g_request_count = 0;

void nm_access_req_init(void)
{
    g_request_count = 0;
    memset(g_requests, 0, sizeof(g_requests));
}

int nm_access_req_request(const char *file, const char *requester, const char *owner)
{
    // Check if request already exists
    for (int i = 0; i < g_request_count; i++)
    {
        if (strcmp(g_requests[i].file, file) == 0 &&
            strcmp(g_requests[i].requester, requester) == 0 &&
            g_requests[i].pending)
        {
            return ERR_ALREADY_EXISTS; // Request already pending
        }
    }

    if (g_request_count >= MAX_REQUESTS)
    {
        return ERR_INTERNAL;
    }

    AccessRequest *req = &g_requests[g_request_count++];
    strncpy(req->file, file, sizeof(req->file) - 1);
    strncpy(req->requester, requester, sizeof(req->requester) - 1);
    strncpy(req->owner, owner, sizeof(req->owner) - 1);
    req->pending = 1;

    return OK;
}

int nm_access_req_list_pending(const char *owner, char *buf, size_t buflen)
{
    buf[0] = '\0';
    int pos = 0;
    int found = 0;

    for (int i = 0; i < g_request_count; i++)
    {
        if (strcmp(g_requests[i].owner, owner) == 0 && g_requests[i].pending)
        {
            if (found > 0)
            {
                pos += snprintf(buf + pos, buflen - pos, ";;");
            }
            pos += snprintf(buf + pos, buflen - pos, "%s:%s",
                            g_requests[i].file, g_requests[i].requester);
            found++;

            if (pos >= (int)buflen - 1)
                break;
        }
    }

    return found > 0 ? OK : ERR_NOT_FOUND;
}

int nm_access_req_respond(const char *file, const char *requester, const char *owner, int approve)
{
    for (int i = 0; i < g_request_count; i++)
    {
        if (strcmp(g_requests[i].file, file) == 0 &&
            strcmp(g_requests[i].requester, requester) == 0 &&
            strcmp(g_requests[i].owner, owner) == 0 &&
            g_requests[i].pending)
        {

            g_requests[i].pending = 0;
            return approve ? OK : ERR_UNAUTHORIZED;
        }
    }

    return ERR_NOT_FOUND;
}
