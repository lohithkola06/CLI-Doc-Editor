#include "nm_replication.h"
#include "../common/proto.h"
#include "../common/net.h"
#include "../common/log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_SS 32
#define MAX_FILE_REPLICAS 10000
#define HEARTBEAT_TIMEOUT 15       // seconds
#define HEARTBEAT_CHECK_INTERVAL 5 // seconds

static SSNode g_ss_nodes[MAX_SS];
static int g_ss_count = 0;
static FileReplica g_file_replicas[MAX_FILE_REPLICAS];
static int g_file_replica_count = 0;
static pthread_mutex_t g_replication_mutex = PTHREAD_MUTEX_INITIALIZER;

void nm_replication_init(void)
{
    pthread_mutex_lock(&g_replication_mutex);
    g_ss_count = 0;
    g_file_replica_count = 0;
    memset(g_ss_nodes, 0, sizeof(g_ss_nodes));
    memset(g_file_replicas, 0, sizeof(g_file_replicas));
    pthread_mutex_unlock(&g_replication_mutex);

    log_message("NM", "REPLICATION_INIT", "127.0.0.1", 5050, "system", "Replication system initialized");
}

int nm_replication_register_ss(const char *ss_id, const char *host, int client_port, int nm_port)
{
    pthread_mutex_lock(&g_replication_mutex);

    // Check if SS already exists (recovery case)
    for (int i = 0; i < g_ss_count; i++)
    {
        if (strcmp(g_ss_nodes[i].ss_id, ss_id) == 0)
        {
            // SS is recovering
            g_ss_nodes[i].alive = 1;
            g_ss_nodes[i].last_heartbeat = time(NULL);
            strncpy(g_ss_nodes[i].host, host, sizeof(g_ss_nodes[i].host) - 1);
            g_ss_nodes[i].client_port = client_port;
            g_ss_nodes[i].nm_port = nm_port;
            pthread_mutex_unlock(&g_replication_mutex);

            log_message("NM", "SS_RECOVERY", host, nm_port, ss_id, "Storage Server recovered and reconnected");
            return OK;
        }
    }

    if (g_ss_count >= MAX_SS)
    {
        pthread_mutex_unlock(&g_replication_mutex);
        return ERR_INTERNAL;
    }

    SSNode *node = &g_ss_nodes[g_ss_count++];
    strncpy(node->ss_id, ss_id, sizeof(node->ss_id) - 1);
    strncpy(node->host, host, sizeof(node->host) - 1);
    node->client_port = client_port;
    node->nm_port = nm_port;
    node->alive = 1;
    node->last_heartbeat = time(NULL);
    node->replica_of[0] = '\0';

    // Assign replication pairs (simple strategy: pair consecutive SSes)
    if (g_ss_count >= 2)
    {
        // Make this SS a replica of the previous one
        SSNode *prev = &g_ss_nodes[g_ss_count - 2];
        if (prev->replica_of[0] == '\0')
        {
            strncpy(node->replica_of, prev->ss_id, sizeof(node->replica_of) - 1);
            char details[256];
            snprintf(details, sizeof(details), "SS %s is now replicating SS %s", node->ss_id, prev->ss_id);
            log_message("NM", "REPLICATION_PAIR", host, client_port, node->ss_id, details);
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);
    char details[256];
    snprintf(details, sizeof(details), "Storage Server registered (NM port: %d)", nm_port);
    log_message("NM", "SS_REGISTER", host, client_port, ss_id, details);
    return OK;
}

int nm_replication_heartbeat(const char *ss_id)
{
    pthread_mutex_lock(&g_replication_mutex);

    for (int i = 0; i < g_ss_count; i++)
    {
        if (strcmp(g_ss_nodes[i].ss_id, ss_id) == 0)
        {
            g_ss_nodes[i].last_heartbeat = time(NULL);
            if (!g_ss_nodes[i].alive)
            {
                g_ss_nodes[i].alive = 1;
                log_message("NM", "SS_BACK_ONLINE", g_ss_nodes[i].host, g_ss_nodes[i].client_port, ss_id, "SS is back online");
            }
            pthread_mutex_unlock(&g_replication_mutex);
            return OK;
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);
    return ERR_NOT_FOUND;
}

void nm_replication_check_failures(void)
{
    pthread_mutex_lock(&g_replication_mutex);

    time_t now = time(NULL);
    for (int i = 0; i < g_ss_count; i++)
    {
        if (g_ss_nodes[i].alive)
        {
            if (now - g_ss_nodes[i].last_heartbeat > HEARTBEAT_TIMEOUT)
            {
                g_ss_nodes[i].alive = 0;
                char details[256];
                snprintf(details, sizeof(details), "FAILURE DETECTED: SS is unresponsive (last heartbeat: %ld seconds ago)",
                         (long)(now - g_ss_nodes[i].last_heartbeat));
                log_message("NM", "SS_FAILURE", g_ss_nodes[i].host, g_ss_nodes[i].client_port, g_ss_nodes[i].ss_id, details);
            }
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);
}

int nm_replication_map_file(const char *file, const char *primary_ss)
{
    pthread_mutex_lock(&g_replication_mutex);

    // Find replica SS for this primary
    char replica_ss[64] = "";
    for (int i = 0; i < g_ss_count; i++)
    {
        if (g_ss_nodes[i].alive &&
            strcmp(g_ss_nodes[i].replica_of, primary_ss) == 0)
        {
            strncpy(replica_ss, g_ss_nodes[i].ss_id, sizeof(replica_ss) - 1);
            break;
        }
    }

    // Check if file already mapped
    for (int i = 0; i < g_file_replica_count; i++)
    {
        if (strcmp(g_file_replicas[i].file, file) == 0)
        {
            // Update mapping
            strncpy(g_file_replicas[i].primary_ss, primary_ss,
                    sizeof(g_file_replicas[i].primary_ss) - 1);
            strncpy(g_file_replicas[i].replica_ss, replica_ss,
                    sizeof(g_file_replicas[i].replica_ss) - 1);
            pthread_mutex_unlock(&g_replication_mutex);
            return OK;
        }
    }

    if (g_file_replica_count >= MAX_FILE_REPLICAS)
    {
        pthread_mutex_unlock(&g_replication_mutex);
        return ERR_INTERNAL;
    }

    FileReplica *fr = &g_file_replicas[g_file_replica_count++];
    strncpy(fr->file, file, sizeof(fr->file) - 1);
    strncpy(fr->primary_ss, primary_ss, sizeof(fr->primary_ss) - 1);
    strncpy(fr->replica_ss, replica_ss, sizeof(fr->replica_ss) - 1);

    pthread_mutex_unlock(&g_replication_mutex);
    char details[512];
    snprintf(details, sizeof(details), "File %s mapped to primary=%s, replica=%s",
             file, primary_ss, replica_ss[0] ? replica_ss : "none");
    log_message("NM", "FILE_REPLICATION_MAP", "127.0.0.1", 5050, "system", details);
    return OK;
}

int nm_replication_rename_file(const char *old_file, const char *new_file)
{
    pthread_mutex_lock(&g_replication_mutex);
    for (int i = 0; i < g_file_replica_count; i++)
    {
        if (strcmp(g_file_replicas[i].file, old_file) == 0)
        {
            strncpy(g_file_replicas[i].file, new_file, sizeof(g_file_replicas[i].file) - 1);
            g_file_replicas[i].file[sizeof(g_file_replicas[i].file) - 1] = '\0';
            pthread_mutex_unlock(&g_replication_mutex);
            return OK;
        }
    }
    pthread_mutex_unlock(&g_replication_mutex);
    return ERR_NOT_FOUND;
}

int nm_replication_get_ss(const char *file, char *host_out, int *port_out, int *is_replica)
{
    pthread_mutex_lock(&g_replication_mutex);

    *is_replica = 0;

    // Find file mapping
    for (int i = 0; i < g_file_replica_count; i++)
    {
        if (strcmp(g_file_replicas[i].file, file) == 0)
        {
            const char *primary_ss = g_file_replicas[i].primary_ss;
            const char *replica_ss = g_file_replicas[i].replica_ss;

            // Try primary first
            for (int j = 0; j < g_ss_count; j++)
            {
                if (strcmp(g_ss_nodes[j].ss_id, primary_ss) == 0 && g_ss_nodes[j].alive)
                {
                    strncpy(host_out, g_ss_nodes[j].host, 63);
                    *port_out = g_ss_nodes[j].client_port;
                    pthread_mutex_unlock(&g_replication_mutex);
                    return OK;
                }
            }

            // Primary failed, try replica
            if (replica_ss[0] != '\0')
            {
                for (int j = 0; j < g_ss_count; j++)
                {
                    if (strcmp(g_ss_nodes[j].ss_id, replica_ss) == 0 && g_ss_nodes[j].alive)
                    {
                        strncpy(host_out, g_ss_nodes[j].host, 63);
                        *port_out = g_ss_nodes[j].client_port;
                        *is_replica = 1;
                        pthread_mutex_unlock(&g_replication_mutex);
                        char details[512];
                        snprintf(details, sizeof(details), "FAILOVER: Using replica SS for file %s (primary %s is down)", file, primary_ss);
                        log_message("NM", "FAILOVER", g_ss_nodes[j].host, g_ss_nodes[j].client_port, replica_ss, details);
                        return OK;
                    }
                }
            }

            pthread_mutex_unlock(&g_replication_mutex);
            return ERR_NOT_FOUND; // Both primary and replica are down
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);
    return ERR_NOT_FOUND;
}

int nm_replication_get_any_ss(char *host_out, int *port_out, char *ss_id_out)
{
    pthread_mutex_lock(&g_replication_mutex);

    for (int i = 0; i < g_ss_count; i++)
    {
        if (g_ss_nodes[i].alive)
        {
            strncpy(host_out, g_ss_nodes[i].host, 63);
            *port_out = g_ss_nodes[i].client_port;
            if (ss_id_out)
            {
                strncpy(ss_id_out, g_ss_nodes[i].ss_id, 63);
            }
            pthread_mutex_unlock(&g_replication_mutex);
            return OK;
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);
    return ERR_NOT_FOUND;
}

// Async replication helper thread
static void *async_replicate_thread(void *arg)
{
    char *cmd = (char *)arg;
    // Parse command and send to replica
    // Format: "ss_id:operation"

    char ss_id[64], host[64];
    int port = 0;

    char *colon = strchr(cmd, ':');
    if (!colon)
    {
        free(cmd);
        return NULL;
    }

    *colon = '\0';
    strncpy(ss_id, cmd, sizeof(ss_id) - 1);
    const char *operation = colon + 1;

    // Find replica SS
    pthread_mutex_lock(&g_replication_mutex);
    for (int i = 0; i < g_ss_count; i++)
    {
        if (strcmp(g_ss_nodes[i].replica_of, ss_id) == 0 && g_ss_nodes[i].alive)
        {
            strncpy(host, g_ss_nodes[i].host, sizeof(host) - 1);
            port = g_ss_nodes[i].client_port;
            break;
        }
    }
    pthread_mutex_unlock(&g_replication_mutex);

    if (port > 0)
    {
        int fd = tcp_connect(host, port);
        if (fd >= 0)
        {
            send_line(fd, operation);
            close(fd);
            log_message("NM", "ASYNC_REPLICATION", host, port, ss_id, "Async replication completed");
        }
    }

    free(cmd);
    return NULL;
}

int nm_replication_async_write(const char *file, const char *operation)
{
    pthread_mutex_lock(&g_replication_mutex);

    // Find primary SS for this file
    char primary_ss[64] = "";
    for (int i = 0; i < g_file_replica_count; i++)
    {
        if (strcmp(g_file_replicas[i].file, file) == 0)
        {
            strncpy(primary_ss, g_file_replicas[i].primary_ss, sizeof(primary_ss) - 1);
            break;
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);

    if (primary_ss[0] == '\0')
    {
        return ERR_NOT_FOUND;
    }

    // Create async thread for replication
    char *cmd = malloc(1024);
    snprintf(cmd, 1024, "%s:%s", primary_ss, operation);

    pthread_t thread;
    pthread_create(&thread, NULL, async_replicate_thread, cmd);
    pthread_detach(thread);

    return OK;
}

int nm_replication_get_replica_for_ss(const char *primary_ss, char *host_out, int *port_out)
{
    pthread_mutex_lock(&g_replication_mutex);

    for (int i = 0; i < g_ss_count; i++)
    {
        if (strcmp(g_ss_nodes[i].replica_of, primary_ss) == 0 && g_ss_nodes[i].alive)
        {
            strncpy(host_out, g_ss_nodes[i].host, 63);
            *port_out = g_ss_nodes[i].client_port;
            pthread_mutex_unlock(&g_replication_mutex);
            return OK;
        }
    }

    pthread_mutex_unlock(&g_replication_mutex);
    return ERR_NOT_FOUND;
}

// Background heartbeat checker thread
void *nm_replication_heartbeat_checker(void *arg)
{
    (void)arg;

    while (1)
    {
        sleep(HEARTBEAT_CHECK_INTERVAL);
        nm_replication_check_failures();
    }

    return NULL;
}
