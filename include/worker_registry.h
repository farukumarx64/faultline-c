#ifndef FAULTLINE_WORKER_REGISTRY_H
#define FAULTLINE_WORKER_REGISTRY_H

#include <stdint.h>

#define FAULTLINE_MAX_WORKERS 64u

enum faultline_worker_state {
    FAULTLINE_WORKER_UNUSED = 0,
    FAULTLINE_WORKER_ALIVE,
    FAULTLINE_WORKER_DEAD
};

struct faultline_worker {
    uint32_t id;
    int fd;
    enum faultline_worker_state state;
    /* Registration time is the initial baseline, then valid heartbeat times. */
    int64_t last_heartbeat_ms;
};

/* Coordinator-owned, bounded storage. Fields are read-only outside its API,
 * WAL replay, and coordinator_store's durable registration publication. Replay
 * restores only the allocator into an empty live registry. */
struct faultline_worker_registry {
    struct faultline_worker workers[FAULTLINE_MAX_WORKERS];
    uint32_t next_worker_id;
};

enum faultline_registry_result {
    FAULTLINE_REGISTRY_OK = 0,
    FAULTLINE_REGISTRY_INVALID_ARGUMENT,
    FAULTLINE_REGISTRY_ALREADY_REGISTERED,
    FAULTLINE_REGISTRY_FULL,
    FAULTLINE_REGISTRY_ID_EXHAUSTED,
    FAULTLINE_REGISTRY_NOT_FOUND,
    FAULTLINE_REGISTRY_NOT_ALIVE,
    FAULTLINE_REGISTRY_CONNECTION_MISMATCH
};

/* Initialize a non-null registry once before use. No sockets are opened/closed. */
void faultline_worker_registry_init(struct faultline_worker_registry *registry);

/*
 * Assign a fresh nonzero ID, with ALIVE state and now_ms as the liveness baseline.
 * fd and monotonic now_ms must be nonnegative. Dead slots may be replaced, but
 * IDs never repeat within one initialized registry. Exhaustion fails, not wraps.
 * All pointers are required; worker_id must point to storage outside the registry.
 * On error neither the registry nor *worker_id changes.
 */
enum faultline_registry_result faultline_worker_register(
    struct faultline_worker_registry *registry, int fd, int64_t now_ms,
    uint32_t *worker_id);

/*
 * Find an ALIVE or retained DEAD record by ID; NULL if absent or registry is NULL.
 * Returned storage belongs to the registry. A later registration may replace a
 * dead slot: retain IDs, not pointers, when storing a worker identity elsewhere.
 */
const struct faultline_worker *faultline_worker_find(
    const struct faultline_worker_registry *registry, uint32_t worker_id);

/*
 * Read-only predicate: true at/after the deadline of an ALIVE record.
 * NULL, invalid times, and nonpositive timeouts return false. Does not close or
 * mark dead; the coordinator owns that transition and the socket cleanup.
 */
int faultline_worker_timed_out(const struct faultline_worker *worker,
                              int64_t now_ms, int timeout_ms);

/* Require the same ALIVE ID/connection pair. Errors leave the record unchanged. */
enum faultline_registry_result faultline_worker_heartbeat(
    struct faultline_worker_registry *registry, uint32_t worker_id, int fd,
    int64_t now_ms);

/* Mark DEAD and clear fd to -1, retaining ID and timestamp until slot reuse. */
enum faultline_registry_result faultline_worker_mark_dead(
    struct faultline_worker_registry *registry, uint32_t worker_id, int fd);

#endif
