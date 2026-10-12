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
 * - Used by container_run() when clone() creates container init.
 * - Returns one bitmask selecting five isolated resource views.
 */
int container_namespaces(void)
{
    /*
     * Bitwise OR combines every flag; it does not select only one.
     * - USER: separate identity view. UID 0 inside still maps to the ordinary
     *   account that launched the container on the host.
     * - PID: separate process tree. Init becomes PID 1; unrelated host
     *   processes do not appear in the container's process list.
     * - NEWNS: separate mount table. Changes to /proc, /tmp, and the root
     *   filesystem affect the container's view, not the host's view.
     * - UTS: separate hostname. Naming the container does not rename the host.
     * - NET: separate interfaces, addresses, and routes for --net mode.
     */
    return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET;
}

/*
 * REQUIRED FUNCTION: container_write_idmaps
 * - Used by the host parent after clone() and before releasing the child.
 * - Maps container-local UID/GID 0 to the caller's ordinary host IDs.
 * - "Inside": identity seen through the child's new user namespace.
 * - "Host": identity used against the real machine's users and files.
 * - The ID map connects those two number spaces.
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
     * uid_map format:
     *   <container starting UID> <host starting UID> <number of IDs>
     * Example when getuid() returns 1000:
     *   "0 1000 1"
     *   - Container UID 0 maps to host UID 1000.
     *   - One ID is mapped.
     *   - UID 0 is container root, not host root.
     */
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());

    /* Provided write_file() writes the mapping; propagate any failure. */
    if (write_file(path, mapping) < 0)
        return -1;

    /*
     * Why write "deny" before gid_map?
     * - Linux requires it for an unprivileged gid_map write.
     * - Supplementary groups may carry extra file permissions.
     * - Denying setgroups prevents the child from retaining or adding them.
     */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    /*
     * gid_map uses the same three-number format.
     * Example when getgid() returns 1000:
     *   "0 1000 1" maps container GID 0 to host GID 1000.
     * Result:
     *   Group permission checks remain as restricted as the UID mapping.
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
 * - Used by container_cgroup_init() for each cgroup limit file.
 * - Converts a numeric limit to the text cgroup control files expect.
 * - Writes "max" for an unlimited negative value.
 */
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
{
    /* PATH_MAX plus extra space for the slash and cgroup filename. */
    char path[PATH_MAX + 32];

    /* Enough text for a long integer or the word "max". */
    char value[32];

    /*
     * Build the control-file path:
     * - c->cg_path was saved by container_cgroup_init().
     * - Example directory: /sys/fs/cgroup/demo
     * - Example file: memory.max
     * - Result: /sys/fs/cgroup/demo/memory.max
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
 * - Called first by container_run() in the host parent.
 * - Creates the container's cgroup directory.
 * - Applies process, memory, and swap limits.
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
     * Save the cgroup directory in c->cg_path.
     * - Base: /sys/fs/cgroup
     * - Container name: demo
     * - Saved result: /sys/fs/cgroup/demo
     * - container_cgroup_enter() later appends /cgroup.procs.
     * - container_cleanup() later removes this directory.
     */
    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s",
             c->cgroup_base, c->name);

    /*
     * Create the saved directory.
     * - It holds controls such as pids.max and memory.max.
     * - EEXIST is allowed because a crash may leave the directory behind.
     * - 0755 supplies ordinary directory read/traverse permissions.
     * - 0755 does not define resource limits; the control files do.
     * - The kernel manages permissions on those control files separately.
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
 * - Used by container_run() before releasing the child.
 * - Moves container init into the limited cgroup.
 * - Every future descendant inherits that cgroup membership.
 */
int container_cgroup_enter(struct container *c, pid_t child)
{
    /* Buffers for the membership control path and decimal PID text. */
    char path[PATH_MAX + 32];
    char pid_text[16];

    /* cgroup.procs is the cgroup-v2 membership control file. */
    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);

    /*
     * Convert the PID to the text cgroup.procs expects.
     * - In memory: binary pid_t value 4312
     * - Written value: the four characters "4312"
     */
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);

    /*
     * Effect of writing the host PID:
     * - Init is the clone() child and PID 1 in the container's PID view.
     * - Writing 4312 to /sys/fs/cgroup/demo/cgroup.procs places init in demo.
     * - Commands init later forks inherit demo's limits.
     */

    return write_file(path, pid_text);
}

/*
 * ADDED HELPER: bind_device
 * - Used twice by setup_filesystem(): once for null and once for zero.
 * - Creates a target below the future rootfs.
 * - Bind-mounts the matching host device onto that target.
 * - /dev/null discards output written to it.
 * - /dev/zero supplies an endless stream of zero bytes.
 */
static int bind_device(const char *rootfs, const char *name)
{
    /* Host source and future-container destination paths. */
    char host_path[PATH_MAX];
    char target_path[PATH_MAX];

    /*
     * Build both sides of the bind mount for name "null":
     * - Source: host /dev/null
     * - Target now: <rootfs>/dev/null
     * - Target after pivot_root(): container /dev/null
     */
    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    /*
     * Prepare the target:
     * - A bind mount requires an existing destination file.
     * - O_CREAT creates that empty placeholder.
     * - O_CLOEXEC prevents this descriptor from leaking through exec.
     * 0666 sets the file permissions to allow read and write access for all users.
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
     * Bind the real device onto the placeholder:
     * - MS_BIND makes the host device appear at target_path.
     * - After the root switch, the command sees normal /dev/null or /dev/zero.
     * - No new device node is created; this user namespace cannot use mknod().
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
 * - Used by container_setup() while init still has mount capabilities.
 * - Constructs the container's private mount layout.
 * - Makes rootfs become `/` for the container.
 * - Detaches the old host root so it cannot remain reachable.
 */
static int setup_filesystem(const char *rootfs)
{
    /*
     * Temporary path buffer.
     * Example rootfs: /images/miniroot
     * Possible values:
     * - /images/miniroot/tmp
     * - /images/miniroot/dev
     * - /images/miniroot/proc
     */
    char path[PATH_MAX];

    /*
     * Isolate mount propagation:
     * - MS_PRIVATE stops container mount changes from reaching peer mounts.
     * - MS_REC applies that rule to `/` and every mount below it.
     * - Result: container mount/unmount operations do not alter the host view.
     */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Make rootfs eligible to become `/`:
     * - pivot_root() requires a mount point, not an ordinary directory.
     * - A mount point is the path where a mounted filesystem is accessed.
     * - Bind-mounting rootfs onto itself turns it into that required mount point.
     */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0)
    {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Protect the rootfs image:
     * - Remount it read-only.
     * - The command cannot change supplied programs or configuration.
     * - Separate writable tmpfs mounts are added for temporary files/devices.
     */
    if (mount(NULL, rootfs, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
    {
        fprintf(stderr, "container: remount rootfs read-only: %s\n",
                strerror(errno));
        return -1;
    }

    /*
     * Prepare writable temporary storage:
     * - <rootfs>/tmp becomes /tmp after pivot_root().
     * - Programs use /tmp for short-lived scratch files.
     * - tmpfs is memory-backed and remains writable while rootfs is read-only.
     */
    snprintf(path, sizeof path, "%s/tmp", rootfs);

    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Prepare a minimal device directory:
     * - <rootfs>/dev becomes /dev after pivot_root().
     * - A fresh tmpfs begins empty and writable.
     * - The host's full /dev stays hidden because it contains sensitive devices.
     */
    snprintf(path, sizeof path, "%s/dev", rootfs);

    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Add only the two required devices:
     * - bind_device(..., "null") exposes /dev/null.
     * - bind_device(..., "zero") exposes /dev/zero.
     */
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;

    /*
     * Prepare the process-information filesystem:
     * - <rootfs>/proc becomes /proc after pivot_root().
     * - A fresh proc mount reflects this PID namespace.
     * - Commands such as ps read process information from it.
     */
    snprintf(path, sizeof path, "%s/proc", rootfs);

    if (mount("proc", path, "proc", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Make rootfs the current directory:
     * - After chdir(rootfs), `.` refers to the prepared rootfs mount.
     * - pivot_root() below uses `.` as its new-root argument.
     */
    if (chdir(rootfs) < 0)
    {
        fprintf(stderr, "container: chdir rootfs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Switch roots:
     * - rootfs becomes `/` for this process.
     * - The container now resolves absolute paths inside the supplied image.
     */
    if (syscall(SYS_pivot_root, ".", ".") < 0)
    {
        fprintf(stderr, "container: pivot_root: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Remove the old-root escape path:
     * - pivot_root() temporarily leaves the old root attached.
     * - MNT_DETACH disconnects it.
     * - The container can no longer reach host files through the former root.
     */
    if (umount2(".", MNT_DETACH) < 0)
    {
        fprintf(stderr, "container: detach old root: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Finish in the new root:
     * - Set the working directory to `/`.
     * - Later relative paths begin inside the container filesystem.
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
 * - Used near the end of container_setup().
 * - Removes every category of root-like power.
 * - Prevents the command from recovering privilege later.
 * - Container UID 0 should not retain powers it needed only during setup.
 * - setuid programs and file capabilities must not restore those powers.
 */
static int drop_capabilities(void)
{
    /*
     * Remove every capability from the bounding set:
     * - The bounding set limits capabilities that can be regained later.
     * - CAP_LAST_CAP is the highest capability number known to these headers.
     */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++)
    {
        /*
         * For each cap number:
         * - PR_CAPBSET_DROP removes it from the bounding set.
         * - EINVAL means this kernel does not support that number; ignore it.
         * - Any other error means setup failed.
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
     * Capability API data:
     * - header describes the request sent to Linux.
     * - data holds the requested capability sets.
     * - API version 3 uses two records for two 32-bit groups.
     */
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];

    /*
     * Zero both structures:
     * - Every mask in data becomes empty.
     * - Requested effective capabilities: none.
     * - Requested permitted capabilities: none.
     * - Requested inheritable capabilities: none.
     */
    memset(&header, 0, sizeof header);
    memset(data, 0, sizeof data);

    /* Select capability API version 3, matching the two-record data layout. */
    header.version = _LINUX_CAPABILITY_VERSION_3;

    /* PID 0 applies the capability change to this calling process. */
    header.pid = 0;

    /*
     * Apply the empty sets:
     * - Replace this process's current capability sets with data.
     * - Because data is zeroed, the process keeps no capabilities.
     */
    if (syscall(SYS_capset, &header, data) < 0)
    {
        fprintf(stderr, "container: clear capabilities: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Prevent future privilege gain:
     * - Applies to this process and future children.
     * - Blocks privilege gain through setuid or file-capability executables.
     * - Cannot be turned off once enabled.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /* All capability-removal steps succeeded. */
    return 0;
}
/*
 * REQUIRED FUNCTION: container_setup
 * - Used by container_init() after the host parent releases it.
 * - Performs every child-side setup step in the required order.
 * - Privileged setup must finish before capabilities/seccomp remove access.
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
 * - Used by container_setup() before CAP_NET_ADMIN is removed.
 * - Activates `lo`, the container's private loopback interface.
 * - Makes localhost communication possible inside the container.
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
 * - Used by container_net_config().
 * - Converts readable IPv4 text such as 10.44.0.2.
 * - Stores the result in the binary sockaddr format the kernel expects.
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
 * - Used by container_setup() only in --net mode.
 * - Configures the container end of the provided veth pair.
 * - Assigns its address, netmask, active state, and default route.
 */
int container_net_config(struct container *c)
{
    /*
     * Example --net layout:
     * - Host bridge/gateway: 10.44.0.1
     * - Container interface ceth0: 10.44.0.2
     * - Provided host setup already moved ceth0 into this namespace.
     * - This function addresses it, activates it, and adds its route.
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
     * Build a default route:
     * - Destination 0.0.0.0: start of the all-address range.
     * - Mask 0.0.0.0: match every destination.
     * - Gateway: host bridge, normally 10.44.0.1.
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
 * - Called last by container_setup().
 * - Installs a kernel BPF syscall filter.
 * - Dangerous calls fail with EPERM; unmatched ordinary calls are allowed.
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
 * - Used after waitpid() by container_init() and container_run().
 * - Converts waitpid's encoded status to a shell-style result.
 * - Normal exit: return its 0-255 code.
 * - Signal death: return 128 plus the signal number.
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
 * - Runs as PID 1 inside the new PID namespace.
 * - Waits until host-parent setup is complete.
 * - Finishes child-side isolation.
 * - Launches the requested command and reaps descendants.
 */
int container_init(struct container *c)
{
    /* Only the arrival of one byte matters; its stored value is irrelevant. */
    char release_byte;

    /* Init reads from the pipe, so close its unused write-end copy. */
    close(c->sync[1]);

    /*
     * Synchronization order:
     * - Parent writes UID/GID maps.
     * - Parent adds init to the cgroup.
     * - Parent optionally prepares host networking.
     * - Parent writes one byte.
     * - Until that byte arrives, init cannot run partly configured.
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
 * - Passed to clone() by container_run().
 * - clone() requires an int(void *) callback.
 * - Converts that generic pointer back to struct container *.
 * - Forwards execution and the exit code to/from container_init().
 */
static int child_entry(void *arg)
{
    /* Recover the struct container pointer and forward init's exit code. */
    return container_init((struct container *)arg);
}

/*
 * REQUIRED FUNCTION: container_run
 * - The only container.c function called directly by provided main.c.
 * - Runs as the host-side parent/coordinator.
 * - Creates, releases, waits for, and cleans up the container.
 * - Returns the requested command's exit code.
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
     * clone() arguments:
     * - stack + CONTAINER_STACK_SIZE: top of the downward-growing child stack.
     * - container_namespaces(): resource views to isolate.
     * - SIGCHLD: notify the parent so normal waitpid() works.
     * - c: passed to child_entry() as its void * argument.
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
     * Always close the parent's write end:
     * - Success: the child already received its release byte.
     * - Failure: no byte was written, so the child reads EOF.
     * - EOF makes init exit instead of blocking or running half-configured.
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
 * - Used by container_run() after init is reaped and on early failures.
 * - Removes the per-container cgroup directory.
 * - The directory must already be empty.
 */
int container_cleanup(struct container *c)
{
    /* An empty first character means no cgroup path was stored. */
    if (c->cg_path[0] == '\0')
        return 0;

    /*
     * Cleanup outcomes:
     * - Empty cgroup: rmdir removes it.
     * - ENOENT: it is already absent, so cleanup is complete.
     * - Nonempty cgroup: rmdir fails because a process/child cgroup remains.
     * - Normal path: init and its PID namespace are gone, so it should be empty.
     */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT)
    {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    return 0;
}
