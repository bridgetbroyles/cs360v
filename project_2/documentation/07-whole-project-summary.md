# Container Runtime: 11 Main Functions and Private Helpers
## How to read this file
This guide follows the actual lifecycle in `container.c`. The **11 main functions** are the required functions that form the runtime's architecture. The private helpers are explained where they support those functions. The central story is:
``` text
main.c
  -> container_run()
       -> prepare cgroup
       -> create pipe
       -> clone child with namespaces
       -> parent writes UID/GID maps
       -> parent enters child into cgroup
       -> optional host network setup
       -> release child
            -> container_init()
                 -> container_setup()
                 -> fork/exec command
                 -> reap children
       -> wait
       -> network teardown
       -> cgroup cleanup
       -> return command status
```
The source states that `main.c` calls `container_run()`, while the TODOs/specification define the required order. fileciteturn3file0L1-L16 The three major mechanisms are:
``` text
Namespaces = isolate what the process sees.
Cgroups    = limit how much resources the process may consume.
Security   = remove capabilities and restrict system calls.
```
------------------------------------------------------------------------
# 1. `container_namespaces()`
## Purpose and caller
`container_namespaces()` builds the bitmask passed to `clone()`. It **does not create namespaces itself**. `container_run()` calls it when it calls `clone()`.
``` c
int container_namespaces(void)
{
    return CLONE_NEWUSER
         | CLONE_NEWPID
         | CLONE_NEWNS
         | CLONE_NEWUTS
         | CLONE_NEWNET;
}
```
The source defines these five namespace types in Part I. fileciteturn3file0L46-L55
## Line by line
``` c
int container_namespaces(void)
```
Returns an `int` and takes no arguments. The integer is being used as a **bitmask**, not an ordinary number.
``` c
return CLONE_NEWUSER
```
Requests a new user namespace. This gives the child its own UID/GID interpretation. Later, `container_write_idmaps()` maps namespace IDs to host IDs.
``` c
| CLONE_NEWPID
```
Adds a PID namespace. Processes receive a separate process-ID view. The first process in that namespace becomes PID 1 from inside the namespace, while the host still has its own PID for it.
``` c
| CLONE_NEWNS
```
Adds a mount namespace. This gives the child a separate mount table, which is necessary before constructing the container's filesystem.
``` c
| CLONE_NEWUTS
```
Adds a UTS namespace. This isolates values such as the hostname, allowing `container_setup()` to call `sethostname()` for the container.
``` c
| CLONE_NEWNET
```
Adds a network namespace. The container gets its own network interfaces and routes, which is why the networking functions can configure `lo` and the veth interface.
## Why `|` does not overwrite flags
The constants are bit flags. If, for illustration:
``` text
A = 00001
B = 00010
C = 00100
```
then:
``` text
A | B = 00011
00011 | C = 00111
```
Previously set bits remain set. Thus the return value means "request all five namespaces." `SIGCHLD`, added by `container_run()`, is not a namespace. It is a child-process signal option.
## 3 likely questions
**Q1. Who calls this?** `container_run()` calls it while constructing
the flags passed to `clone()`.
**Q2. Does it create namespaces?** No. It only constructs the flag
bitmask. `clone()` performs the creation.
**Q3. Why use `|`?** Because each namespace is an independent bit flag.
OR combines them into one value without clearing previous flags. ------------------------------------------------------------------------
# 2. `container_write_idmaps()`
## Purpose and caller
This function configures the UID/GID mapping of the new user namespace. It runs in the **parent**, after `clone()` returns the child's host PID and before the child is released.
``` c
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;
    char path[64];
    char mapping[64];

    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
    if (write_file(path, mapping) < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    return 0;
}
```
The source describes it as mapping root inside the container to the runtime user's UID/GID outside. fileciteturn3file0L58-L82
## Parameters and variables
`c` is present because the required function signature includes it, but this implementation does not use it. `child` is the host-visible PID returned by `clone()`. `path` is reused for `/proc/<child>/uid_map`, `/proc/<child>/setgroups`, and `/proc/<child>/gid_map`. `mapping` is reused for the textual mapping rule.
## Line by line
``` c
(void)c;
```
Explicitly marks `c` as intentionally unused, preventing an unused-parameter warning.
``` c
char path[64];
char mapping[64];
```
Allocate string buffers on the stack.
``` c
snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
```
Constructs a kernel-interface path such as `/proc/4312/uid_map`. `snprintf()` only creates the string; it has not changed the mapping yet.
``` c
snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
```
`getuid()` obtains the real UID of the runtime user on the host. If it is 1000, the string becomes:
``` text
0 1000 1
```
The fields mean:
``` text
0       1000       1
inside  host       count
```
So namespace UID 0 maps to host UID 1000 for one ID.
``` c
if (write_file(path, mapping) < 0)
    return -1;
```
`write_file()` performs the actual kernel-interface write. A negative result aborts setup.
``` c
snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
```
Reuses `path` for the `setgroups` control file.
``` c
if (write_file(path, "deny") < 0)
    return -1;
```
Writes `deny`. In this setup, this must happen before `gid_map` is written.
``` c
snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
```
Builds the GID-map path.
``` c
snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
```
`getgid()` obtains the runtime user's host GID and constructs the equivalent mapping.
``` c
if (write_file(path, mapping) < 0)
    return -1;
```
Actually writes the GID mapping.
``` c
return 0;
```
All three writes succeeded.
## Why the parent does it
The child is blocked on the synchronization pipe. The parent knows the child's host PID and can therefore address `/proc/<child>/...` while the child waits. The mapping does **not** make the process host root. It makes namespace UID 0 correspond to the ordinary host UID. This is why namespace root and host root are different concepts.
## 3 likely questions
**Q1. Where does `child` come from?** `clone()` returns the child's PID
to the parent, and `container_run()` passes it here.
**Q2. What does `0 1000 1` mean?** Namespace ID 0 maps to host ID 1000,
with one consecutive ID mapped.
**Q3. Why write `setgroups` before `gid_map`?** The kernel requires
`setgroups` to be denied before accepting the GID mapping in this unprivileged setup. ------------------------------------------------------------------------
# 3. `container_cgroup_init()`
## Purpose and caller
This parent-side function creates/configures the container cgroup before the child is released. It enables controllers, creates the cgroup, and writes the process, memory, and swap limits. fileciteturn3file0L102-L130
``` c
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s", c->cgroup_base, c->name);
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
        return -1;

    return 0;
}
```
## Line by line
``` c
char path[PATH_MAX];
```
Temporary buffer for the parent cgroup control-file path.
``` c
snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
```
Builds the parent's `cgroup.subtree_control` path.
``` c
if (write_file(path, "+pids +memory") < 0)
    return -1;
```
This is the important controller-enabling operation. `cgroup.subtree_control` says which controllers are enabled for **child cgroups**. `+` means enable/add. It does not mean priority. Thus:
``` text
+pids   -> pids controller available to child cgroups
+memory -> memory controller available to child cgroups
```
This is different from setting a limit. The actual limits are written later to `pids.max` and `memory.max`.
``` c
snprintf(c->cg_path, sizeof c->cg_path,
         "%s/%s", c->cgroup_base, c->name);
```
Constructs and permanently stores this container's cgroup path in the shared container structure. Later functions use `c->cg_path`.
``` c
if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
```
Attempts to create the cgroup directory. If `mkdir()` fails, the code checks `errno`. `EEXIST` means the directory already exists. The implementation deliberately accepts that case, then rewrites the limits below.
``` c
fprintf(stderr, ...);
```
Only reports the error to the human. It does not change kernel state.
``` c
return -1;
```
Tells the caller initialization failed.
``` c
write_cgroup_limit(c, "pids.max", c->pids_max)
```
Writes the configured process-count limit.
``` c
write_cgroup_limit(c, "memory.max", c->mem_max)
```
Writes the configured memory limit.
``` c
write_cgroup_limit(c, "memory.swap.max", 0)
```
Writes zero swap allowance.
``` c
return 0;
```
All cgroup setup succeeded.
## Important distinction
``` text
cgroup.subtree_control
    -> enables controllers for child cgroups

pids.max
    -> sets process limit

memory.max
    -> sets memory limit

memory.swap.max
    -> sets swap limit
```
## 3 likely questions
**Q1. Why is `+pids +memory` written first?** Because the controllers
must be enabled for child cgroups before their limit interfaces can be used.
**Q2. Does `fprintf()` create or configure the cgroup?** No. It only
prints diagnostics. `mkdir()` and `write_file()` perform the actual operations.
**Q3. Why is `EEXIST` accepted?** A previous crashed run may have left
the cgroup directory. The implementation reuses it and rewrites its limits. ------------------------------------------------------------------------
# 4. `container_cgroup_enter()`
## Purpose and caller
This parent-side function moves the cloned container-init process into the cgroup configured above. It is called before the child is released. fileciteturn3file0L133-L142
``` c
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX + 32];
    char pid_text[16];

    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);
    return write_file(path, pid_text);
}
```
## Line by line
``` c
char path[PATH_MAX + 32];
```
Buffer for the cgroup's `cgroup.procs` path.
``` c
char pid_text[16];
```
Buffer for the PID as text.
``` c
snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
```
Constructs a path such as:
``` text
/sys/fs/cgroup/mycontainer/cgroup.procs
```
``` c
snprintf(pid_text, sizeof pid_text, "%d", (int)child);
```
Converts the child's PID into decimal text.
``` c
return write_file(path, pid_text);
```
This is the actual cgroup-membership operation. Writing the PID to `cgroup.procs` tells the kernel to move the process into the cgroup. The return value is forwarded directly:
``` text
0  -> success
-1 -> failure
```
Once container init is in the cgroup, processes it creates inherit that membership.
## 3 likely questions
**Q1. What actually moves the process?** `write_file(path, pid_text)`.
`snprintf()` only prepares strings.
**Q2. Where does `child` come from?** It is the PID returned by
`clone()`.
**Q3. Why do this before releasing the child?** So the command cannot
begin running before resource limits apply. ------------------------------------------------------------------------
# Private helper: `write_cgroup_limit()`
``` c
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
{
    char path[PATH_MAX + 32];
    char value[32];

    snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);
    if (limit < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", limit);
    return write_file(path, value);
}
```
`static` makes this helper private to this source file.
``` c
snprintf(path, ..., "%s/%s", ...)
```
combines the cgroup path and control filename.
``` c
if (limit < 0)
```
interprets a negative project value as "unlimited."
``` c
snprintf(value, ..., "max");
```
creates the cgroup text representation for unlimited. Otherwise:
``` c
snprintf(value, ..., "%ld", limit);
```
converts the `long` to decimal text. Finally:
``` c
write_file(path, value)
```
performs the actual write.
### 3 likely questions
**Q1. Why is it `static`?** Only this source file needs the helper.
**Q2. What does a negative limit mean here?** The helper represents it
by writing `max`.
**Q3. Which function uses it?** `container_cgroup_init()` uses it for
`pids.max`, `memory.max`, and `memory.swap.max`. ------------------------------------------------------------------------
# 5. `container_setup()`
## Purpose and caller
`container_init()` calls this **inside the container**, after the parent releases the child and before the requested command starts. The required order is important: operations needing privileges happen before capabilities/seccomp are removed. fileciteturn3file0L273-L300
``` c
int container_setup(struct container *c)
{
    if (sethostname(c->hostname, strlen(c->hostname)) < 0) {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    (void)container_network();

    if (c->net_enabled)
        (void)container_net_config(c);

    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    if (drop_capabilities() < 0)
        return -1;

    if (container_seccomp() < 0)
        return -1;

    return 0;
}
```
## Line by line
``` c
sethostname(c->hostname, strlen(c->hostname))
```
Uses the configured hostname. `strlen()` supplies its length. Because the child has a UTS namespace, the hostname change is namespace-scoped.
``` c
if (...) {
    fprintf(...);
    return -1;
}
```
Hostname setup is required. Failure aborts setup.
``` c
(void)container_network();
```
Attempts to bring up loopback. `(void)` intentionally discards the return value because this networking step is best-effort according to the source.
``` c
if (c->net_enabled)
    (void)container_net_config(c);
```
If optional veth networking was requested, configure the container-side interface. This is also best-effort.
``` c
if (setup_filesystem(c->rootfs) < 0)
    return -1;
```
Builds the container filesystem and pivots into it. This is required, so failure stops setup.
``` c
if (drop_capabilities() < 0)
    return -1;
```
Removes Linux capabilities after privileged setup is finished.
``` c
if (container_seccomp() < 0)
    return -1;
```
Installs the syscall filter. This must be last because it blocks filesystem syscalls needed by `setup_filesystem()`.
``` c
return 0;
```
The container environment is ready for the command.
## 3 likely questions
**Q1. Who calls this?** `container_init()` calls it after the parent
releases the child.
**Q2. Why are capabilities dropped after filesystem/network
setup?** Those setup operations need privileges. The code uses them first and removes them before the command runs.
**Q3. Why is seccomp last?** The filter blocks `mount()`,
`pivot_root()`, and other setup calls. ------------------------------------------------------------------------
# 6. `container_network()`
## Purpose and caller
A new network namespace starts with loopback down. This function brings `lo` up. It uses a socket as a handle for network `ioctl()` operations. fileciteturn3file0L303-L333
``` c
int container_network(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read lo flags: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring lo up: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}
```
## Line by line
``` c
socket(AF_INET, SOCK_DGRAM, 0)
```
Creates an IPv4 datagram socket. The socket is mainly a file descriptor used by the following ioctls.
``` c
if (sock < 0)
```
Checks for failure.
``` c
struct ifreq request;
```
Creates the interface-configuration structure.
``` c
memset(&request, 0, sizeof request);
```
Clears it so no uninitialized fields are used.
``` c
strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
```
Selects the loopback interface named `lo`.
``` c
ioctl(sock, SIOCGIFFLAGS, &request)
```
Reads the interface's current flags.
``` c
request.ifr_flags |= IFF_UP | IFF_RUNNING;
```
Uses OR to preserve existing flags while adding the desired "up/running" flags.
``` c
ioctl(sock, SIOCSIFFLAGS, &request)
```
Writes the modified flags back to the kernel.
``` c
close(sock);
```
The configuration handle is no longer needed.
## 3 likely questions
**Q1. Why use a socket if no packets are sent?** The socket provides the
descriptor required by the interface-configuration ioctls.
**Q2. Why read the flags first?** So the code can add bits without
accidentally clearing existing flags.
**Q3. Who calls this?** `container_setup()` calls it inside the
container's network namespace. ------------------------------------------------------------------------
# 7. `container_net_config()`
## Purpose and caller
When optional `--net` networking is enabled, this configures the container's end of the veth pair: IP address, netmask, interface state, and default route. The host-side helper in `net.c` has already moved the container end into the network namespace. fileciteturn3file0L351-L421
``` c
int container_net_config(struct container *c)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);

    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        fprintf(stderr, "container: set %s address: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    struct sockaddr_in *netmask =
        (struct sockaddr_in *)&request.ifr_netmask;
    memset(netmask, 0, sizeof *netmask);
    netmask->sin_family = AF_INET;

    uint32_t mask_bits = 0;
    if (c->net_prefix > 0)
        mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);

    netmask->sin_addr.s_addr = htonl(mask_bits);

    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
        fprintf(stderr, "container: set %s netmask: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read %s flags: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring %s up: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    struct rtentry route;
    memset(&route, 0, sizeof route);

    if (make_ipv4_address(&route.rt_dst, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_genmask, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_gateway, c->net_gw) < 0)
        goto fail;

    route.rt_flags = RTF_UP | RTF_GATEWAY;

    if (ioctl(sock, SIOCADDRT, &route) < 0) {
        fprintf(stderr, "container: add default route via %s: %s\n",
                c->net_gw, strerror(errno));
        goto fail;
    }

    close(sock);
    return 0;

fail:
    close(sock);
    return -1;
}
```
## Line/block walkthrough
The initial `socket()` and `if (sock < 0)` have the same purpose as in `container_network()`.
``` c
struct ifreq request;
memset(&request, 0, sizeof request);
strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);
```
Creates, clears, and names the interface request. The name comes from `c->net_ifname`.
``` c
make_ipv4_address(&request.ifr_addr, c->net_ip)
```
converts the configured IP text into binary IPv4 form.
``` c
ioctl(sock, SIOCSIFADDR, &request)
```
sets that address on the interface.
### Netmask
``` c
struct sockaddr_in *netmask =
    (struct sockaddr_in *)&request.ifr_netmask;
```
Treats the generic address storage as an IPv4 `sockaddr_in`.
``` c
memset(netmask, 0, sizeof *netmask);
netmask->sin_family = AF_INET;
```
clears it and marks it IPv4.
``` c
uint32_t mask_bits = 0;
```
Starts the mask at zero.
``` c
if (c->net_prefix > 0)
    mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);
```
Builds the subnet mask from the prefix length. The separate `> 0` check avoids shifting a 32-bit value by 32, which is undefined. For `/24`, this produces 24 one-bits followed by 8 zero-bits.
``` c
netmask->sin_addr.s_addr = htonl(mask_bits);
```
Converts the mask to network byte order.
``` c
ioctl(sock, SIOCSIFNETMASK, &request)
```
applies the netmask.
### Bringing the interface up
``` c
ioctl(sock, SIOCGIFFLAGS, &request)
```
reads existing flags.
``` c
request.ifr_flags |= IFF_UP | IFF_RUNNING;
```
adds the desired bits without clearing existing ones.
``` c
ioctl(sock, SIOCSIFFLAGS, &request)
```
writes them back.
### Default route
``` c
struct rtentry route;
memset(&route, 0, sizeof route);
```
creates and clears a route structure.
``` c
make_ipv4_address(&route.rt_dst, "0.0.0.0")
make_ipv4_address(&route.rt_genmask, "0.0.0.0")
make_ipv4_address(&route.rt_gateway, c->net_gw)
```
sets destination `0.0.0.0`, mask `0.0.0.0`, and the configured gateway. Together, destination/mask represent:
``` text
0.0.0.0/0
```
the default route.
``` c
route.rt_flags = RTF_UP | RTF_GATEWAY;
```
marks it as an active gateway route.
``` c
ioctl(sock, SIOCADDRT, &route)
```
adds the route.
### Cleanup
`goto fail` jumps to one common cleanup block:
``` c
fail:
    close(sock);
    return -1;
```
This avoids repeating the same cleanup after every possible error.
## 3 likely questions
**Q1. Where do `net_ip`, `net_prefix`, and `net_gw` come from?** They
are fields in the container configuration populated before `container_run()`.
**Q2. What does `0.0.0.0/0` mean?** It is the default route, matching
destinations for which no more specific route exists.
**Q3. Why use `goto fail`?** All these errors need the same cleanup, so
one shared cleanup path avoids duplicated code. ------------------------------------------------------------------------
# Private helper: `make_ipv4_address()`
``` c
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
The cast converts the generic `sockaddr *` into the IPv4-specific `sockaddr_in *`. `memset()` clears the structure. `sin_family = AF_INET` marks it as IPv4. `inet_pton()` converts text such as `10.44.0.2` into binary address form.
### 3 likely questions
**Q1. Why cast to `sockaddr_in`?** Because the caller supplies generic
socket-address storage, but this helper needs IPv4-specific fields.
**Q2. What does `inet_pton()` do?** Converts printable IP text to binary
network-address form.
**Q3. Who uses this helper?** `container_net_config()` uses it for the
interface address, route destination, route mask, and gateway. ------------------------------------------------------------------------
# Private helper: `setup_filesystem()`
## Purpose
This prepares the root filesystem and makes the supplied `rootfs` become `/`. The source explicitly warns that the order matters. fileciteturn3file0L171-L236
## 1. Make mounts private
``` c
mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
```
`MS_REC` applies the operation recursively. `MS_PRIVATE` prevents mount propagation back to the host.
## 2. Bind rootfs to itself
``` c
mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL);
```
Makes `rootfs` a mount point, which `pivot_root()` needs.
## 3. Make rootfs read-only
``` c
mount(NULL, rootfs, NULL,
      MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);
```
Remounts the prepared root filesystem read-only.
## 4. Add writable `/tmp`
``` c
snprintf(path, sizeof path, "%s/tmp", rootfs);
mount("tmpfs", path, "tmpfs", 0, NULL);
```
Creates a writable tmpfs over the container's `/tmp`.
## 5. Add minimal `/dev`
``` c
snprintf(path, sizeof path, "%s/dev", rootfs);
mount("tmpfs", path, "tmpfs", 0, NULL);
```
Creates a minimal writable device filesystem. Then:
``` c
bind_device(rootfs, "null");
bind_device(rootfs, "zero");
```
places the host's existing device nodes into it.
## 6. Mount `/proc`
``` c
snprintf(path, sizeof path, "%s/proc", rootfs);
mount("proc", path, "proc", 0, NULL);
```
Creates the container's proc filesystem before the old root is detached.
## 7. Pivot
``` c
chdir(rootfs);
```
Makes the future root the current directory.
``` c
syscall(SYS_pivot_root, ".", ".");
```
Changes the process's root to the prepared filesystem. glibc has no wrapper for this call, so the implementation uses `syscall()`.
``` c
umount2(".", MNT_DETACH);
```
Detaches the old host root.
``` c
chdir("/");
```
Moves the current directory to the new root. Every failed operation prints an error and returns `-1`.
### 3 likely questions
**Q1. Why make `/` private first?** To stop mount changes in the
container from propagating back to the host.
**Q2. Why does `/tmp` remain writable if the rootfs is
read-only?** Because tmpfs is mounted separately on top of `/tmp`.
**Q3. What does `pivot_root()` accomplish?** It makes the prepared
rootfs become the process's `/` and allows the old host root to be detached. ------------------------------------------------------------------------
# Private helper: `bind_device()`
``` c
static int bind_device(const char *rootfs, const char *name)
{
    char host_path[PATH_MAX];
    char target_path[PATH_MAX];

    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "container: create %s: %s\n",
                target_path, strerror(errno));
        return -1;
    }
    close(fd);

    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0) {
        fprintf(stderr, "container: bind %s: %s\n",
                host_path, strerror(errno));
        return -1;
    }

    return 0;
}
```
The first `snprintf()` creates the host device path, such as `/dev/null`. The second creates the target path inside the rootfs. `open(... O_CREAT ...)` ensures the target exists because a bind mount needs an existing target. `close(fd)` closes the temporary descriptor. `mount(... MS_BIND ...)` performs the actual bind mount.
### 3 likely questions
**Q1. Why does the target need to exist?** The bind mount needs an
existing target path.
**Q2. Why not create a new device with `mknod()`?** The project's
user-namespace setup does not permit the required device-node creation.
**Q3. Who uses `bind_device()`?** `setup_filesystem()` uses it for
`null` and `zero`. ------------------------------------------------------------------------
# 8. `container_seccomp()`
## Purpose and caller
This function constructs and installs the seccomp filter. The filter kills the process for an unexpected architecture, returns `EPERM` for denied syscalls, and allows everything else. fileciteturn3file0L424-L502 The denied list is:
``` text
ptrace
mount
umount2
pivot_root
chroot
setns
unshare
reboot
swapon
swapoff
kexec_load
init_module
finit_module
delete_module
```
## Filter construction
``` c
struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1];
size_t count = 0;
```
There are four fixed instructions, two instructions per denied syscall, and one final allow instruction. `count` tracks the next unused slot.
## Architecture instruction
``` c
filter[count++] = (struct sock_filter)BPF_STMT(
    BPF_LD | BPF_W | BPF_ABS,
    offsetof(struct seccomp_data, arch));
```
Loads the architecture field from the seccomp data. The cast is needed because `BPF_STMT` expands to an initializer-list form.
``` c
filter[count++] = (struct sock_filter)BPF_JUMP(
    BPF_JMP | BPF_JEQ | BPF_K,
    OUR_AUDIT_ARCH, 1, 0);
```
Compares the loaded architecture against the architecture this program was built for.
``` c
filter[count++] = (struct sock_filter)BPF_STMT(
    BPF_RET | BPF_K,
    SECCOMP_RET_KILL_PROCESS);
```
If the architecture check fails, execution reaches this instruction and kills the process. This matters because syscall numbers can mean different things on different architectures.
## Load syscall number
``` c
filter[count++] = (struct sock_filter)BPF_STMT(
    BPF_LD | BPF_W | BPF_ABS,
    offsetof(struct seccomp_data, nr));
```
Loads the current syscall number.
## Check every denied syscall
``` c
for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++)
```
Loops through the static denylist. For each syscall:
``` c
BPF_JUMP(... denied_syscalls[i], 0, 1)
```
checks whether the current syscall number matches. If it matches, the next instruction returns:
``` c
SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)
```
so the syscall fails with `EPERM`. If it does not match, the jump skips that error-return instruction and continues checking the next denied syscall.
## Allow everything else
``` c
filter[count++] =
    (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K,
        SECCOMP_RET_ALLOW);
```
Any syscall that survived every deny check is allowed. Thus the logic is:
``` text
wrong architecture -> kill
denied syscall      -> EPERM
anything else       -> allow
```
## Build the program
``` c
struct sock_fprog program = {
    .len = (unsigned short)count,
    .filter = filter,
};
```
Packages the filter array and its length in the structure expected by seccomp.
## `no_new_privs`
``` c
prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)
```
sets `no_new_privs`, required here so the filter can be installed without `CAP_SYS_ADMIN`.
## Install
``` c
syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program)
```
passes the BPF program to the kernel. `syscall()` is used because the source notes that glibc does not provide the wrapper used here.
### 3 likely questions
**Q1. What happens to a denied syscall?** The filter returns `EPERM`, so
the syscall fails rather than executing.
**Q2. Why check architecture first?** Syscall numbers are
architecture-dependent. A number safe on one architecture could refer to another syscall on another architecture.
**Q3. Why is seccomp installed last?** The filter blocks `mount`,
`pivot_root`, and related setup calls that are still needed by the filesystem setup. ------------------------------------------------------------------------
# Private helper: `drop_capabilities()`
## Purpose
This helper removes Linux capabilities before the command runs. The source explicitly performs bounding-set removal first, then clears capability sets, then sets `no_new_privs`. fileciteturn3file0L239-L270
``` c
for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
    if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 &&
        errno != EINVAL) {
        ...
        return -1;
    }
}
```
The loop visits every capability number. `PR_CAPBSET_DROP` removes each capability from the bounding set. `EINVAL` is ignored because the kernel may support fewer capabilities than the header's maximum. Then:
``` c
struct __user_cap_header_struct header;
struct __user_cap_data_struct data[2];
memset(&header, 0, sizeof header);
memset(data, 0, sizeof data);
```
creates and clears the Linux capability structures.
``` c
header.version = _LINUX_CAPABILITY_VERSION_3;
header.pid = 0;
```
selects capability API version 3 and the current process. Because `data` is zeroed, the represented effective/permitted/inheritable sets are cleared.
``` c
syscall(SYS_capset, &header, data)
```
applies the empty capability sets. Finally:
``` c
prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)
```
prevents later execution from gaining new privileges.
### 3 likely questions
**Q1. Why drop the bounding set first?** The source specifies this
ordering because removing capabilities from the bounding set requires the relevant privilege while it still exists.
**Q2. Why is `data` an array of two structures?** Linux capability
version 3 uses two data structures to represent the capability bit ranges.
**Q3. What is the difference between capabilities and
seccomp?** Capabilities remove categories of privilege; seccomp restricts which system calls can execute. ------------------------------------------------------------------------
# 9. `container_init()`
## Purpose and caller
`container_init()` is the container's init process, effectively PID 1 inside the new PID namespace. `child_entry()` calls it. It waits for parent setup, configures the container, starts the command, and reaps children. fileciteturn3file0L516-L559
``` c
int container_init(struct container *c)
{
    char release_byte;
    close(c->sync[1]);

    if (read(c->sync[0], &release_byte, 1) != 1) {
        fprintf(stderr, "container: parent did not release the container\n");
        return 1;
    }

    close(c->sync[0]);

    if (container_setup(c) < 0)
        return 1;

    pid_t command_pid = fork();
    if (command_pid < 0) {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }

    if (command_pid == 0) {
        execvp(c->argv[0], c->argv);
        fprintf(stderr, "container: exec %s: %s\n",
                c->argv[0], strerror(errno));
        _exit(127);
    }

    for (;;) {
        int status;
        pid_t reaped = waitpid(-1, &status, 0);

        if (reaped < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }

        if (reaped == command_pid)
            return status_to_exit_code(status);
    }
}
```
## Synchronization
``` c
char release_byte;
```
Storage for the one-byte release signal.
``` c
close(c->sync[1]);
```
The child closes its copy of the pipe's write end because it only needs to read.
``` c
read(c->sync[0], &release_byte, 1)
```
blocks until the parent writes one byte or closes the pipe. If exactly one byte is not received:
``` c
return 1;
```
The child refuses to run. This is how the parent can abort the child if setup failed.
``` c
close(c->sync[0]);
```
After release, the read end is no longer needed.
## Inside-container setup
``` c
if (container_setup(c) < 0)
    return 1;
```
Runs hostname, networking, filesystem, capability, and seccomp setup.
## Fork command
``` c
pid_t command_pid = fork();
```
Creates a child for the actual requested command. If `fork()` fails, container init exits.
``` c
if (command_pid == 0)
```
selects the command child.
``` c
execvp(c->argv[0], c->argv);
```
replaces that process image with the requested program. If `execvp()` succeeds, it never returns. If it returns, the execution failed:
``` c
_exit(127);
```
terminates the command child.
## Reaping
The container init remains alive as PID 1.
``` c
for (;;)
```
keeps it alive.
``` c
waitpid(-1, &status, 0)
```
waits for any child, not just the command. This is necessary because PID 1 must reap orphaned children in its PID namespace.
``` c
if (errno == EINTR)
    continue;
```
Retries if a signal interrupted the wait.
``` c
if (reaped == command_pid)
    return status_to_exit_code(status);
```
Once the requested command exits, convert its encoded wait status to the final command exit code.
### 3 likely questions
**Q1. Why does container init wait on a pipe?** To prevent it from
running before the parent finishes identity, cgroup, and network setup.
**Q2. Why does it fork instead of immediately `execvp()`?** Because
container init must remain PID 1 to reap other children.
**Q3. Why does it call `waitpid(-1, ...)`?** `-1` means any child. PID 1
needs to reap orphaned children as well as the requested command. ------------------------------------------------------------------------
# Private helper: `status_to_exit_code()`
``` c
static int status_to_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}
```
`waitpid()` returns an encoded status, not simply the command's exit number.
``` c
WIFEXITED(status)
```
checks for normal termination.
``` c
WEXITSTATUS(status)
```
extracts the normal exit code. If a signal killed the command:
``` c
WIFSIGNALED(status)
```
is true, and:
``` c
128 + WTERMSIG(status)
```
produces the conventional shell-style status.
### 3 likely questions
**Q1. Why not return `status` directly?** Because `status` contains
encoded termination information.
**Q2. What does `WEXITSTATUS()` extract?** The normal exit code when the
child exited normally.
**Q3. What does `WTERMSIG()` return?** The signal number that terminated
the child. ------------------------------------------------------------------------
# 10. `container_run()`
## Purpose and caller
This is the main orchestrator. `main.c` calls it. It performs parent-side preparation, creates the child, releases it only after setup, waits for it, and cleans up. fileciteturn3file0L561-L644
## Cgroup first
``` c
if (container_cgroup_init(c) < 0)
    return 1;
```
Creates and configures the cgroup before the child is created/released.
## Synchronization pipe
``` c
if (pipe(c->sync) < 0) {
    ...
}
```
creates:
``` text
c->sync[0] = read end
c->sync[1] = write end
```
Parent writes a byte; child reads it.
## Allocate child stack
``` c
char *stack = malloc(CONTAINER_STACK_SIZE);
```
`clone()` needs a stack for its callback. If allocation fails, the pipe and cgroup are cleaned up.
## Clone
``` c
pid_t child = clone(child_entry,
                    stack + CONTAINER_STACK_SIZE,
                    container_namespaces() | SIGCHLD,
                    c);
```
This is the central process-creation line. Arguments are:
``` text
child_entry
    callback where the child begins

stack + CONTAINER_STACK_SIZE
    top of child stack

container_namespaces() | SIGCHLD
    namespace and signal flags

c
    argument passed to child_entry
```
The stack grows downward, so the top of the allocated buffer is passed. The child begins in `child_entry()`, which calls `container_init()`. The parent receives the child's host PID in `child`.
## Parent setup
``` c
int setup_ok = (container_write_idmaps(c, child) == 0);
```
Configures UID/GID mapping. The comparison turns the result into success/failure.
``` c
setup_ok = setup_ok &&
           (container_cgroup_enter(c, child) == 0);
```
If mapping succeeded, move the child into its cgroup. Because `&&` short-circuits, the second operation is skipped if `setup_ok` is already false.
## Host networking
``` c
if (c->net_enabled)
    (void)container_net_host_setup(c, child);
```
Calls the provided host-side networking implementation. It is best-effort in this project.
## Release barrier
``` c
close(c->sync[0]);
```
Parent no longer needs its read end.
``` c
if (setup_ok && write(c->sync[1], "x", 1) != 1)
```
If setup succeeded, write one byte to release the child. If setup failed, no byte is written.
``` c
close(c->sync[1]);
```
Closing the parent's write end causes the child's blocked `read()` to return EOF when the parent intentionally refuses to release it. Thus:
``` text
setup succeeds -> write byte -> child runs
setup fails    -> close pipe -> child exits
```
## Wait
``` c
while (waitpid(child, &status, 0) < 0)
```
waits for container init. If `errno == EINTR`, retry. Otherwise report failure.
## Cleanup
``` c
free(stack);
```
The child is gone, so the allocated clone stack is no longer needed.
``` c
container_net_host_teardown(c);
```
removes optional host-side networking.
``` c
container_cleanup(c);
```
removes the cgroup. Finally:
``` c
if (!setup_ok)
    return 1;

return status_to_exit_code(status);
```
If parent setup failed, report failure. Otherwise return the command's exit status.
### 3 likely questions
**Q1. Who calls `container_run()`?** `main.c`.
**Q2. Why does the parent not simply let the child continue after
`clone()`?** Because identity mapping, cgroup membership, and other required setup must occur first.
**Q3. What happens if setup fails?** `setup_ok` becomes false, the child
is not released, the parent reaps it, and the runtime returns failure. ------------------------------------------------------------------------
# Private helper: `child_entry()`
``` c
static int child_entry(void *arg)
{
    return container_init((struct container *)arg);
}
```
`clone()` requires a callback accepting `void *`. The runtime passes `c` as the fourth argument to `clone()`. `arg` receives that same pointer as a generic `void *`.
``` c
(struct container *)arg
```
casts it back to the correct type. Then:
``` c
container_init(...)
```
starts the container-side lifecycle.
### 3 likely questions
**Q1. Why not pass `container_init` directly to `clone()`?** Its
signature does not match the callback signature required by this `clone()` interface.
**Q2. Where does `arg` come from?** From the `c` argument passed to
`clone()`.
**Q3. What does the cast accomplish?** It tells C to interpret the
generic pointer as `struct container *`. ------------------------------------------------------------------------
# 11. `container_cleanup()`
## Purpose and caller
This removes the container's cgroup after the container process has been reaped. fileciteturn3file0L647-L663
``` c
int container_cleanup(struct container *c)
{
    if (c->cg_path[0] == '\0')
        return 0;

    if (rmdir(c->cg_path) < 0 && errno != ENOENT) {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    return 0;
}
```
## Line by line
``` c
if (c->cg_path[0] == '\0')
    return 0;
```
Checks whether the cgroup path string is empty. A C string beginning with `'\0'` is empty, so there is nothing to remove.
``` c
if (rmdir(c->cg_path) < 0 && errno != ENOENT)
```
Attempts to remove the cgroup directory. If the directory is already gone, `errno == ENOENT`, which is accepted as success. If another error occurs:
``` c
fprintf(stderr, ...)
return -1;
```
reports failure.
``` c
return 0;
```
means the desired final state was achieved: the cgroup is gone.
### 3 likely questions
**Q1. Who calls `container_cleanup()`?** `container_run()` calls it
after waiting for container init.
**Q2. Why accept `ENOENT`?** Because if the cgroup is already absent,
cleanup has already achieved its intended result.
**Q3. Why wait before removing the cgroup?** The container's processes
must leave the cgroup before the cgroup directory can be removed. ------------------------------------------------------------------------
# The three most important distinctions to memorize
## 1. `snprintf()` versus `write_file()`
``` text
snprintf()
    constructs a string in memory
write_file()
    performs the actual write to the kernel interface
```
For example:
``` c
snprintf(path, ..., "%s/cgroup.procs", c->cg_path);
```
does not move a process. This does:
``` c
write_file(path, pid_text);
```
Likewise:
``` c
snprintf(mapping, ..., "0 %d 1", ...);
```
does not establish an ID mapping. This does:
``` c
write_file(path, mapping);
```
## 2. Namespace isolation versus cgroup limits
``` text
namespace
    controls the process's view of the system

cgroup
    controls resource consumption
```
A PID namespace can isolate the process view without limiting memory. A cgroup can limit memory without changing the process's view. The runtime uses both.
## 3. Parent setup versus child setup
The parent must perform operations that require knowing the child's host identity:
``` text
UID/GID maps
cgroup membership
host-side veth setup
```
The child performs operations that need to happen from inside its new namespaces:
``` text
hostname
container-side networking
filesystem pivot
capability removal
seccomp
command execution
```
The synchronization pipe connects these two phases. ------------------------------------------------------------------------
# Project-level picture that connects the functions

## What the project builds
This is a small Linux container runtime. Given a rootfs and command, it isolates
users, PIDs, mounts, hostname, and networking; limits resources; removes
privileges; filters dangerous syscalls; preserves exit status; and cleans up.
It is not a VM: container processes run directly on the host Linux kernel.
The course provided `main.c`, `container.h`, `util.c` (`write_file()`), `net.c`
(host veth setup/teardown), the rootfs builder, runner, and tests. `container.c`
requires 11 functions plus 7 private helpers. The original summary reports
**32 passed, 0 failed** for the tested branch.

## Complete lifecycle
```text
main -> container_run -> cgroup_init -> pipe -> clone(child_entry)
  child -> container_init(PID 1) -> sync -> container_setup
        -> hostname -> networking -> filesystem/pivot
        -> capabilities/no_new_privs -> seccomp
        -> fork -> execvp(command) -> waitpid(-1) -> status
  parent -> UID/GID maps -> cgroup enter -> host veth -> release
         -> wait -> network teardown -> cgroup cleanup -> status
```
The pipe is the ordering barrier: identity, cgroup, and host networking finish
before release. Failure means no release byte, so the child sees EOF and stops.

## Why the order matters
| Rule | Purpose |
|---|---|
| UID/GID maps before release | Identity exists before setup. |
| Cgroup before release | Command never runs outside intended limits. |
| Veth before release | Child sees its interface during setup. |
| Private mounts before mount changes | Mount changes cannot leak to host. |
| `/proc` before pivot/detach | Proc gets the intended PID namespace view. |
| Setup before capability removal | Setup needs privilege. |
| Seccomp last | Setup syscalls such as `mount()` still work. |
| Wait before cgroup removal | Cgroup must be empty before removal. |

## Security model
Defense in depth combines **namespaces** (visibility isolation), **user mapping**
(container UID 0 maps to the caller's ordinary host UID), **read-only
rootfs/minimal `/dev`** (reduced exposure), **cgroups** (resource limits),
**capabilities** (remove root powers), **`no_new_privs`** (prevent later
privilege restoration), and **seccomp** (deny dangerous kernel entry points).

## Important data flow
`struct container *c` is the central configuration object.
| Field | Consumer |
|---|---|
| `argv` | `container_init()` -> `execvp()` |
| `hostname` | `container_setup()` -> `sethostname()` |
| `rootfs` | `container_setup()` -> `setup_filesystem()` |
| `pids_max`, `mem_max` | `container_cgroup_init()` -> cgroup files |
| `cgroup_base`, `name` | `container_cgroup_init()` -> `cg_path` |
| `cg_path` | limit helper, enter, cleanup |
| network fields | host setup and `container_net_config()` |
| `sync[0]`, `sync[1]` | parent release / child wait |

## Exit status, PID 1, and cleanup
```text
command -> waitpid() in container_init() -> status_to_exit_code()
        -> init -> clone child exits -> waitpid() in container_run()
        -> status_to_exit_code() -> run -> main/shell
```
`waitpid()` returns encoded status: `WEXITSTATUS()` extracts normal exits and
`128 + WTERMSIG()` preserves signal termination conventionally. PID 1 uses
`waitpid(-1)` to reap orphans and prevent zombies. After the final process exits,
namespace/private-mount state disappears automatically.

## What the grading areas exercise
- **Namespaces:** flags, ID maps, hostname, PID visibility, mounts, networking.
- **Filesystem:** pivoting, read-only root, `/tmp`, devices, propagation, `/proc`.
- **Processes:** PID 1, orphan adoption, zombie reaping, exit propagation.
- **Cgroups:** limits and process/memory enforcement.
- **Security:** empty capabilities and denied syscalls.
- **Lifecycle:** stale cgroups, simultaneous containers, cleanup, AddressSanitizer.
## Short interview explanation
“The runtime creates a child with five new namespaces and blocks it on a pipe.
The parent maps container root to the caller, puts the child in a limited
cgroup, and optionally creates a veth. After release, the child configures
hostname/networking, pivots into a read-only rootfs with private `/tmp`,
`/dev`, and `/proc`, drops capabilities, and installs seccomp. It stays PID 1
while a forked child executes the command so it can reap orphans. The parent
waits, tears down networking and the cgroup, and returns the command status.”

# Final exam-style mental model
If asked to explain the whole program, say:
> `main.c` calls `container_run()`. The parent first creates and
> configures the cgroup, then creates a synchronization pipe and
> allocates a stack. It calls `clone()` with a bitmask returned by
> `container_namespaces()`, creating a child with separate user, PID,
> mount, UTS, and network namespaces. The child immediately waits on the
> pipe. The parent uses the child's host PID to write its UID/GID
> mappings, places the child into the cgroup, and performs any required
> host-side network setup. Only after successful setup does the parent
> write the release byte. The child then enters `container_init()`,
> which calls `container_setup()` to configure the hostname, networking,
> root filesystem, capabilities, and seccomp. After setup, container
> init forks and execs the requested command, while remaining alive as
> PID 1 so it can reap children. The parent waits for container init,
> tears down optional networking, removes the cgroup, and returns the
> command's exit status.
The key ordering is:
``` text
PARENT
cgroup setup
    ↓
clone namespaces
    ↓
ID mapping
    ↓
cgroup membership
    ↓
host network setup
    ↓
release child

CHILD
wait for release
    ↓
hostname/network
    ↓
filesystem
    ↓
drop capabilities
    ↓
seccomp
    ↓
fork + exec command
    ↓
reap children

PARENT
wait
    ↓
network teardown
    ↓
cgroup cleanup
    ↓
return command status
```
That ordering is the central story of `container.c`.
