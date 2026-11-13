#ifndef GARDENER_DASHBOARD_H
#define GARDENER_DASHBOARD_H

#include "gardener/event.h"
#include "gardener/registry.h"
#include "gardener/packet.h"
#include <stdbool.h>

/* ---------------------------------------------------------------------
 * Dashboard Link (Phase F)
 *
 * A simple TCP server that streams JSONL to a connected dashboard.
 * Non-blocking: if no dashboard is connected, events are silently dropped
 * from the network stream (they are still written to disk/logs).
 * ------------------------------------------------------------------- */

#define DASHBOARD_MAX_CLIENTS 4

typedef struct {
    int listen_fd;
    int client_fds[DASHBOARD_MAX_CLIENTS];
    uint16_t port;
} dashboard_server_t;

/* Start the TCP server on the given port. Returns 0 on success. */
int dashboard_init(dashboard_server_t *dash, uint16_t port);

/* Poll for new connections or disconnects. Call this in the main loop. */
void dashboard_poll(dashboard_server_t *dash);

/* Send an event to all connected dashboards. */
void dashboard_send_event(dashboard_server_t *dash, const event_t *e);

/* Send the registry schema to a newly connected client. */
void dashboard_send_registry(dashboard_server_t *dash, const registry_t *reg);

/* Clean up sockets. */
void dashboard_close(dashboard_server_t *dash);

#endif /* GARDENER_DASHBOARD_H */