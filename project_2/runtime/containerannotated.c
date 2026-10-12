/*
 * CONTAINER.C — ANNOTATED STUDY COPY
 *
 * ASSIGNMENT CONTEXT
 *   - main.c parses the command line into struct container, then calls only
 *     container_run(). Everything after that call is this implementation's job.
 *   - The TODOs in the original starter file are the implementation checklist:
 *     which mechanisms to call and the required order.
 *   - SPEC.md is the contract. If a comment and SPEC.md disagree, follow SPEC.md.
 *   - Starter state: container_run() returned 1, so no command or test ran.
 *     The first dependency to make work was command execution: container_run()
 *     creates init, container_init() launches the command, and container_setup()
 *     pivots into rootfs where that command lives.
 *
 * PROVIDED FILES (not implemented here)
 *   - main.c: argument parsing and the only direct call to container_run().
 *   - util.c: write_file(), used for /proc and cgroup control files.
 *   - net.c: host-side --net bridge/veth creation and teardown.
 *   - container.h: struct container, function declarations, stack-size constant.
 *
 * WHAT THIS PROGRAM BUILDS
 *   - One ordinary Linux child becomes container init (PID 1 inside).
 *   - Init forks one child that becomes the requested command.
 *   - Both share the host kernel; this is a container, not a virtual machine.
 *
 * PROCESS TREE
 *   host main()
 *     -> container_run()                 host-side coordinator
 *          -> cloned container_init()    PID 1 inside; performs setup/reaping
 *               -> forked command        PID 2 inside; replaced by execvp()
 *
 * SETUP ORDER
 *   1. Parent creates cgroup, pipe, namespaces, identity maps, and optional net.
 *   2. Parent releases init by writing one byte to the pipe.
 *   3. Init sets hostname/network, enters rootfs, drops capabilities, adds seccomp.
 *   4. Init forks and reaps the requested command.
 *   5. Host parent reaps init, cleans up, and returns the command's exit status.
 *
 * FUNCTION GROUPS
 *   Namespaces/identity  container_namespaces, container_write_idmaps
 * 
 *   Resource limits      write_cgroup_limit, container_cgroup_init/enter
 * 
 *   Filesystem/security  bind_device, setup_filesystem, drop_capabilities,
 *                        container_setup, container_seccomp
 * 
 *   Networking           container_network, make_ipv4_address,
 *                        container_net_config
 * 
 *   Process lifecycle    status_to_exit_code, container_init, child_entry,
 *                        container_run, container_cleanup
 */

#define _GNU_SOURCE

#include "container.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/audit.h>
#include <stddef.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
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
    return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET;
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
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST)
    {
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
    if (fd < 0)
    {
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
    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0)
    {
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
    /*
     * Temporary buffer for building paths inside rootfs.
     * For example, if rootfs is "/images/miniroot", path can hold
     * "/images/miniroot/tmp" or "/images/miniroot/dev".
     */
    char path[PATH_MAX];

    /*
     * Stop mount changes made by this container from propagating to
     * the host or other mount namespaces. This keeps the container's
     * filesystem mount changes separate from the host's.
     */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Make rootfs a mount point. pivot_root() requires the new root
     * to be a mount point, not just an ordinary directory.
     * A mount point is a directory that is used as an access point to a mounted filesystem. In this case, rootfs is being made a mount point so that it can be used as the new root filesystem for the container.
     */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0)
    {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Make the root filesystem read-only so the container cannot
     * modify the files stored in the original rootfs image.
     * We will create separate writable areas for temporary files
     * and devices below.
     */
    if (mount(NULL, rootfs, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
    {
        fprintf(stderr, "container: remount rootfs read-only: %s\n",
                strerror(errno));
        return -1;
    }

    /*
     * Build the path to the directory that will become /tmp inside
     * the container. Mounting tmpfs here gives the container a
     * writable temporary directory even though rootfs is read-only.
     */
    snprintf(path, sizeof path, "%s/tmp", rootfs);

    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Build the path to the directory that will become /dev.
     * Mount a fresh tmpfs here instead of exposing the host's /dev,
     * which contains devices the container should not automatically access.
     */
    snprintf(path, sizeof path, "%s/dev", rootfs);

    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Add the two devices this project needs to the new /dev.
     * bind_device() makes the host's real devices available at
     * rootfs/dev/null and rootfs/dev/zero.
     */
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;

    /*
     * Build the path to the directory that will become /proc.
     * Mounting proc gives programs access to process information,
     * such as the information used by commands like ps.
     */
    snprintf(path, sizeof path, "%s/proc", rootfs);

    if (mount("proc", path, "proc", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Enter rootfs so "." refers to the directory that will become
     * the container's new root.
     * chdir changes the current working directory of the process to rootfs. This is necessary because pivot_root() requires the new root filesystem to be the current working directory.
     */
    if (chdir(rootfs) < 0)
    {
        fprintf(stderr, "container: chdir rootfs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Switch the process's root filesystem to rootfs.
     * After this succeeds, the container sees rootfs as its /.
     */
    if (syscall(SYS_pivot_root, ".", ".") < 0)
    {
        fprintf(stderr, "container: pivot_root: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Detach the old root filesystem so the container cannot keep
     * accessing the host's old filesystem through the previous root.
     */
    if (umount2(".", MNT_DETACH) < 0)
    {
        fprintf(stderr, "container: detach old root: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Set the working directory to the new root. This ensures that
     * relative paths used by later code start from the container's /.
     */
    if (chdir("/") < 0)
    {
        fprintf(stderr, "container: chdir /: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * ADDED HELPER: drop_capabilities
 * Called near the end of container_setup().
 * Removes every category of root power and prevents privilege recovery.
 *
 * why do we need to do this ?
 * Because even though the process is running as UID 0 inside the container, it is still running as an unprivileged user on the host. If we do not drop capabilities, the process could potentially regain privileges through setuid binaries or other mechanisms. Dropping capabilities ensures that the process cannot gain elevated privileges and helps maintain the isolation and security of the container.
 */
static int drop_capabilities(void)
{
    /*
     * Try to remove every capability from the bounding set.
     * The bounding set limits which capabilities a process can regain
     * through certain privilege-gaining operations.
     * CAP_LAST_CAP is the highest capability number supported by this kernel.
     */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++)
    {
        /*
         * cap is the capability number we are removing.
         * PR_CAPBSET_DROP asks Linux to remove it from the bounding set.
         * EINVAL is ignored because this capability number may not be
         * supported by the kernel. Any other error means setup failed.
         */
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL)
        {
            fprintf(stderr,
                    "container: drop capability %d from bounding set: %s\n",
                    cap, strerror(errno));
            return -1;
        }
    }

    /*
     * header describes the capability-setting request sent to Linux.
     * data holds the actual capability sets we want the process to have.
     * Two records are used because capability API version 3 stores
     * capability bits in two 32-bit groups.
     */
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];

    /*
     * Initialize everything to zero. This makes all capability masks
     * in data empty, meaning we are requesting no effective,
     * permitted, or inheritable capabilities.
     */
    memset(&header, 0, sizeof header);
    memset(data, 0, sizeof data);

    /*
     * Tell Linux which format of the capability structures we are using.
     */
    header.version = _LINUX_CAPABILITY_VERSION_3;

    /*
     * PID 0 means the capability change applies to this process itself.
     */
    header.pid = 0;

    /*
     * Ask Linux to replace this process's capability sets with the
     * empty sets in data, removing the capabilities it currently holds.
     */
    if (syscall(SYS_capset, &header, data) < 0)
    {
        fprintf(stderr, "container: clear capabilities: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Prevent this process and its future children from gaining new
     * privileges through operations such as executing a setuid program.
     * This setting cannot be turned off once enabled.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * All capability-removal steps succeeded.
     */
    return 0;
}
/*
 * REQUIRED FUNCTION: container_setup
 * Called by container_init() after the parent releases it.
 * Orders all child-side isolation; privileged setup must precede restrictions.
 */
int container_setup(struct container *c)
{
    /* Set the requested hostname without changing the host, because this is a private UTS view. */
    if (sethostname(c->hostname, strlen(c->hostname)) < 0)
    {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    /* Bring up private loopback so local clients and servers can use localhost. */
    (void)container_network();

    /* In --net mode, finish configuring the veth that the provided host code moved here. */
    if (c->net_enabled)
        (void)container_net_config(c);

    /* Build and enter the isolated filesystem before capabilities and seccomp forbid mount work. */
    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    /* Drop root-like powers only after setup, because mount and networking need them. */
    if (drop_capabilities() < 0)
        return -1;

    /* Install last because this filter deliberately blocks mount(), umount2(), and pivot_root(). */
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
    /* This socket is only a handle for network-control ioctls; it sends no packets. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* ifreq carries an interface name and request-specific data such as flags. */
    struct ifreq request;

    /* Zero unused union/padding bytes before passing the structure to the kernel. */
    memset(&request, 0, sizeof request);

    /* Select the loopback interface; leave room for a terminating NUL. */
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);

    /* Read current flags so enabling loopback does not erase unrelated kernel-set flags. */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: read lo flags: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* Mark loopback usable; otherwise localhost traffic has no active interface. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* Ask the kernel to apply the modified flags. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: bring lo up: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* The kernel retains the interface state, so this temporary control handle is no longer needed. */
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
    if (inet_pton(AF_INET, ip_text, &ipv4->sin_addr) != 1)
    {
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
    /* Create a control socket because these ioctls need a network-kernel handle, not a packet connection. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* One reusable interface request structure. */
    struct ifreq request;
    memset(&request, 0, sizeof request);

    /* Select the moved-in veth so later requests affect the container interface, not loopback. */
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);

    /* Parse c->net_ip into request.ifr_addr. */
    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;

    /* Give ceth0 an address so it can identify itself and exchange IP packets. */
    if (ioctl(sock, SIOCSIFADDR, &request) < 0)
    {
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

    /* Tell the kernel which addresses are directly reachable on this local network. */
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0)
    {
        fprintf(stderr, "container: set %s netmask: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Read existing interface flags before modifying them. */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: read %s flags: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Preserve existing bits while adding UP and RUNNING. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* Activate the veth; an addressed but down interface still cannot carry packets. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0)
    {
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

    /* Add the catch-all route so traffic beyond the local subnet goes to the host bridge gateway. */
    if (ioctl(sock, SIOCADDRT, &route) < 0)
    {
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

/* Syscall numbers differ by CPU ABI; checking architecture prevents number 165 meaning different things. */
#if defined(__x86_64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "container_seccomp() only supports x86_64 and aarch64"
#endif

/* The specification's dangerous-operation denylist, expressed as kernel syscall IDs. */
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
    /* Reserve enough BPF instructions to inspect architecture, inspect syscall, deny each entry, then allow the rest. */
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

    /* A foreign ABI could reinterpret syscall numbers and bypass the list, so reject it entirely. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* Load seccomp_data.nr, the current syscall number, for comparisons. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));

    /* Compare every attempted syscall because the filter must reject only this targeted denylist. */
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++)
    {
        /* On no match skip one instruction; on match execute the EPERM return. */
        filter[count++] = (struct sock_filter)BPF_JUMP(
            BPF_JMP | BPF_JEQ | BPF_K,
            (unsigned int)denied_syscalls[i], 0, 1);

        /* Make the matched syscall fail with errno EPERM. */
        filter[count++] = (struct sock_filter)BPF_STMT(
            BPF_RET | BPF_K,
            SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    /* Allow non-denied calls so ordinary commands can still read, write, allocate, and exit. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    /* Package the instruction count and array pointer for SYS_seccomp. */
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };

    /* no_new_privs is required here and also stops later exec from gaining privileges around the filter. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /* Install the filter through the kernel syscall interface. */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0)
    {
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
    /* Example: preserve `exit 7` so the invoking host shell receives result 7. */
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
    if (read(c->sync[0], &release_byte, 1) != 1)
    {
        fprintf(stderr, "container: parent did not release the container\n");
        return 1;
    }

    /* The release signal has arrived; the pipe is no longer needed by init. */
    close(c->sync[0]);

    /* Perform hostname, network, filesystem, capability, and seccomp setup. */
    if (container_setup(c) < 0)
        return 1;

    /* Fork the command so this process remains PID 1 to reap orphaned descendants and avoid zombie buildup. */
    pid_t command_pid = fork();
    if (command_pid < 0)
    {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }

    /* fork() returns zero only in the newly created command child. */
    if (command_pid == 0)
    {
        /* Replace this process image with the requested executable and args. */
        execvp(c->argv[0], c->argv);

        /* Successful exec never returns, so reaching here means failure. */
        fprintf(stderr, "container: exec %s: %s\n",
                c->argv[0], strerror(errno));

        /* Exit immediately; 127 conventionally means command could not execute. */
        _exit(127);
    }

    /* Remain PID 1 and reap every child because orphaned processes are adopted by PID 1 in this namespace. */
    for (;;)
    {
        /* waitpid writes an encoded reason/status into this variable. */
        int status;

        /* PID -1 means wait for any child, including an adopted orphan. */
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0)
        {
            /* A signal interrupted the wait before completion; simply retry. */
            if (errno == EINTR)
                continue;

            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }

        /* Reap other descendants for cleanup, but return only when the requested command itself exits. */
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
    /* Create the cgroup before clone so no unbounded child can run if cgroup setup fails. */
    if (container_cgroup_init(c) < 0)
        return 1;

    /* Create a synchronization pipe so the child cannot run before its host-side identity and limits exist. */
    if (pipe(c->sync) < 0)
    {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        container_cleanup(c);
        return 1;
    }

    /* clone() starts a callback rather than duplicating this call stack, so it needs a separate child stack. */
    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (stack == NULL)
    {
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
    if (child < 0)
    {
        fprintf(stderr, "container: clone: %s\n", strerror(errno));
        free(stack);
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /* Keep tracking setup instead of returning early, because the blocked child still needs pipe closure and reaping. */
    int setup_ok = (container_write_idmaps(c, child) == 0);

    /* Do not continue setup after an ID-map failure; the child must never run partly configured. */
    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    /* Provided code creates bridge/veth and moves one end into the child's netns. */
    if (c->net_enabled)
        (void)container_net_host_setup(c, child);

    /* Close the parent's read end so EOF correctly reaches the child if setup fails. */
    close(c->sync[0]);

    /* One arbitrary byte releases init only after required parent setup succeeds. */
    if (setup_ok && write(c->sync[1], "x", 1) != 1)
    {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }

    /*
     * Always close the write end. If setup failed and no byte was written, init
     * sees EOF and exits instead of remaining blocked or running half-configured.
     */
    close(c->sync[1]);

    /* Wait for the clone child so cleanup happens only after it cannot use its stack or cgroup. */
    int status;

    /* Retry if a signal interrupts waitpid before it reaps the specific child. */
    while (waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
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

    /* Remove the cgroup after its last process exits; retain the command result even if cleanup has trouble. */
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

    /*
     * rmdir removes an empty cgroup; ENOENT also means there is nothing left to
     * clean. If it is not empty, rmdir fails (normally because a process or a
     * child cgroup still remains) and this function reports the failure. In the
     * normal path, init has exited and its PID namespace's remaining processes
     * have been terminated, so the cgroup should be empty by this point.
     */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT)
    {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    return 0;
}
