#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
#endif

#include "gardener/dashboard.h"
#include "gardener/platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Cross-platform socket helpers --- */
#ifdef _WIN32
  #define CLOSE_SOCKET closesocket
  #define SHUTDOWN_SOCKET SD_BOTH
  typedef int socklen_t;
#else
  #define CLOSE_SOCKET close
  #define SHUTDOWN_SOCKET SHUT_RDWR
  #define INVALID_SOCKET -1
#endif

static void set_nonblocking(int fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void send_str(int fd, const char *str) {
    if (fd < 0) return;
    size_t len = strlen(str);
#ifdef _WIN32
    send(fd, str, (int)len, 0);
#else
    send(fd, str, len, MSG_NOSIGNAL);
#endif
}

/* --- Implementation --- */

int dashboard_init(dashboard_server_t *dash, uint16_t port) {
    if (!dash) return -1;
    memset(dash, 0, sizeof(*dash));
    dash->port = port;
    dash->listen_fd = -1;
    for (int i = 0; i < DASHBOARD_MAX_CLIENTS; i++) {
        dash->client_fds[i] = -1;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return -1;
#endif

    dash->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (dash->listen_fd == INVALID_SOCKET) return -1;

    int opt = 1;
    setsockopt(dash->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(dash->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CLOSE_SOCKET(dash->listen_fd);
        return -1;
    }

    if (listen(dash->listen_fd, 5) < 0) {
        CLOSE_SOCKET(dash->listen_fd);
        return -1;
    }

    set_nonblocking(dash->listen_fd);
    plat_log(LOG_INFO, "dashboard: listening on port %u", port);
    return 0;
}

void dashboard_poll(dashboard_server_t *dash) {
    if (dash->listen_fd < 0) return;

    /* 1. Accept new clients */
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int new_fd = accept(dash->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    
    if (new_fd != INVALID_SOCKET) {
        set_nonblocking(new_fd);
        for (int i = 0; i < DASHBOARD_MAX_CLIENTS; i++) {
            if (dash->client_fds[i] < 0) {
                dash->client_fds[i] = new_fd;
                plat_log(LOG_INFO, "dashboard: client connected");
                send_str(new_fd, "{\"type\":\"hello\",\"version\":1}\n");
                break;
            }
        }
    }

    /* 2. Check for disconnected clients */
    for (int i = 0; i < DASHBOARD_MAX_CLIENTS; i++) {
        int fd = dash->client_fds[i];
        if (fd < 0) continue;

        char buf[1];
        /* If recv returns 0, client disconnected gracefully. If < 0, socket error. */
        int n = recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) {
            plat_log(LOG_INFO, "dashboard: client disconnected");
            CLOSE_SOCKET(fd);
            dash->client_fds[i] = -1;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            CLOSE_SOCKET(fd);
            dash->client_fds[i] = -1;
        }
    }
}

void dashboard_send_registry(dashboard_server_t *dash, const registry_t *reg) {
    if (!reg) return;
    
    char line[4096];
    int offset = snprintf(line, sizeof(line), "{\"type\":\"registry\",\"channels\":[");
    
    for (size_t i = 0; i < reg->count; i++) {
        const channel_desc_t *c = &reg->entries[i];
        offset += snprintf(line + offset, sizeof(line) - offset,
            "%s{\"id\":%u,\"name\":\"%s\",\"unit\":\"%s\",\"type\":\"%s\",\"role\":\"%s\"}",
            i > 0 ? "," : "",
            (unsigned)c->id,
            c->name ? c->name : "",
            c->unit ? c->unit : "",
            ch_type_name(c->type),
            ch_role_name(c->role));
    }
    snprintf(line + offset, sizeof(line) - offset, "]}\n");

    for (int i = 0; i < DASHBOARD_MAX_CLIENTS; i++) {
        if (dash->client_fds[i] >= 0) {
            send_str(dash->client_fds[i], line);
        }
    }
}

void dashboard_send_event(dashboard_server_t *dash, const event_t *e) {
    char msg[64];
    snprintf(msg, sizeof(msg), "detector %s fired", detector_name((detector_t)e->detector));
    
    char json[512];
    int n = event_to_json(e, msg, json, sizeof(json));
    if (n > 0 && n < (int)sizeof(json)) {
        char line[520];
        snprintf(line, sizeof(line), "%s\n", json);
        for (int i = 0; i < DASHBOARD_MAX_CLIENTS; i++) {
            if (dash->client_fds[i] >= 0) {
                send_str(dash->client_fds[i], line);
            }
        }
    }
}

void dashboard_close(dashboard_server_t *dash) {
    for (int i = 0; i < DASHBOARD_MAX_CLIENTS; i++) {
        if (dash->client_fds[i] >= 0) {
            CLOSE_SOCKET(dash->client_fds[i]);
        }
    }
    if (dash->listen_fd >= 0) {
        CLOSE_SOCKET(dash->listen_fd);
    }
#ifdef _WIN32
    WSACleanup();
#endif
}