/* hc_http_probe.c — POSIX-socket connectivity probe for hc_http. See hc_http.h for the contract.
 *
 * Pure sockets, no libcurl: resolve host:port, then non-blocking connect with a timeout to the
 * first address that accepts. Independent of the easy handle so it is unit-testable against a
 * loopback listener. No interface enumeration (that is environment-dependent and untestable);
 * "no interface" is inferred from a network-unreachable connect error instead.
 */

#define _DEFAULT_SOURCE 1

#include "hc_http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_with_timeout(const struct addrinfo *ai, int timeout_ms, int *out_errno)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        *out_errno = errno;
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
        *out_errno = 0;
        close(fd);
        return 0;
    }
    if (errno != EINPROGRESS) {
        *out_errno = errno;
        close(fd);
        return -1;
    }
    struct pollfd pfd = {fd, POLLOUT, 0};
    int pr = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 2000);
    if (pr <= 0) {
        *out_errno = (pr == 0) ? ETIMEDOUT : errno;
        close(fd);
        return -1;
    }
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    close(fd);
    *out_errno = soerr;
    return soerr == 0 ? 0 : -1;
}

hc_http_net_state hc_http_net_probe(const char *host, const char *port, int timeout_ms)
{
    if (!host || !port) return HC_HTTP_NET_NO_DNS;

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return HC_HTTP_NET_NO_DNS;

    int last_errno = ECONNREFUSED;
    bool connected = false;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int e = 0;
        if (connect_with_timeout(ai, timeout_ms, &e) == 0) {
            connected = true;
            break;
        }
        last_errno = e;
    }
    freeaddrinfo(res);

    if (connected) return HC_HTTP_NET_ONLINE;
    if (last_errno == ENETUNREACH || last_errno == ENETDOWN) return HC_HTTP_NET_NO_IFACE;
    return HC_HTTP_NET_NO_ROUTE;
}

const char *hc_http_net_state_str(hc_http_net_state s)
{
    switch (s) {
    case HC_HTTP_NET_ONLINE:   return "online";
    case HC_HTTP_NET_NO_IFACE: return "network unreachable";
    case HC_HTTP_NET_NO_DNS:   return "name did not resolve";
    case HC_HTTP_NET_NO_ROUTE: return "no route / connection refused";
    }
    return "unknown";
}
