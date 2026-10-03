# Networking

## What “networking the container” means

A network interface is a kernel endpoint that can send and receive network
packets. A network namespace gives the container its own list of interfaces, IP
addresses, routes, and related networking state instead of automatically sharing
the host’s Wi-Fi/Ethernet interfaces.

Initially the new namespace contains only disabled loopback. Loopback (`lo`) is
internal communication: one process in the container can contact another through
`localhost` without packets leaving the namespace.

In optional `--net` mode, provided host code creates a virtual Ethernet pair.
Think of it as two ends of a virtual cable: one end connects to host bridge
`cvbr0`; the other moves into the container and is named `ceth0`. The assignment
uses it so a host process/test can reach a server inside the container. The code
does not automatically expose the container to the public Internet; it configures
the interface and a default route through the host bridge.

## Important networking data structures

### `struct ifreq`

`struct ifreq` is the traditional interface-control structure used with socket
`ioctl()` requests. Its `ifr_name` identifies an interface. A union then carries
the field relevant to a particular request, such as `ifr_flags`, `ifr_addr`, or
`ifr_netmask`.

### `struct sockaddr` and `struct sockaddr_in`

`struct sockaddr` is a generic socket-address container accepted by APIs that
support multiple address families. For IPv4, the bytes are interpreted as
`struct sockaddr_in`, whose important fields are `sin_family = AF_INET` and
`sin_addr`, the 32-bit network-order address.

### `struct rtentry`

This structure describes a routing-table entry. This implementation fills its
destination, netmask, gateway, and flags to create a default route.

## `container_network()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Switch the loopback interface `lo` on inside the new network
namespace so programs can use `localhost`.

**Used by:** `container_setup()` before capabilities are dropped.

```c
int container_network(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }
```

- `AF_INET` selects IPv4 networking.
- `SOCK_DGRAM` creates a datagram socket. No packets need to be sent; the socket
  is a kernel handle through which interface `ioctl()` operations are issued.
- `sock` is the file descriptor. A negative result is failure.

```c
    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
```

- `request` carries interface name and flags.
- Zeroing it initializes all bytes and guarantees a NUL remains at the end of
  `ifr_name`.
- `IFNAMSIZ - 1` prevents the copy from filling the entire name array.

```c
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        /* print, close socket, and return -1 */
    }
    request.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        /* print, close socket, and return -1 */
    }
```

- `SIOCGIFFLAGS` reads the existing interface flags into `ifr_flags`.
- `|=` adds `IFF_UP` and `IFF_RUNNING` while preserving all other bits.
- `SIOCSIFFLAGS` asks the kernel to apply the modified flags.
- Every error path closes the descriptor before returning.

```c
    close(sock);
    return 0;
}
```

- The control socket is no longer needed after configuration.
- Returning `0` reports success to the caller, although the caller deliberately
  treats this operation as best-effort.

## `make_ipv4_address()`

**Assignment status:** Added private helper. The specification required IPv4
address construction but did not name this function.

**Purpose:** Convert an address string such as `10.44.0.2` into the binary IPv4
socket-address layout expected by networking ioctls.

**Used by:** `container_net_config()` for destination, mask, gateway, and
interface address structures.

```c
static int make_ipv4_address(struct sockaddr *addr, const char *ip_text)
{
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
    memset(ipv4, 0, sizeof *ipv4);
    ipv4->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip_text, &ipv4->sin_addr) != 1) {
        fprintf(stderr, "container: bad IPv4 address '%s'\n", ip_text);
        return -1;
    }
    return 0;
}
```

### Variables and lines

- `addr` points to generic address storage supplied by the caller.
- `ip_text` points to the human-readable dotted-decimal address.
- The cast creates `ipv4`, a typed view of the same memory. No new structure is
  allocated.
- `sizeof *ipv4` means the size of the structure that `ipv4` points to.
- `sin_family = AF_INET` labels the result as IPv4.
- `inet_pton()` means “presentation to network.” It parses text into the binary
  network representation. Exactly `1` means success; `0` is invalid input and
  `-1` is an unsupported-family/error result.
- On success, the parsed value is stored in `sin_addr` and the function returns
  `0`.

## `container_net_config()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Configure the container end of the veth pair created by the
provided `container_net_host_setup()`.

**Used by:** `container_setup()` only when `c->net_enabled` is nonzero.

### Socket and interface request

```c
int container_net_config(struct container *c)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { /* report and fail */ }

    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);
```

- The socket again exists only as the control endpoint for ioctls.
- `request.ifr_name` identifies the provided interface, normally `ceth0`.

### Assign the IPv4 address

```c
    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        /* report */
        goto fail;
    }
```

- `&request.ifr_addr` points to the address union member.
- The helper stores the binary form of `c->net_ip` there.
- `SIOCSIFADDR` assigns it to the named interface.
- `goto fail` centralizes socket cleanup for every later failure.

### Construct and assign the netmask

```c
    struct sockaddr_in *netmask =
        (struct sockaddr_in *)&request.ifr_netmask;
    memset(netmask, 0, sizeof *netmask);
    netmask->sin_family = AF_INET;
    uint32_t mask_bits = 0;
    if (c->net_prefix > 0)
        mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);
    netmask->sin_addr.s_addr = htonl(mask_bits);
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
        /* report */
        goto fail;
    }
```

- `netmask` is a typed pointer into another union field of `request`.
- `uint32_t` guarantees exactly 32 bits, matching IPv4.
- `0xFFFFFFFFu` is 32 one-bits. Shifting left leaves `net_prefix` leading ones
  and trailing zeros. For prefix 24, the result represents `255.255.255.0`.
- Prefix zero is handled separately because shifting a 32-bit value by 32 is
  undefined in C.
- `htonl()` converts host byte order to network byte order.
- `SIOCSIFNETMASK` installs the resulting mask.

### Bring the interface up

```c
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) { /* fail */ }
    request.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) { /* fail */ }
```

This repeats the safe read-modify-write pattern used for loopback. Reading first
prevents unrelated existing flags from being cleared.

### Add the default route

```c
    struct rtentry route;
    memset(&route, 0, sizeof route);
    if (make_ipv4_address(&route.rt_dst, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_genmask, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_gateway, c->net_gw) < 0)
        goto fail;
    route.rt_flags = RTF_UP | RTF_GATEWAY;
    if (ioctl(sock, SIOCADDRT, &route) < 0) {
        /* report */
        goto fail;
    }
```

- `route` starts fully zeroed.
- Destination `0.0.0.0` with mask `0.0.0.0` matches every address, making this
  the default route.
- `rt_gateway` holds the host bridge address from `c->net_gw`.
- `RTF_UP` enables the route; `RTF_GATEWAY` says traffic goes through a gateway.
- `SIOCADDRT` adds the route to this network namespace’s routing table.

### Shared cleanup

```c
    close(sock);
    return 0;

fail:
    close(sock);
    return -1;
}
```

Both paths close the socket exactly once. A successful configuration returns
zero, while any failed parse or ioctl reaches `fail` and returns `-1`.

## Connection to provided `net.c`

`container_net_config()` cannot create the connection alone. In the parent,
provided `container_net_host_setup(c, child)` creates a bridge and veth pair,
moves one end into the child’s network namespace, and records the host-side
interface name. After the container exits, provided
`container_net_host_teardown(c)` removes that host-side connection. Those two
functions were supplied; our required work was calling them in the correct
lifecycle positions and configuring the child end.
