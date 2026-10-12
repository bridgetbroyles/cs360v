/*
 * containerannotated.c
 *
 * This is an annotated copy of container.c. It preserves the implementation's
 * behavior while explaining why important statements exist. The normal build
 * still uses container.c; this file is a study copy unless the Makefile is run
 * with SRC=containerannotated.c.
 *
 * HIGH-LEVEL STORY
 * ----------------
 * A container is a group of ordinary Linux processes that share the host
 * kernel but receive isolated views and restrictions. There is no guest kernel.
 *
 * Provided main.c fills a `struct container` and calls container_run(). The
 * parent creates a resource-limited cgroup, a synchronization pipe, and a child
 * with new user, PID, mount, UTS, and network namespaces. The child becomes PID
 * 1 inside the PID namespace and waits on the pipe. While it waits, the parent
 * maps container root to the caller's ordinary host UID/GID, places the child
 * in the cgroup, and optionally constructs the host side of a virtual network.
 *
 * After the parent sends one release byte, container init configures its
 * hostname/network, pivots into a small read-only root filesystem, drops every
 * capability, and installs a seccomp system-call filter. Init then forks: its
 * child becomes the requested command through execvp(), while init stays PID 1
 * to reap the command and any orphan descendants. Finally, the host parent
 * waits for init, removes networking and the cgroup, and returns the command's
 * exit status.
 *
 * PROCESS TREE
 * ------------
 * host runtime: container_run()
 * `-- container init: container_init(), PID 1 inside
 *     `-- requested command: execvp(c->argv[0], c->argv)
 *
 * REQUIRED VS. ADDED FUNCTIONS
 * ----------------------------
 * The specification required the eleven public container_* functions in this
 * file. The static functions are private helpers added by this implementation:
 * write_cgroup_limit, bind_device, setup_filesystem, drop_capabilities,
 * make_ipv4_address, status_to_exit_code, and child_entry.
 */

/* Expose GNU/Linux declarations such as clone() before including headers. */
#define _GNU_SOURCE

/* Project-provided declarations, struct container, and stack-size constant. */
#include "container.h"

/* clone() flags and declaration. */
#include <sched.h>
/* Diagnostics and formatted strings. */
#include <stdio.h>
/* malloc() and free(). */
#include <stdlib.h>
/* strlen(), memset(), strncpy(), and strerror(). */
#include <string.h>
/* read(), write(), close(), fork(), execvp(), getuid(), and getgid(). */
#include <unistd.h>
/* errno and error constants such as EEXIST, EINVAL, EINTR, and ENOENT. */
#include <errno.h>
/* open() flags. */
#include <fcntl.h>
/* SIGCHLD. */
#include <signal.h>
/* mount(), umount2(), and mount flags. */
#include <sys/mount.h>
/* mkdir() and rmdir-related declarations. */
#include <sys/stat.h>
/* syscall() numbers such as SYS_pivot_root, SYS_capset, and SYS_seccomp. */
#include <sys/syscall.h>
/* waitpid() and wait-status macros. */
#include <sys/wait.h>
/* ioctl(). */
#include <sys/ioctl.h>
/* prctl(). */
#include <sys/prctl.h>
/* Linux capability structures and constants. */
#include <linux/capability.h>
/* Classic BPF instruction structures/macros. */
#include <linux/filter.h>
/* Seccomp structures and return actions. */
#include <linux/seccomp.h>
/* Architecture identifiers used by the seccomp filter. */
#include <linux/audit.h>
/* offsetof() and size_t. */
#include <stddef.h>
/* socket(). */
#include <sys/socket.h>
/* struct ifreq and interface flags. */
#include <net/if.h>
/* struct rtentry and route flags. */
#include <net/route.h>
/* struct sockaddr_in. */
#include <netinet/in.h>
/* inet_pton() and htonl(). */
#include <arpa/inet.h>

/*
 * REQUIRED FUNCTION: container_namespaces
 * Called by container_run() when it creates container init with clone().
 * Returns one bitmask selecting the five isolated resource views.
 */
int container_namespaces(void)
{
    /*
     * Bitwise OR combines independent flags; it does not choose only one.
     * USER creates a new identity view. In that view ("inside the container"),
     * the process can be UID 0; outside it, the host still treats the same
     * process as the ordinary account that launched the container.
     * PID gives the child its own process tree, where it becomes PID 1. Thus a
     * process listing inside shows container processes, not unrelated host ones.
     * NEWNS gives it a private mount table: mounts it creates for /proc, /tmp,
     * or the new root affect only its view, not the host's filesystem view.
     * UTS gives it a separate hostname, so programs can use a container name
     * for configuration or identification without changing the host name.
     * NET gives it separate interfaces, addresses, and routes for --net mode.
     */
    return CLONE_NEWUSER
         | CLONE_NEWPID
         | CLONE_NEWNS
         | CLONE_NEWUTS
         | CLONE_NEWNET;
}

/*
 * REQUIRED FUNCTION: container_write_idmaps
 * Called by the host parent after clone() and before releasing the child.
 * Maps the child’s container-local UID/GID 0 to the caller's ordinary host IDs.
 * "Inside" means the child is asking the kernel for an identity through its new
 * user namespace. "Host" means the kernel is checking the same process against
 * the real machine's users and files. The map connects those two number spaces.
 */
int container_write_idmaps(struct container *c, pid_t child)
{

    (void)c;

    /* Reusable buffers for a /proc path and its textual mapping value. */
    char path[64];
    char mapping[64];

    /* Build a path such as /proc/4312/uid_map for the host-visible child PID. */
    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);

    /*
     * A uid_map line has: <container starting UID> <host starting UID> <count>.
     * If getuid() returns 1000, this writes "0 1000 1": the one container
     * identity numbered 0 maps to the one host identity numbered 1000. UID 0
     * is called root inside the container, but it does not become host root.
     */
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());

    /* Provided write_file() writes the mapping; propagate any failure. */
    if (write_file(path, mapping) < 0)
        return -1;

    /*
     * Linux requires this before an unprivileged process may write gid_map.
     * Supplementary groups can carry extra file permissions. Denying setgroups
     * makes the child give up changing them before its group mapping is set.
     */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    /*
     * gid_map has the same three-number format. If getgid() is 1000, "0 1000
     * 1" means container group 0 corresponds to host group 1000. This keeps
     * group permission checks as restricted as the UID mapping.
     */
    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    /* All three required kernel-control writes succeeded. */
    return 0;
}

/*
 * ADDED HELPER: write_cgroup_limit
 * Called by container_cgroup_init() for each cgroup limit file.
 * Cgroup controls accept text, so this converts a long to decimal or "max".
 */
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
{
    /* PATH_MAX plus extra space for the slash and cgroup filename. */
    char path[PATH_MAX + 32];

    /* Enough text for a long integer or the word "max". */
    char value[32];

    /*
     * container_cgroup_init() already saved this container's directory in
     * c->cg_path. Append the requested control-file name: for example,
     * /sys/fs/cgroup/demo plus memory.max becomes
     * /sys/fs/cgroup/demo/memory.max, the file that holds the memory limit.
     */
    snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);

    /* The assignment represents an unlimited negative setting as literal max. */
    if (limit < 0)
        snprintf(value, sizeof value, "max");
    else
        /* %ld converts the long integer to the text expected by cgroup v2. */
        snprintf(value, sizeof value, "%ld", limit);

    /* Return write_file's 0 success or -1 failure directly. */
    return write_file(path, value);
}

/*
 * REQUIRED FUNCTION: container_cgroup_init
 * Called first by container_run() in the parent.
 * Creates this container's control group and applies process/memory limits.
 * 
 */
int container_cgroup_init(struct container *c)
{
    /* Temporary path for a control file in the parent cgroup. */
    char path[PATH_MAX];

    /* Select the parent file that delegates controllers to child cgroups. */
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);

    /* Enable the process-count and memory controllers below cgroup_base. */
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    /*
     * Save this container's cgroup-directory path in c->cg_path. For example,
     * base /sys/fs/cgroup plus name demo becomes /sys/fs/cgroup/demo. The next
     * function appends /cgroup.procs to this saved path; cleanup later removes
     * this same directory.
     */
    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s",
             c->cgroup_base, c->name);

    /*
     * Example: base /sys/fs/cgroup and name demo produce
     * /sys/fs/cgroup/demo, the directory that will contain this container's
     * pids.max and memory.max files.  EEXIST permits reuse after a crash left
     * that directory behind.
     */
    /*
     * 0755 is the directory's ordinary read/traverse permission mode. It does
     * not set the process or memory limits; the cgroup control files below it
     * do that. The kernel manages access to those control files separately.
     */
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    /* Limit how many processes init and all descendants may create. */
    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;

    /* Limit the group's memory consumption. */
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;

    /* Disable swap so swapping cannot postpone enforcement of memory.max. */
    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
        return -1;

    return 0;
}

/*
 * REQUIRED FUNCTION: container_cgroup_enter
 * Called by container_run() before releasing the child.
 * Moves container init into the cgroup; its future descendants inherit it.
 */
int container_cgroup_enter(struct container *c, pid_t child)
{
    /* Buffers for the membership control path and decimal PID text. */
    char path[PATH_MAX + 32];
    char pid_text[16];

    /* cgroup.procs is the cgroup-v2 membership control file. */
    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);

    /*
     * cgroup.procs expects decimal text, not the in-memory binary pid_t. For a
     * child whose host PID is 4312, this turns the number into "4312" so that
     * write_file() can place those four characters in cgroup.procs.
     */
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);

    /*
     * Writing the host PID moves init into the limited group. Init is the
     * clone() child that becomes PID 1 in the container's separate PID view.
     * Example: writing 4312 to /sys/fs/cgroup/demo/cgroup.procs places that
     * process in demo. Commands it later forks inherit demo's limits.
     */

    return write_file(path, pid_text);
}

/*
 * ADDED HELPER: bind_device
 * Called twice by setup_filesystem(), for /dev/null and /dev/zero.
 * Creates a mount target and overlays the corresponding host device there.
 * 
 * dev/null is used in linux for discarding output streams and as a dev/zero is  
 * source of empty input streams. devices expected to exist in linux
 */
static int bind_device(const char *rootfs, const char *name)
{
    /* Host source and future-container destination paths. */
    char host_path[PATH_MAX];
    char target_path[PATH_MAX];

    /*
     * Build both sides of the bind mount. For name "null", host_path is the
     * existing host device /dev/null; target_path is the empty file at
     * <rootfs>/dev/null that will become /dev/null after changing root.
     */
    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    /*
     * A bind mount needs an existing destination. O_CREAT makes the empty file;
     * O_CLOEXEC prevents this descriptor leaking if an exec unexpectedly occurs.
     */
    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "container: create %s: %s\n",
                target_path, strerror(errno));
        return -1;
    }

    /* The path remains after closing; only the descriptor is released. */
    close(fd);

    /*
     * MS_BIND makes the existing host device appear at target_path. After the
     * root switch, the command can use /dev/null and /dev/zero normally. This
     * avoids creating new device nodes, an operation this unprivileged user
     * namespace is not permitted to perform.
     */
    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0) {
        fprintf(stderr, "container: bind %s: %s\n",
                host_path, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * ADDED HELPER: setup_filesystem
 * Called by container_setup() while init still has mount capabilities.
 * Constructs the private filesystem and makes rootfs become container `/`.
 */
static int setup_filesystem(const char *rootfs)
{
    /* Reused for <rootfs>/tmp, <rootfs>/dev, and <rootfs>/proc. */
    char path[PATH_MAX];

    /*
     * Recursively mark mounts private. Without this, mount changes could
     * propagate through shared-mount relationships back to the host.
     */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /* pivot_root() requires rootfs to be a mount point, so bind it to itself. */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }

    /* Remount the rootfs bind read-only so the command cannot alter its image. */
    if (mount(NULL, rootfs, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        fprintf(stderr, "container: remount rootfs read-only: %s\n",
                strerror(errno));
        return -1;
    }

    /* Build the future /tmp path under the not-yet-pivoted rootfs. */
    snprintf(path, sizeof path, "%s/tmp", rootfs);

    /* Overlay writable in-memory tmpfs because the underlying root is read-only. */
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /* Build and mount a fresh /dev so host devices are not broadly exposed. */
    snprintf(path, sizeof path, "%s/dev", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }

    /* Expose only the two devices required by the project. */
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;

    /* Build the future /proc path. */
    snprintf(path, sizeof path, "%s/proc", rootfs);

    /*
     * Mount proc before pivoting. It will reflect the new PID namespace; doing
     * this after the old /proc is detached can fail with EPERM.
     */
    if (mount("proc", path, "proc", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /* Move into rootfs so "." identifies the desired new root. */
    if (chdir(rootfs) < 0) {
        fprintf(stderr, "container: chdir rootfs: %s\n", strerror(errno));
        return -1;
    }

    /* glibc has no wrapper, so invoke pivot_root through syscall(). */
    if (syscall(SYS_pivot_root, ".", ".") < 0) {
        fprintf(stderr, "container: pivot_root: %s\n", strerror(errno));
        return -1;
    }

    /* Detach the old host root, removing the container's route back to it. */
    if (umount2(".", MNT_DETACH) < 0) {
        fprintf(stderr, "container: detach old root: %s\n", strerror(errno));
        return -1;
    }

    /* Establish a sensible current directory in the new filesystem root. */
    if (chdir("/") < 0) {
        fprintf(stderr, "container: chdir /: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * ADDED HELPER: drop_capabilities
 * Called near the end of container_setup().
 * Removes every category of root power and prevents privilege recovery.
 */
static int drop_capabilities(void)
{
    /* Visit every capability number understood by the build headers. */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        /* Remove cap from the bounding ceiling for this process and descendants. */
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
            /* EINVAL is harmless when an older kernel lacks this cap number. */
            fprintf(stderr,
                    "container: drop capability %d from bounding set: %s\n",
                    cap, strerror(errno));
            return -1;
        }
    }

    /* Header identifies the capability API version and target process. */
    struct __user_cap_header_struct header;

    /* Version 3 needs two records for capability bits 0-31 and 32-63. */
    struct __user_cap_data_struct data[2];

    /* Zero initialization makes all capability sets in data empty. */
    memset(&header, 0, sizeof header);
    memset(data, 0, sizeof data);

    /* Select the modern two-record capability structure layout. */
    header.version = _LINUX_CAPABILITY_VERSION_3;

    /* PID 0 means apply the change to the calling process. */
    header.pid = 0;

    /* Clear effective, permitted, and inheritable capabilities. */
    if (syscall(SYS_capset, &header, data) < 0) {
        fprintf(stderr, "container: clear capabilities: %s\n", strerror(errno));
        return -1;
    }

    /* Forbid later exec'd setuid/file-capability programs from gaining power. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * REQUIRED FUNCTION: container_setup
 * Called by container_init() after the parent releases it.
 * Orders all child-side isolation; privileged setup must precede restrictions.
 */
int container_setup(struct container *c)
{
    /* Change only this UTS namespace's hostname. */
    if (sethostname(c->hostname, strlen(c->hostname)) < 0) {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    /* Bring private loopback up; the specification treats failure as best effort. */
    (void)container_network();

    /* In --net mode configure the veth that provided host code already moved in. */
    if (c->net_enabled)
        (void)container_net_config(c);

    /* Build and pivot into the isolated filesystem while mount power remains. */
    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    /* Remove root-like powers after all operations that require them. */
    if (drop_capabilities() < 0)
        return -1;

    /* Install last because the filter blocks mount(), umount2(), and pivot_root(). */
    if (container_seccomp() < 0)
        return -1;

    return 0;
}

/*
 * REQUIRED FUNCTION: container_network
 * Called by container_setup() before CAP_NET_ADMIN is removed.
 * Turns on `lo`, the private interface used for localhost communication.
 */
int container_network(void)
{
    /* This socket is a handle for networking ioctls; no datagrams are sent. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* ifreq carries an interface name and request-specific data such as flags. */
    struct ifreq request;

    /* Zero unused union/padding bytes before passing the structure to the kernel. */
    memset(&request, 0, sizeof request);

    /* Select the loopback interface; leave room for a terminating NUL. */
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);

    /* Read existing flags first so later code preserves unrelated flag bits. */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read lo flags: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* Add the administratively-up and operationally-running bits. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* Ask the kernel to apply the modified flags. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring lo up: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* The interface stays configured after its control socket closes. */
    close(sock);
    return 0;
}

/*
 * ADDED HELPER: make_ipv4_address
 * Called by container_net_config().
 * Converts dotted text into the binary IPv4 socket-address structure.
 */
static int make_ipv4_address(struct sockaddr *addr, const char *ip_text)
{
    /* View the caller's generic sockaddr storage as IPv4-specific storage. */
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;

    /* Initialize every field, including unused port and padding bytes. */
    memset(ipv4, 0, sizeof *ipv4);

    /* Tell the kernel the structure contains an IPv4 address. */
    ipv4->sin_family = AF_INET;

    /* inet_pton converts presentation text to network-format address bytes. */
    if (inet_pton(AF_INET, ip_text, &ipv4->sin_addr) != 1) {
        fprintf(stderr, "container: bad IPv4 address '%s'\n", ip_text);
        return -1;
    }

    return 0;
}

/*
 * REQUIRED FUNCTION: container_net_config
 * Called by container_setup() only in --net mode.
 * Configures the container end of the provided virtual Ethernet pair.
 */
int container_net_config(struct container *c)
{
    /*
     * Example --net layout: host bridge 10.44.0.1 connects to ceth0 at
     * 10.44.0.2.  The provided host setup moved ceth0 into this namespace;
     * this function assigns its address, activates it, and adds its route.
     */
    /* Create a control socket for address, flag, mask, and route ioctls. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* One reusable interface request structure. */
    struct ifreq request;
    memset(&request, 0, sizeof request);

    /* Select the moved-in interface, normally ceth0. */
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);

    /* Parse c->net_ip into request.ifr_addr. */
    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;

    /* Assign that address to the selected interface. */
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        fprintf(stderr, "container: set %s address: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Treat the ifreq netmask storage as an IPv4 sockaddr. */
    struct sockaddr_in *netmask =
        (struct sockaddr_in *)&request.ifr_netmask;
    memset(netmask, 0, sizeof *netmask);
    netmask->sin_family = AF_INET;

    /* Begin with /0 (all mask bits zero). */
    uint32_t mask_bits = 0;

    /* Shift 32 one-bits to form a prefix such as /24 = 255.255.255.0. */
    if (c->net_prefix > 0)
        mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);

    /* Convert host byte order to the network byte order required for IPv4. */
    netmask->sin_addr.s_addr = htonl(mask_bits);

    /* Assign the calculated netmask. */
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
        fprintf(stderr, "container: set %s netmask: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Read existing interface flags before modifying them. */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read %s flags: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Preserve existing bits while adding UP and RUNNING. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* Apply the flags so the veth can carry packets. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring %s up: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Zero a routing-table entry before filling the fields we need. */
    struct rtentry route;
    memset(&route, 0, sizeof route);

    /*
     * Destination 0.0.0.0 plus mask 0.0.0.0 matches all destinations; the
     * gateway is the host bridge, normally 10.44.0.1.
     */
    if (make_ipv4_address(&route.rt_dst, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_genmask, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_gateway, c->net_gw) < 0)
        goto fail;

    /* Mark this as an enabled route whose next hop is a gateway. */
    route.rt_flags = RTF_UP | RTF_GATEWAY;

    /* Add the default route to this network namespace's route table. */
    if (ioctl(sock, SIOCADDRT, &route) < 0) {
        fprintf(stderr, "container: add default route via %s: %s\n",
                c->net_gw, strerror(errno));
        goto fail;
    }

    /* Configuration persists; release the temporary control descriptor. */
    close(sock);
    return 0;

fail:
    /* All failures after socket creation share one cleanup path. */
    close(sock);
    return -1;
}

/* Syscall numbers differ by CPU ABI, so identify the build architecture. */
#if defined(__x86_64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "container_seccomp() only supports x86_64 and aarch64"
#endif

/* The specification's system-call denylist, expressed as numeric IDs. */
static const int denied_syscalls[] = {
    __NR_ptrace,
    __NR_mount,
    __NR_umount2,
    __NR_pivot_root,
    __NR_chroot,
    __NR_setns,
    __NR_unshare,
    __NR_reboot,
    __NR_swapon,
    __NR_swapoff,
    __NR_kexec_load,
    __NR_init_module,
    __NR_finit_module,
    __NR_delete_module,
};

/* Compute array length so it stays correct if the denylist changes. */
#define NUM_DENIED_SYSCALLS (sizeof denied_syscalls / sizeof denied_syscalls[0])

/*
 * REQUIRED FUNCTION: container_seccomp
 * Called last by container_setup().
 * Installs a kernel BPF filter that rejects dangerous syscalls with EPERM.
 */
int container_seccomp(void)
{
    /* Four setup instructions, two per denied call, and one final allow. */
    struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1];

    /* count is both the next array index and final instruction count. */
    size_t count = 0;

    /* Load seccomp_data.arch, the current syscall's architecture identifier. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, arch));

    /* If architecture matches, skip the following kill instruction. */
    filter[count++] = (struct sock_filter)BPF_JUMP(
        BPF_JMP | BPF_JEQ | BPF_K, OUR_AUDIT_ARCH, 1, 0);

    /* A foreign ABI could reinterpret syscall numbers, so kill it. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* Load seccomp_data.nr, the current syscall number, for comparisons. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));

    /* Add one compare-and-deny instruction pair for every denied syscall. */
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++) {
        /* On no match skip one instruction; on match execute the EPERM return. */
        filter[count++] = (struct sock_filter)BPF_JUMP(
            BPF_JMP | BPF_JEQ | BPF_K,
            (unsigned int)denied_syscalls[i], 0, 1);

        /* Make the matched syscall fail with errno EPERM. */
        filter[count++] = (struct sock_filter)BPF_STMT(
            BPF_RET | BPF_K,
            SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    /* A syscall that matched no deny entry is allowed to execute. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    /* Package the instruction count and array pointer for SYS_seccomp. */
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };

    /* Unprivileged filter installation requires the irreversible NNP promise. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /* Install the filter through the kernel syscall interface. */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0) {
        fprintf(stderr, "container: install seccomp filter: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * ADDED HELPER: status_to_exit_code
 * Called after waitpid() by container_init() and container_run().
 * Converts waitpid's encoded status to a shell-style command result.
 */
static int status_to_exit_code(int status)
{
    /* Example: a command that runs "exit 7" becomes result 7 for the host. */
    /* A normal exit stores the program's 0-255 code in the encoded status. */
    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    /* Shell convention represents signal death as 128 + signal number. */
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    /* Stopped/continued or otherwise unexpected states count as failure. */
    return 1;
}

/*
 * REQUIRED FUNCTION: container_init
 * Runs as PID 1 inside the new PID namespace.
 * Waits for parent setup, configures isolation, launches/reaps the command.
 */
int container_init(struct container *c)
{
    /* Only the arrival of one byte matters; its stored value is irrelevant. */
    char release_byte;

    /* Init reads from the pipe, so close its unused write-end copy. */
    close(c->sync[1]);

    /*
     * Example order: the parent writes the ID maps and cgroup membership,
     * then writes one byte.  Until that byte arrives, init cannot start setup
     * with incomplete host-side configuration.
     */
    if (read(c->sync[0], &release_byte, 1) != 1) {
        fprintf(stderr, "container: parent did not release the container\n");
        return 1;
    }

    /* The release signal has arrived; the pipe is no longer needed by init. */
    close(c->sync[0]);

    /* Perform hostname, network, filesystem, capability, and seccomp setup. */
    if (container_setup(c) < 0)
        return 1;

    /* Create a separate command process so this process can remain PID 1. */
    pid_t command_pid = fork();
    if (command_pid < 0) {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }

    /* fork() returns zero only in the newly created command child. */
    if (command_pid == 0) {
        /* Replace this process image with the requested executable and args. */
        execvp(c->argv[0], c->argv);

        /* Successful exec never returns, so reaching here means failure. */
        fprintf(stderr, "container: exec %s: %s\n",
                c->argv[0], strerror(errno));

        /* Exit immediately; 127 conventionally means command could not execute. */
        _exit(127);
    }

    /* Remain PID 1 and repeatedly collect exited children/adopted orphans. */
    for (;;) {
        /* waitpid writes an encoded reason/status into this variable. */
        int status;

        /* PID -1 means wait for any child, including an adopted orphan. */
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0) {
            /* A signal interrupted the wait before completion; simply retry. */
            if (errno == EINTR)
                continue;

            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }

        /* Ignore other reaped descendants; stop when the main command exits. */
        if (reaped == command_pid)
            return status_to_exit_code(status);
    }
}

/*
 * ADDED HELPER: child_entry
 * Passed to clone() by container_run().
 * Adapts clone's required int(void *) callback to container_init's typed input.
 */
static int child_entry(void *arg)
{
    /* Recover the struct container pointer and forward init's exit code. */
    return container_init((struct container *)arg);
}

/*
 * REQUIRED FUNCTION: container_run
 * The only container.c function called directly by provided main.c.
 * Owns the entire host-parent lifecycle and returns the command's exit code.
 */
int container_run(struct container *c)
{
    /* Create the cgroup before a child exists so failure is easy to clean up. */
    if (container_cgroup_init(c) < 0)
        return 1;

    /* Create sync[0] for reading and sync[1] for writing. */
    if (pipe(c->sync) < 0) {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        container_cleanup(c);
        return 1;
    }

    /* clone() needs caller-provided stack memory for the child callback. */
    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (stack == NULL) {
        fprintf(stderr, "container: out of memory for the child stack\n");
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /*
     * Stacks grow down on supported CPUs, so pass one byte beyond the allocation.
     * Namespace flags isolate resources; SIGCHLD enables normal parent waiting.
     * Pointer c becomes child_entry's void * argument.
     */
    pid_t child = clone(child_entry, stack + CONTAINER_STACK_SIZE,
                        container_namespaces() | SIGCHLD, c);
    if (child < 0) {
        fprintf(stderr, "container: clone: %s\n", strerror(errno));
        free(stack);
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /* Record success/failure rather than returning while the child is blocked. */
    int setup_ok = (container_write_idmaps(c, child) == 0);

    /* Short-circuit: enter the cgroup only if ID mapping succeeded. */
    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    /* Provided code creates bridge/veth and moves one end into the child's netns. */
    if (c->net_enabled)
        (void)container_net_host_setup(c, child);

    /* Parent writes, so close its unused copy of the pipe's read end. */
    close(c->sync[0]);

    /* One arbitrary byte releases init only after required parent setup succeeds. */
    if (setup_ok && write(c->sync[1], "x", 1) != 1) {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }

    /*
     * Always close the write end. If setup failed and no byte was written, init
     * sees EOF and exits instead of remaining blocked or running half-configured.
     */
    close(c->sync[1]);

    /* Receive the clone child's encoded exit reason. */
    int status;

    /* Retry if a signal interrupts waitpid before it reaps the specific child. */
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            free(stack);
            return 1;
        }
    }

    /* Child has exited and can no longer use the caller-provided stack. */
    free(stack);

    /* Provided code removes host-side veth state in --net mode. */
    if (c->net_enabled)
        (void)container_net_host_teardown(c);

    /* Remove the now-empty cgroup; cleanup must not replace command status. */
    (void)container_cleanup(c);

    /* If parent setup failed, the command never validly ran. */
    if (!setup_ok)
        return 1;

    /* Init returned the command status; decode it from the parent's wait status. */
    return status_to_exit_code(status);
}

/*
 * REQUIRED FUNCTION: container_cleanup
 * Called by container_run() after init has been reaped and on early failures.
 * Removes the per-container cgroup directory when it exists and is empty.
 */
int container_cleanup(struct container *c)
{
    /* An empty first character means no cgroup path was stored. */
    if (c->cg_path[0] == '\0')
        return 0;

    /* rmdir removes an empty cgroup; already absent (ENOENT) is also success. */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT) {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    return 0;
}
