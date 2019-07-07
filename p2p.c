#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <errno.h>

#define CONNECTION_PORT 9998
#define DISCOVERY_PORT CONNECTION_PORT
#define DISCOVERY_MAGIC "ALU_P2P_DISCOVERY"

#define EVMAN_MAX_EVENTS 5

#ifdef ENABLE_DEBUG
#define _DBG 1
#else
#define _DBG 0
#endif

#define DEBUG_PRINT(fmt, ...)                             \
    do                                                    \
    {                                                     \
        if (_DBG)                                         \
            fprintf(stderr, "%s:%d:%s(): " fmt, __FILE__, \
                    __LINE__, __func__, __VA_ARGS__);     \
    } while (0)

typedef struct event_subscription
{
    int fd;
    void (*handler)(int, void *);
    void *user_data;
    struct event_subscription *next;
} event_subscription;

typedef struct event_manager
{
    int epoll_fd;
    event_subscription *subscription_head;
} event_manager;

event_manager *new_evman()
{
    event_manager *mgr = malloc(sizeof(event_manager));
    mgr->epoll_fd = epoll_create1(0);
    mgr->subscription_head = malloc(sizeof(event_subscription));
    mgr->subscription_head->fd = -1;
    return mgr;
}

int evman_subscribe(event_manager *evman, int fd, struct epoll_event *event, void (*handler)(int, void *), void *user_data)
{
    DEBUG_PRINT("evman_subscribe fd=%d\n", fd);
    event_subscription *current = evman->subscription_head;

    while (current->next != NULL)
    {
        current = current->next;
    }
    current->next = malloc(sizeof(event_subscription));
    if (current->next == NULL)
    {

        return -1;
    }
    current->next->fd = fd;
    current->next->handler = handler;
    current->next->user_data = user_data;
    if (epoll_ctl(evman->epoll_fd, EPOLL_CTL_ADD, fd, event))
    {
        perror("failed to epoll_ctl");
        return -1;
    }
    return 0;
}

int evman_subscribe_events_flag(event_manager *evman, int fd, uint32_t events, void (*handler)(int, void *), void *user_data)
{
    struct epoll_event event;
    event.events = events;
    event.data.fd = fd;
    return evman_subscribe(evman, fd, &event, handler, user_data);
}

int evman_unsubscribe_fd(event_manager *evman, int fd)
{
    event_subscription *current = evman->subscription_head;

    while (1)
    {
        if (current->next == NULL)
        {
            return -1;
        }

        if (current->next->fd == fd)
        {
            break;
        }
        current = current->next;
    }

    event_subscription *to_delete = current->next;
    event_subscription *next = to_delete->next;
    current->next = next;
    struct epoll_event event;
    epoll_ctl(evman->epoll_fd, EPOLL_CTL_DEL, fd, &event);
    free(to_delete);
    return 0;
}

void evman_dispatch_event(event_manager *evman, struct epoll_event event)
{
    DEBUG_PRINT("evman_dispatch_event fd=%d\n", event.data.fd);
    event_subscription *current = evman->subscription_head;

    while (current != NULL)
    {
        if (current->fd == event.data.fd)
        {
            current->handler(event.data.fd, current->user_data);
            return;
        }
        current = current->next;
    }
    DEBUG_PRINT("evman_dispatch_event handler not found fd=%d", event.data.fd);
}

int evman_loop(event_manager *evman)
{
    DEBUG_PRINT("evman_loop started %d\n", 0);
    struct epoll_event events[EVMAN_MAX_EVENTS];
    while (1)
    {
        int num_events = epoll_wait(evman->epoll_fd, events, EVMAN_MAX_EVENTS, 30000);
        if (num_events < 0)
        {
            perror("failed to epoll_wait");
            return -1;
        }
        for (int i = 0; i < num_events; i++)
        {
            evman_dispatch_event(evman, events[i]);
        }
    }
    return 0;
}

void handle_peer_connection(int fd, void *user_data)
{
    struct sockaddr_in in;
    socklen_t sz = sizeof(in);

    fd = accept(fd, (struct sockaddr *)&in, &sz);
    if (fd == -1)
    {
        perror("cannot accept peer connection");
        return;
    }

    DEBUG_PRINT("connection fd %d from %s:%d\n", fd,
                inet_ntoa(in.sin_addr), (int)ntohs(in.sin_port));
}

// binds a socket to the connection_port or changes it when already in use
int listen_for_peer_connections(event_manager *evman, int *connection_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("failed to open socket for peer conenction listening");
        return -1;
    }
    struct sockaddr_in bind_addr;
    int port_attempts = 0;
    while (1)
    {

        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind_addr.sin_port = htons(*connection_port);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
        {
            if (errno == EADDRINUSE)
            {
                (*connection_port)++;
                port_attempts++;
                if (port_attempts > 20)
                {
                    perror("failed to bind socket for peer connection listening 20 times");
                    return -1;
                }
                continue;
            }
            perror("failed to bind socket for peer connection listening");
            close(fd);
            return -1;
        }
        else
        {
            break;
        }
    }
    if (listen(fd, 1) == -1)
    {
        perror("failed to listen socket for peer connection listening");
        close(fd);
        return -1;
    }
    evman_subscribe_events_flag(evman, fd, EPOLLIN, handle_peer_connection, NULL);
    return 0;
}

int broadcast_discovery(struct sockaddr *broadcast_addr, const char *msg_to_send)
{
    if (broadcast_addr == 0)
    {
        printf(" broadcast_addr == null\n");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("failed to open socket for broadcasting");
        return -1;
    }
    int broadcast_value = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast_value, sizeof(broadcast_value)) < 0)
    {
        perror("failed to set SO_BROADCAST sockopt on the broadcast socket");
        close(fd);
        return -1;
    }
    struct sockaddr_in addr = *(struct sockaddr_in *)broadcast_addr;
    addr.sin_port = htons(DISCOVERY_PORT);
    if (sendto(fd, msg_to_send, strlen(msg_to_send) + 1, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("failed to send discovery packet");
        close(fd);
        return -1;
    }
    close(fd);
#ifdef DEBUG_BROADCAST
    printf("broadcast ok \n");
#endif
    return 0;
}

int broadcast_discovery_on_all_interfaces(const char *msg_to_send)
{
#ifdef DEBUG_BROADCAST
    printf("discovery message: %s\n", msg_to_send);
#endif
    struct ifaddrs *ifap,
        *iterator;
    if (getifaddrs(&ifap) < 0)
    {
        perror("failed to list interfaces");
        return -1;
    }
    iterator = ifap;
    while (iterator != NULL)
    {
#ifdef DEBUG_BROADCAST
        printf("found interface: %s\n", iterator->ifa_name);
#endif
        if ((iterator->ifa_flags & IFF_BROADCAST) != 0 && iterator->ifa_ifu.ifu_broadaddr != NULL && iterator->ifa_ifu.ifu_broadaddr->sa_family == AF_INET)
        {
#ifdef DEBUG_BROADCAST
            printf(" interface %s has a broadcast address (IFF_BROADCAST)\n", iterator->ifa_name);
#endif
            broadcast_discovery(iterator->ifa_ifu.ifu_broadaddr, msg_to_send);
        }
        iterator = iterator->ifa_next;
    }
    freeifaddrs(ifap);
    return 0;
}

int parse_discovery_message(const char *msg)
{
    int port;

    sscanf(msg, DISCOVERY_MAGIC ":%d", &port);

    return port;
}

void handle_discovery_data(int fd, void *user_data)
{
    const int BUFFER_SIZE = 512;
    char buffer[BUFFER_SIZE];
    unsigned int len;
    struct sockaddr_in client_addr;
    recvfrom(fd, (char *)buffer, BUFFER_SIZE,
             MSG_WAITALL, (struct sockaddr *)&client_addr,
             &len);
    DEBUG_PRINT("Recieved: %s\n", buffer);
    DEBUG_PRINT("Parsed port: %d\n", parse_discovery_message(buffer));
}

int listen_for_discovery(event_manager *evman)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
    {
        perror("failed to open socket for broadcast listening");
        return -1;
    }
    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(DISCOVERY_PORT);

    if (bind(fd, (const struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        perror("failed to bind socket for broadcast listening");
        close(fd);
        return -1;
    }

    evman_subscribe_events_flag(evman, fd, EPOLLIN, handle_discovery_data, NULL);
    return 0;
}

const char *assemble_discovery_message(int connection_port)
{
    size_t result_len = strlen(DISCOVERY_MAGIC) + 10;
    char *result = malloc(result_len);
    if (result == NULL)
    {
        return NULL;
    }
    snprintf(result, result_len, "%s:%d", DISCOVERY_MAGIC, connection_port);

    return result;
}

int main(int argc, char **argv)
{
    event_manager *evman = new_evman();

    int port = CONNECTION_PORT;
    listen_for_peer_connections(evman, &port);
    DEBUG_PRINT("listening for connections on %d\n", port);
    listen_for_discovery(evman);
    const char *msg = assemble_discovery_message(port);
    broadcast_discovery_on_all_interfaces(msg);
    free((void *)msg);

    evman_loop(evman);

    return 0;
}