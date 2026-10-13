/*
 * CONTAINER.C - STUDY COPY (comments rewritten for quiz prep, code unchanged)
 *
 * BIG PICTURE
 *   A container is just ordinary Linux processes with a restricted view of
 *   the machine. It shares the host kernel (it is NOT a virtual machine).
 *   We restrict it with four tools:
 *     1. Namespaces   -> what the process can SEE   (PIDs, files, hostname, network)
 *     2. Capabilities -> what root can DO           (drop all root powers)
 *     3. Seccomp      -> which syscalls it can CALL (block dangerous ones)
 *     4. Cgroups      -> how much it can USE        (process count, memory)
 *
 * PROCESS TREE
 *   host main()
 *     -> container_run()          host side: creates, releases, waits, cleans up
 *          -> container_init()    PID 1 inside the container: sets up, reaps
 *               -> command        PID 2 inside: becomes the requested program
 *
 * ORDER OF EVENTS
 *   Parent: cgroup -> pipe -> clone -> idmaps -> join cgroup -> (net) -> release
 *   Child (init): wait for release -> hostname -> loopback -> (net) ->
 *                 filesystem -> drop capabilities -> seccomp -> fork command -> reap
 *
 * QUIZ PRIORITY TAGS USED BELOW
 *   [HIGH]   very likely to be asked
 *   [MEDIUM] could be asked
 *   [LOW]    unlikely (spec says to do it last, or it is routine)
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

/* ======================================================================
 * container_namespaces
 * - Returns the flags clone() uses to give the child its own private views.
 * ====================================================================== */
int container_namespaces(void)
{
    /*
     * Each flag isolates one thing. They are combined with | (OR).
     *   USER: own user/group IDs. Container root (UID 0) is really our
     *         normal user on the host, so it has no real host power.
     *   PID:  own process numbering. Init is PID 1 and cannot see host processes.
     *   NS:   own mount table. Our mounts don't show up on the host.
     *   UTS:  own hostname. Changing it doesn't rename the host.
     *   NET:  own network interfaces and routes.
     */
    return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWNET;
}

/* ======================================================================
 * [HIGH] container_write_idmaps
 * - Gives the child an identity inside its new user namespace.
 *
 * - WHY the PARENT does this: a brand new user namespace has no ID mapping,
 *   and the child can't set up its own. The parent writes the map using the
 *   child's PID as the HOST sees it (not 1).
 * ====================================================================== */
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;

    char path[64];
    char mapping[64];

    /* The child's /proc entry, as seen from the host. */
    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);

    /*
     * Map format: "<ID inside> <ID outside> <how many IDs>"
     * "0 1000 1" means: container UID 0 = host UID 1000, one ID only.
     * So container "root" is just our normal user on the host.
     */
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());

    if (write_file(path, mapping) < 0)
        return -1;

    /*
     * - a GID is used to identify a group of users on a system.
     * - Each user can belong to one or more groups, and each group has a unique GID.
     * - Group IDs are used for managing permissions and access control on files
     * and directories, allowing multiple users to share access based on their
     * group membership.

     * - Write "deny" to setgroups BEFORE gid_map. WHY: without this, a
     * process could drop its supplementary groups and sidestep file
     * permissions that block certain groups. The kernel refuses to let an
     * unprivileged process write gid_map until this is denied.
     *
     * - denies the ability to call setgroups() in the child, which would allow
     * it to change its supplementary groups and potentially gain access to
     * files it shouldn't.
     */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    /* Same format as uid_map, but for group IDs. */
    /*
     * sets up the mapping for group IDs in the child namespace, so that
     * container GID 0 maps to the host's GID of the user running the container.
     */
    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    return 0;
}

/* ======================================================================
 * ADDED HELPER: write_cgroup_limit
 * - Used by container_cgroup_init() for each cgroup limit file.
 * - Converts a numeric limit to the text cgroup control files expect.
 * - Writes "max" for an unlimited negative value.
 * ====================================================================== */
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

/* ======================================================================
 * container_cgroup_init
 * - Creates the cgroup and sets limits. Runs in the parent BEFORE clone, so
 *   the child is never running without limits.
 * ====================================================================== */
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    /*
     * - A child cgroup may only use a controller if its parent turns it on.
     * - "+pids +memory" enables the process-count and memory controllers.
     */
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    /* Save the path; enter() and cleanup() need it later. */
    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s",
             c->cgroup_base, c->name);

    /*
     * - A cgroup is just a directory. Making the directory creates the cgroup.
     * - EEXIST is OK: a crashed earlier run may have left it behind, and the
     * next run still has to work.
     *
     * - It holds controls such as pids.max and memory.max.
     * - 0755 supplies ordinary directory read/traverse permissions.
     */
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST)
    {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    /* Max number of processes (this is what stops fork bombs). */
    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;

    /* Max memory. Going over this gets the process killed. */
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;

    /*
     * - Swap = 0. WHY: otherwise a process over its memory limit would be
     * pushed to swap instead of killed, so the limit wouldn't really hold.
     */
    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
        return -1;

    return 0;
}

/* ======================================================================
 * REQUIRED FUNCTION: container_cgroup_enter
 * - Used by container_run() before releasing the child.
 * - Moves container init into the limited cgroup.
 * - Every future descendant inherits that cgroup membership.
 * ====================================================================== */
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX + 32];
    char pid_text[16];

    /*
     * Build the path to the cgroup.procs file for the container's cgroup
     * directory.
     * This file is used to add processes to the cgroup.
     */
    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);

    /* The file wants the PID as text, e.g. "4312". */
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);

    /*
     * - Writing the HOST PID of init puts it in the cgroup.
     * - Every process init starts later inherits the cgroup, so the limits
     *   cover the whole container.
     *
     * - we are in the parent process here so pid already is host side version
     */
    return write_file(path, pid_text);
}

/* ======================================================================
 * bind_device (helper, used for /dev/null and /dev/zero)
 * - Makes a host device appear inside the container's /dev.
 *
 * - WHY bind mount instead of mknod: a user namespace is not allowed to
 *   create device files, so we borrow the host's real ones.
 * ====================================================================== */
static int bind_device(const char *rootfs, const char *name)
{
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
     * - A bind mount needs an existing file to mount onto.
     * - We have to create an empty one because the fresh tmpfs on /dev hides
     *   the files that make-rootfs.sh made (without this, mount fails with ENOENT).
     * - 0666 sets the file permissions to allow read and write access for all
     *   users.
     */
    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0)
    {
        fprintf(stderr, "container: create %s: %s\n",
                target_path, strerror(errno));
        return -1;
    }

    /* Only the empty placeholder file was needed, not the open descriptor. */
    close(fd);

    /*
     * Bind the real device onto the placeholder:
     * - MS_BIND makes the host device appear at target_path.
     * - After the root switch, the command sees normal /dev/null or /dev/zero.
     */
    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0)
    {
        fprintf(stderr, "container: bind %s: %s\n",
                host_path, strerror(errno));
        return -1;
    }

    return 0;
}

/* ======================================================================
 * ADDED HELPER: setup_filesystem
 * - Used by container_setup() while init still has mount capabilities.
 * - Constructs the container's private mount layout.
 * - Makes rootfs become `/` for the container.
 * - Detaches the old host root so it cannot remain reachable.
 * ====================================================================== */
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
     * STEP 1: Make mounts private.
     * - WHY: many systems mark "/" as "shared", which means mounts in one
     *   place get copied to peers.
     * - Even in a new mount namespace, our mounts could leak back to the host.
     * - PRIVATE stops that.
     * - REC applies it to every mount below "/" too.
     */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /*
     * STEP 2a: Bind-mount rootfs onto itself.
     * - WHY: pivot_root requires the new root to be a MOUNT POINT, and a
     *   normal directory is not one.
     * - Mounting it onto itself makes it one.
     */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0)
    {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * STEP 2b: Remount it read-only.
     * - WHY a second call: the first bind mount ignores the read-only flag,
     *   so read-only must be applied with MS_REMOUNT afterward.
     * - Result: the container cannot modify its own image.
     */
    if (mount(NULL, rootfs, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0)
    {
        fprintf(stderr, "container: remount rootfs read-only: %s\n",
                strerror(errno));
        return -1;
    }

    /*
     * STEP 3: Writable /tmp. temp is temp storage for the command to use.
     * - WHY it stays writable: this tmpfs is a NEW filesystem mounted on top
     *   of the read-only one.
     * - Read-only only applies to the mount underneath.
     * - tmpfs lives in memory and starts empty.
     */
    snprintf(path, sizeof path, "%s/tmp", rootfs);

    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /*
     * STEP 4: Minimal /dev.
     * - A fresh empty tmpfs hides the host's /dev (which has sensitive devices).
     * - We then add back only null and zero.
     */
    snprintf(path, sizeof path, "%s/dev", rootfs);

    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }

    /* dev/null and dev/zero binding */
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;

    /*
     * STEP 5: Create fresh /proc.
     * - It shows only container processes because init is already inside the
     *   new PID namespace.
     * - MUST come before pivot_root: the kernel only allows mounting proc
     *   while the host's /proc is still visible; afterward it fails (EPERM).
     */
    snprintf(path, sizeof path, "%s/proc", rootfs);

    if (mount("proc", path, "proc", 0, NULL) < 0)
    {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /* STEP 6a: Move into rootfs so "." names it in the next call. */
    if (chdir(rootfs) < 0)
    {
        fprintf(stderr, "container: chdir rootfs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * STEP 6b: pivot_root(".", ".") makes rootfs the new "/".
     * - THE TRICK: normally the 2nd argument is a directory INSIDE the new
     *   root where the old root gets parked.
     * - Our rootfs is read-only, so we can't create one.
     * - Passing "." for both stacks the old root on top of the new root at the
     *   same spot.
     * - No glibc wrapper exists, so we call it through syscall().
     */
    if (syscall(SYS_pivot_root, ".", ".") < 0)
    {
        fprintf(stderr, "container: pivot_root: %s\n", strerror(errno));
        return -1;
    }

    /*
     * - STEP 6c: Unmount the old root (the host's files) so the container
     *   can't reach them.
     * - MNT_DETACH = lazy unmount: it disappears right away even though we are
     *   still "inside" it (our cwd).
     */
    if (umount2(".", MNT_DETACH) < 0)
    {
        fprintf(stderr, "container: detach old root: %s\n", strerror(errno));
        return -1;
    }

    /* STEP 6d: Our old cwd is stale now. Move to the new "/". */
    if (chdir("/") < 0)
    {
        fprintf(stderr, "container: chdir /: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* ======================================================================
 * [HIGH] drop_capabilities (helper called by container_setup)
 * - Removes all of root's powers. A capability = one piece of root's power.
 * - Container root has them (inside its namespace) until we drop them here.
 * ====================================================================== */
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
         * - what is prctl? process control linux sys call for environment
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
     * STEP 2: Clear the capabilities we hold right now with capset.
     * There are more than 32 capabilities, so version 3 stores them in TWO
     * 32-bit chunks. That is why data[] has two entries.
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
    memset(data, 0, sizeof data); /* all zeros = no capabilities */

    header.version = _LINUX_CAPABILITY_VERSION_3;
    /*
     * - capset has 3 API versions.
     * - Use 3 (current): it uses two 32-bit structs because there are more than
     *   32 capabilities.
     * - v1 only covers 0-31.
     * - v2 doesnt exist
     */

    header.pid = 0; /* PID 0 applies the capability change to this calling process. */

    /*
     * - Replace this process's current capability sets with data.
     * - Because data is zeroed, the process keeps no capabilities.
     */
    if (syscall(SYS_capset, &header, data) < 0)
    {
        fprintf(stderr, "container: clear capabilities: %s\n", strerror(errno));
        return -1;
    }

    /*
     * STEP 3: NO_NEW_PRIVS. Running a setuid program (or one with file
     * capabilities) can no longer raise privileges. Once set, it can never
     * be turned off. Seccomp also requires it (set again there on purpose).
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/* ======================================================================
 * container_setup
 * - All the child-side setup, in an order that matters.
 * - Runs inside init after the parent releases it.
 * ====================================================================== */
int container_setup(struct container *c)
{
    /*
     * Set the requested hostname without changing the host, because we made it
     * a private UTS view.
     */
    if (sethostname(c->hostname, strlen(c->hostname)) < 0)
    {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    /* Needs root powers, so it must come before capabilities are dropped. */
    /* Bring up private loopback so local clients and servers can use localhost. */
    (void)container_network();

    /* Same reason. Only used with --net. */
    /* In --net mode, finish configuring the veth that the provided host code moved here. */
    if (c->net_enabled)
        (void)container_net_config(c);

    /*
     * Needs mount powers, so it must also come before capabilities are dropped.
     * Build and enter isolated fs
     */
    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    /* Drop powers only AFTER everything above that needed them. */
    if (drop_capabilities() < 0)
        return -1;

    /*
     * Seccomp goes LAST. WHY: its filter blocks mount, umount2, and
     * pivot_root, which setup_filesystem uses. Installing it earlier would
     * break our own setup.
     */
    if (container_seccomp() < 0)
        return -1;

    return 0;
}

/* ======================================================================
 * container_network
 * - Turns on loopback ("lo"), which is what "localhost" uses.
 * - A new network namespace starts with only a switched-off loopback.
 * ====================================================================== */
int container_network(void)
{
    /*
     * - Open a socket.
     * - It's only a way to talk to the kernel.
     * - We never actually send data.
     */
    /* AF_INET = IPv4, SOCK_DGRAM = datagram (UDP), 0 = default protocol */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* "Form" that holds the interface name and its settings. */
    struct ifreq request;
    memset(&request, 0, sizeof request);           /* blank the form */
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1); /* interface is "lo" (localhost) */
    /*
     * IFNAMSIZ is the maximum length of an interface name, including the null
     * terminator.
     */

    /* Ask the kernel for lo's current settings (the flags). */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: read lo flags: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* Turn on UP and RUNNING without changing the other flags. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* Send the updated flags back so the kernel applies them. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: bring lo up: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /*
     * The kernel retains the interface state, so this temporary control handle
     * is no longer needed.
     */
    close(sock);
    return 0;
}

/* ======================================================================
 * make_ipv4_address (helper for --net)
 * - Converts text like "10.44.0.2" into the binary form the kernel wants.
 * ====================================================================== */
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

/* ======================================================================
 * container_net_config (only used with --net; spec says do last)
 * - Configures the container's end of the host-to-container link (ceth0):
 *   host (10.44.0.1) <-----> ceth0 (10.44.0.2) inside the container
 * - Steps: set address, set netmask, switch on, add default route.
 * ====================================================================== */
int container_net_config(struct container *c)
{
    /*
     * - Layout: host (10.44.0.1) <----> ceth0 (10.44.0.2) inside the container.
     * - The provided host code already put ceth0 inside this container, but it
     *   has no address and is switched off.
     * - This function sets it up: address, netmask, switch on, default route.
     */

    /* A socket is only a way to talk to the kernel. We never send data on it. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* "Form" that holds the interface name and its settings. */
    struct ifreq request;
    memset(&request, 0, sizeof request); /* blank the form */

    /* Name the interface to configure: ceth0 (not loopback). */
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);

    /* STEP 1: ADDRESS. Convert the text "10.44.0.2" into binary form. */
    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;

    /* Send the address to the kernel. */
    if (ioctl(sock, SIOCSIFADDR, &request) < 0)
    {
        fprintf(stderr, "container: set %s address: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /*
     * STEP 2: NETMASK. This says which addresses are "nearby" (on the
     * same local network).
     * - We reuse the same form.
     * - the netmask field shares memory with the address field, which is fine
     *   because the address was already sent.
     */
    struct sockaddr_in *netmask =
        (struct sockaddr_in *)&request.ifr_netmask;
    memset(netmask, 0, sizeof *netmask);
    netmask->sin_family = AF_INET;

    /*
     * Turn a prefix like /24 into a mask like 255.255.255.0:
     * - start with 32 one-bits and shift left, so the last (32 - prefix) bits
     *   become zeros.
     * - The "> 0" check avoids shifting by 32, which is undefined in C
     *   (prefix 0 means mask 0.0.0.0).
     */
    uint32_t mask_bits = 0;
    if (c->net_prefix > 0)
        mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);

    /* htonl converts to the byte order used on the network. */
    netmask->sin_addr.s_addr = htonl(mask_bits);

    /* Send the netmask to the kernel. */
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0)
    {
        fprintf(stderr, "container: set %s netmask: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* STEP 3: SWITCH ON. Read the current flags first so we keep the others. */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: read %s flags: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* Turn on UP and RUNNING. An interface that is off carries no packets. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* Send the new flags back. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0)
    {
        fprintf(stderr, "container: bring %s up: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* STEP 4: DEFAULT ROUTE. A route entry says where to send traffic. */
    struct rtentry route;
    memset(&route, 0, sizeof route);

    /*
     * - Destination 0.0.0.0 + mask 0.0.0.0 matches EVERY address, so this
     *   means "anything not on the local network goes through the gateway".
     * - The gateway is the host (10.44.0.1).
     * - Without this route, the container could only reply to 10.44.0.x
     *   addresses.
     */
    if (make_ipv4_address(&route.rt_dst, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_genmask, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_gateway, c->net_gw) < 0)
        goto fail;

    /* UP = route is active. GATEWAY = send via a gateway, not directly. */
    route.rt_flags = RTF_UP | RTF_GATEWAY;

    /* Add the route to the kernel's routing table. */
    if (ioctl(sock, SIOCADDRT, &route) < 0)
    {
        fprintf(stderr, "container: add default route via %s: %s\n",
                c->net_gw, strerror(errno));
        goto fail;
    }

    /* The kernel keeps these settings, so the socket is no longer needed. */
    close(sock);
    return 0;

fail:
    /* Every error above jumps here so the socket is closed in one place. */
    close(sock);
    return -1;
}

/*
 * - Syscall numbers are different on different CPU types.
 * - We record which one we are built for so the filter can check it
 *   (see container_seccomp).
 */
#if defined(__x86_64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "container_seccomp() only supports x86_64 and aarch64"
#endif

/* The syscalls the spec says to block. */
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

/* ======================================================================
 * container_seccomp
 * - Installs a filter the kernel runs on EVERY syscall.
 * - Blocked syscalls fail with EPERM.
 * - Everything else is allowed (a "denylist").
 *
 * - Capabilities vs seccomp: dropping capabilities stops root from doing
 *   privileged things.
 * - seccomp blocks chosen syscalls entirely, for any user.
 *
 * - The filter is a tiny program, laid out like this:
 *   1. load arch          -> if wrong architecture, kill the process
 *   2. load syscall number
 *   3. for each denied syscall: if match, return EPERM
 *   4. otherwise, allow
 * ====================================================================== */
int container_seccomp(void)
{
    /* Instruction list the kernel will run on every syscall. We only BUILD it here. */
    struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1];
    size_t count = 0; /* next free slot in the list */

    /*
     * - Step 1: look at the CPU type, and kill the process if it isn't ours.
     * - (Other CPU modes number syscalls differently and could bypass the list.)
     */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, arch));
    filter[count++] = (struct sock_filter)BPF_JUMP(
        BPF_JMP | BPF_JEQ | BPF_K, OUR_AUDIT_ARCH, 1, 0);
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* Step 2: look at the syscall number. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));

    /* For each banned syscall: if the number matches, fail it with EPERM. */
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++)
    {
        filter[count++] = (struct sock_filter)BPF_JUMP(
            BPF_JMP | BPF_JEQ | BPF_K,
            (unsigned int)denied_syscalls[i], 0, 1);
        filter[count++] = (struct sock_filter)BPF_STMT(
            BPF_RET | BPF_K,
            SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    /* Step 3: not banned, so allow it. */
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    /* Package the list (pointer + length) for the kernel. */
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };

    /* Required before an unprivileged process may install a filter. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /*
     * - Give the list to the kernel.
     * - It now runs on every syscall, and the command inherits it.
     */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0)
    {
        fprintf(stderr, "container: install seccomp filter: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}

/* ======================================================================
 * status_to_exit_code (helper)
 * - Turns the raw waitpid status into a normal exit code.
 * ====================================================================== */
static int status_to_exit_code(int status)
{
    /* Exited normally: use its exit code. */
    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    /* Killed by a signal: shell convention is 128 + signal number. */
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}

/* ======================================================================
 * container_init
 * - Runs as PID 1 inside the container. Jobs:
 *   1. wait for the parent to finish its setup
 *   2. finish container setup
 *   3. start the command
 *   4. stay alive to reap zombies until the command exits
 * ====================================================================== */
int container_init(struct container *c)
{
    char release_byte;

    /*
     * - clone gave us our own copy of the pipe's file descriptors.
     * - We only read, so close our write end.
     * - If we kept it open, read() below would never see end-of-file when the
     *   parent fails and closes ITS write end.
     */
    close(c->sync[1]);

    /* reminder:
     * Synchronization order:
     * - Parent writes UID/GID maps.
     * - Parent adds init to the cgroup.
     * - Parent optionally prepares host networking.
     * - Parent writes one byte.
     * - Until that byte arrives, init cannot run partly configured.
     */
    /*
     * - Block until the parent writes one byte.
     * - That byte means "ID maps and cgroup are done, you may continue."
     * - The value doesn't matter.
     * - If the parent fails it closes the pipe without writing, we get EOF,
     *   and exit.
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
    /*
     * - WHY fork instead of exec directly: PID 1 must stay alive.
     * - When any process's parent dies, the kernel hands it to PID 1, and PID 1
     *   must collect it.
     * - If init became the command, nobody would reap orphans.
     */
    pid_t command_pid = fork();
    if (command_pid < 0)
    {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }

    /* fork returns 0 only in the new child. */
    if (command_pid == 0)
    {
        /* Replace this process with the requested program (it becomes PID 2). */
        execvp(c->argv[0], c->argv);

        /* Only reached if exec failed. */
        fprintf(stderr, "container: exec %s: %s\n",
                c->argv[0], strerror(errno));

        /*
         * - _exit, not exit: exit would flush copied stdio buffers and could
         *   print duplicate output.
         * - 127 = "command could not be run".
         */
        _exit(127);
    }

    /*
     * - Container init must collect the exit status of its child processes.
     *
     * - When a process exits, Linux keeps a small record of it until its parent
     *   collects its exit status with waitpid().
     * - Until then, it is called a zombie.
     *
     * - Keep waiting for children until the MAIN COMMAND exits.
     * - Other children may exit first, and init must collect their statuses too.
     */
    for (;;)
    {
        int status;

        /*
         * - Wait for any child process, not just the main command.
         * - -1 means "any child".
         * - &status receives information about how the child finished.
         * - 0 means wait rather than returning immediately if no child has
         *   exited yet.
         *
         * - Returns the PID of the child whose status was collected, or -1 if
         *   an error occurred.
         */
        pid_t reaped = waitpid(-1, &status, 0);

        if (reaped < 0)
        {
            /*
             * - A signal interrupted waitpid().
             * - This does not necessarily mean a child failed, so try waiting
             *   again.
             */
            if (errno == EINTR)
                continue;

            /* An unexpected waitpid() error occurred. */
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }

        /*
         * - We successfully collected this child's exit status.
         *
         * - If it was the main command, the container's main workload has
         *   finished.
         *
         * - If it was some other child, its status has been collected, so loop
         *   again to wait for the remaining children.
         */
        if (reaped == command_pid)
            return status_to_exit_code(status);
    }
}

/* ======================================================================
 * child_entry (helper)
 * - clone() needs a function taking void*.
 * - This just converts the pointer back to struct container* and calls
 *   container_init.
 * ====================================================================== */
static int child_entry(void *arg)
{
    return container_init((struct container *)arg);
}

/* ======================================================================
 * container_run
 * - The parent: creates the container, releases it, waits, cleans up.
 * ====================================================================== */
int container_run(struct container *c)
{
    /* Cgroup first, so the child can never run without limits. */
    if (container_cgroup_init(c) < 0)
        return 1;

    /*
     * - The pipe is how the parent tells the child "setup is done."
     * - The child blocks reading it, so it can't run half-configured.
     */
    if (pipe(c->sync) < 0)
    {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        container_cleanup(c);
        return 1;
    }

    /*
     * - clone runs a function (it doesn't continue from this spot like fork).
     * - so it needs its own stack for that function.
     */
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
     * clone arguments:
     *   stack + SIZE : the TOP of the stack, because stacks grow DOWNWARD
     *   namespaces   : which private views to create
     *   SIGCHLD      : so the parent is notified on exit and waitpid works
     *   c            : passed to child_entry
     * No CLONE_VM, so the child gets a copy of memory, not shared memory.
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

    /*
     * - The child is now blocked waiting on the pipe.
     * - Do the parent-side setup.
     * - Track success instead of returning early, because we still must close
     *   the pipe and reap the child.
     */
    int setup_ok = (container_write_idmaps(c, child) == 0);

    /* Skipped automatically if the ID maps already failed. */
    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    /* Provided code: builds the host side of the network link (--net only). */
    if (c->net_enabled)
        (void)container_net_host_setup(c, child);

    /* The parent never reads from the pipe, so close its unused read end. */
    close(c->sync[0]);

    /* One byte releases the child, but only if setup succeeded. */
    if (setup_ok && write(c->sync[1], "x", 1) != 1)
    {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }

    /*
     * - Always close the write end.
     * - On success the child already got its byte.
     * - On failure no byte was written, so the child sees EOF and exits instead
     *   of waiting forever.
     */
    close(c->sync[1]);

    int status;

    /* reaping init itself not the children */
    /* Wait for init to exit. Retry if a signal interrupts the wait. */
    while (waitpid(child, &status, 0) < 0)
    {
        if (errno != EINTR)
        {
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            free(stack);
            return 1;
        }
    }

    /* Only safe to free the stack once the child is gone. */
    free(stack);

    /* Provided code: removes the host side of the network link. */
    if (c->net_enabled)
        (void)container_net_host_teardown(c);

    /* Cleanup errors are ignored so we still return the command's result. */
    (void)container_cleanup(c);

    if (!setup_ok)
        return 1;

    return status_to_exit_code(status);
}

/* ======================================================================
 * container_cleanup
 * - Deletes the cgroup directory after the container has exited.
 * ====================================================================== */
int container_cleanup(struct container *c)
{
    /* No path saved means no cgroup was created. */
    if (c->cg_path[0] == '\0')
        return 0;

    /*
     * - Use rmdir (cgroups are directories, removed with rmdir, not unlink).
     * - It only works if the cgroup is empty; with processes still inside it
     *   fails with EBUSY.
     * - ENOENT means it's already gone, which is fine.
     * - The mounts from setup need no cleanup: they vanish by themselves when
     *   the last process in the mount namespace exits.
     */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT)
    {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    return 0;
}
