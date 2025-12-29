#ifndef NM_REPLICATION_H
#define NM_REPLICATION_H

#include <time.h>

// Storage Server tracking with replication
typedef struct
{
    char ss_id[64];
    char host[64];
    int client_port;
    int nm_port; // Port for NM-SS communication
    int alive;   // 1 if alive, 0 if dead
    time_t last_heartbeat;
    char replica_of[64]; // If this is a replica, which SS is it replicating?
} SSNode;

// File replication mapping
typedef struct
{
    char file[256];
    char primary_ss[64];
    char replica_ss[64];
} FileReplica;

// Initialize replication system
void nm_replication_init(void);

// Register a storage server
int nm_replication_register_ss(const char *ss_id, const char *host, int client_port, int nm_port);

// Update heartbeat for SS
int nm_replication_heartbeat(const char *ss_id);

// Check for failed storage servers
void nm_replication_check_failures(void);

// Map file to primary and replica SS
int nm_replication_map_file(const char *file, const char *primary_ss);
int nm_replication_rename_file(const char *old_file, const char *new_file);

// Get SS for file (with failover to replica)
int nm_replication_get_ss(const char *file, char *host_out, int *port_out, int *is_replica);

// Get any available SS
int nm_replication_get_any_ss(char *host_out, int *port_out, char *ss_id_out);

// Async write replication
int nm_replication_async_write(const char *file, const char *operation);

// SS recovery - sync data back
int nm_replication_recover_ss(const char *ss_id);

// Get replica SS for a primary SS
int nm_replication_get_replica_for_ss(const char *primary_ss, char *host_out, int *port_out);

#endif
