# 03 — Namespaces, Identity, and Networking

## Table of contents

- [Namespace creation](#namespace-creation)
- [Why the child waits](#why-the-child-waits)
- [UID and GID maps](#uid-and-gid-maps)
- [Loopback](#loopback)
- [Optional veth networking](#optional-veth-networking)
- [Default-route construction](#default-route-construction)

## Namespace creation

`container_run()` calls `container_namespaces()` while building the `clone`
flags. `SIGCHLD` is added so the parent can use ordinary `waitpid` semantics.

```c
int container_namespaces(void)
{
    return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS |
           CLONE_NEWUTS | CLONE_NEWNET;
}

child = clone(child_entry, stack_top,
              container_namespaces() | SIGCHLD, c);
```

The child immediately exists in the namespaces, but the user namespace begins
with empty identity maps. Therefore, it must not continue setup yet.

## Why the child waits

A pipe has two descriptors:

```text
c->sync[0] = read end
c->sync[1] = write end
```

Child behavior:

```c
close(c->sync[1]);
read(c->sync[0], &release_byte, 1); /* blocks */
close(c->sync[0]);
```

Parent behavior:

```c
close(c->sync[0]);
/* perform maps, cgroup, host networking */
write(c->sync[1], &release_byte, 1);
close(c->sync[1]);
```

Closing unused ends is not cosmetic. If extra write ends remained open, a reader
waiting for EOF could block forever.

## UID and GID maps

The parent writes three procfs pseudo-files belonging to the cloned child:

```c
/proc/<child>/uid_map    ← "0 <host uid> 1"
/proc/<child>/setgroups  ← "deny"
/proc/<child>/gid_map    ← "0 <host gid> 1"
```

Each mapping has three columns:

```text
inside-ID   outside-ID   number-of-consecutive-IDs
0           getuid()     1
```

Only the single inside identity 0 is mapped. `setgroups` must be set to `deny`
before an unprivileged parent may write `gid_map`; Linux enforces this ordering
to prevent group-based privilege expansion.

The implementation builds `/proc/<pid>` once, uses bounded `snprintf`, and calls
the provided `write_file()` for each pseudo-file.

## Loopback

A new network namespace has `lo`, but it starts down. `container_network()` is
called by `container_setup()` before capabilities are removed.

```c
int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
struct ifreq ifr = {0};
snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "lo");
ioctl(sock, SIOCGIFFLAGS, &ifr);
ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
ioctl(sock, SIOCSIFFLAGS, &ifr);
close(sock);
```

The datagram socket is a control handle; no packet is sent. `SIOCGIFFLAGS` reads
current flags so the code preserves them, and `SIOCSIFFLAGS` writes the updated
set. The spec marks loopback best-effort, so failure is reported but does not
abort the core container.

## Optional veth networking

With `--net`, the provided parent-side function constructs:

```text
host bridge cvbr0 (10.44.0.1/24)
        │
        └── host veth ══ container veth ceth0 (10.44.0.2/24)
```

It moves `ceth0` into the child's network namespace before releasing the pipe.
Our child-side `container_net_config()` then:

1. validates prefix `0..32`;
2. applies `c->net_ip` with `SIOCSIFADDR`;
3. derives and applies the netmask with `SIOCSIFNETMASK`;
4. preserves current flags and ORs `IFF_UP | IFF_RUNNING`;
5. adds a default route through `c->net_gw`.

The prefix-to-mask calculation is:

```c
uint32_t mask = prefix == 0 ? 0 : UINT32_MAX << (32 - prefix);
struct in_addr mask_addr = { .s_addr = htonl(mask) };
```

`UINT32_MAX` is 32 one-bits. Shifting left leaves `prefix` leading ones.
`htonl` converts host integer byte order to network byte order.

## Default-route construction

The route says “for every destination not matched more specifically, send via
the gateway”:

```c
struct rtentry route = {0};
destination = (struct sockaddr_in *)&route.rt_dst;     /* 0.0.0.0 */
genmask     = (struct sockaddr_in *)&route.rt_genmask; /* 0.0.0.0 */
gateway     = (struct sockaddr_in *)&route.rt_gateway; /* 10.44.0.1 */

route.rt_flags = RTF_UP | RTF_GATEWAY;
route.rt_dev = (char *)c->net_ifname;
ioctl(sock, SIOCADDRT, &route);
```

Destination `0.0.0.0` with mask `0.0.0.0` is the default route. The official
network test deliberately connects from another subnet, so an address and an
up interface alone are insufficient; the reply requires this route.

