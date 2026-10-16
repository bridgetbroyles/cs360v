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
 * PROCESS ARCHITECTURE
 *
 *   Host operating system
 *   |
 *   +-- Host launcher / host parent
 *       The original process running main() and container_run().
 *       It calls clone() and remains outside the container namespaces.
 *       |
 *       +-- Container init
 *           The process created by clone(). It is a child of the launcher
 *           on the host, but appears as PID 1 inside the container.
 *           This process runs container_init() and stays alive as the reaper.
 *           |
 *           +-- Requested command
 *               Created by fork() and then replaced by execvp().
 *               It is normally PID 2 inside the container.
 *
 *   "Host" means the real operating system outside the container. "Parent"
 *   describes a process relationship: the launcher is the parent of container
 *   init, and container init is the parent of the requested command.
 *
 *   There is no PID 0 child. fork() returns 0 in the new child to identify the
 *   child branch; that child's actual PID is normally 2 inside the container.
 *
 * CONTAINER_RUN() VERSUS CONTAINER_SETUP()
 *
 *   container_run() runs in the host launcher, outside the container. It
 *   controls the entire container lifetime: create the cgroup and pipe, call
 *   clone(), write UID/GID maps, enter the cgroup, optionally create host-side
 *   networking, release container init, wait for it, and clean up afterward.
 *
 *   container_setup() runs in container init, inside the new namespaces. It
 *   prepares the environment processes will see: set the hostname, configure
 *   inside networking, switch to the container filesystem, drop capabilities,
 *   and install the seccomp filter.
 *
 *   container_init() connects them. It waits for container_run() to finish
 *   the outside setup, calls container_setup() for the inside setup, forks the
 *   requested command, and remains PID 1 to reap children.
 *
 * ORDER OF EVENTS
 *   Parent: cgroup -> pipe -> clone -> idmaps -> join cgroup -> (net) -> release
 *   Child (init): wait for release -> hostname -> loopback -> (net) ->
 *                 filesystem -> drop capabilities -> seccomp -> fork command -> reap
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


/* The namespaces the container gets its own copy of. container_run() calls
 * this when it passes the namespace flags to clone(). */
int container_namespaces(void)
{
    /* Return one clone flags value containing all five requested namespaces.
     * clone() will use it to give the child private user IDs, process IDs, mounts, hostname, and networking.
     * */
    return CLONE_NEWUSER /* A process can appear to be UID 0 (root) inside the container while mapping to the ordinary user's UID on the host. */
         | CLONE_NEWPID /* separate process tree. Init becomes PID 1; unrelated host */
         | CLONE_NEWNS /* separate mount table. Changes to /proc, /tmp, and the root */
         | CLONE_NEWUTS /* separate hostname. Naming the container does not rename the host. */
         | CLONE_NEWNET; /* separate interfaces, addresses, and routes for --net mode. */
}

/* Runs in the parent. maps root inside the container onto the real uid/gid
 * outside it. */
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;
    char path[64];
    char mapping[64];

    /* find our path to the uid_map file for the child process */
    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);

    /* map the container's root uid (0) to the host's uid of the user running the container */
    /* uid 0 provides the container with root privileges inside the container, but it is still our normal user on the host */
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
    if (write_file(path, mapping) < 0) 
        return -1;

    /* The kernel won't accept gid_map until setgroups is denied. */
    /* why deny? setgroups could allow the process to change other groups it is in and gain access to files it shouldn't have */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    /* get the path to the gid_map file for the child process */
    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);

    /* map the container's root gid (0) to the host's gid of the user running the container */
    /* gid 0 provides the container with root privileges inside the container, but it is still our normal group on the host */
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    return 0;
}

/* ---- Part V: cgroup --------------------------------------------------- */

/* Writes one limit into a file in the container's cgroup. A negative limit
 * means "no limit" and is written as "max". container_cgroup_init() calls it
 * once for each resource limit. */
static int write_cgroup_limit(struct container *c, const char *file_name, long limit)
{
    char path[PATH_MAX + 32];
    char value[32];
    /* given our saved cg_path, add the file name to it */
    snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);
    /* if limit < 0 it means max, otherwise write the limit as a string */
    if (limit < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", limit); /* convert into string */
    return write_file(path, value);
}

/* SPEC Part V: creates the container's cgroup and sets its limits. Runs in
 * the parent, before the container starts. */
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    /* configure path and turn on controllers for process counter and memory */
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    /* save the cgroup path for enter, cleanup and writing limits */
    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s", c->cgroup_base, c->name);

    /* make cgroup directory, if it already exists that is fine (from a crash) 0755 is read/traverse perms */
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0) /* limit the number of processes in the container */
        return -1;
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0) /* limit the amount of memory the container can use */
        return -1;
    /* disable swap so that the container cannot use swap space to exceed its memory limit */
    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0) 
        return -1;

    return 0;
}

/* SPEC Part V: moves the container's init into the cgroup. Runs in the parent
 * before the child is released, so the command never runs unlimited. */
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX + 32];
    char pid_text[16];

    /* parent write the host pid of the child to the cgroup.procs file, which puts it in the cgroup, and every process init starts later inherits the cgroup, so the limits cover the whole container */
    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
    snprintf(pid_text, sizeof pid_text, "%d", (int)child); /* convert the pid to a string */
    return write_file(path, pid_text);
}

/* ---- Parts I/II/III: isolation, run inside the container init --------- */

/* Bind-mounts the host's /dev/<name> at <rootfs>/dev/<name>. mknod() isn't
 * allowed in a user namespace, so we bind the host's node instead.
 * setup_filesystem() calls it for /dev/null and /dev/zero. */
static int bind_device(const char *rootfs, const char *name)
{
    char host_path[PATH_MAX];
    char target_path[PATH_MAX];
    /* get host path to the host's devices */
    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    /* get path to where the device will be in the container */
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    /* A bind mount needs an existing file to mount over. */
    /* so we open the target path with O_CREAT to create an empty file, O_WRONLY to open for writing, and O_CLOEXEC to close on exec. 0666 sets the file permissions to allow read and write access for all users. */
    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "container: create %s: %s\n", target_path, strerror(errno));
        return -1;
    }
    close(fd); /* close the file descriptor, we only needed to create the file */

    /* bind mount the host device onto the empty file so it appears in the container. now container thinks it has the device, but it is really the host's device. */
    /* the container cannot create new devices because it is in a user namespace. */
    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0) {
        fprintf(stderr, "container: bind %s: %s\n", host_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* builds the container's filesystem and pivots into it, so that
 * `rootfs` becomes "/". The order of the steps matters. container_setup()
 * calls it before capabilities are dropped. */
static int setup_filesystem(const char *rootfs)
{
    char path[PATH_MAX];

    /* make mounts private to stop them from propagating back to the host. */
    /* new mount space doesn't protect leaks from shared mounts. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /* pivot_root() needs a mount point, so bind the rootfs onto itself, makes it a mount point */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }

    /* remount the rootfs read-only so that the container cannot modify its own image.  */
    if (mount(NULL, rootfs, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        fprintf(stderr, "container: remount rootfs read-only: %s\n", strerror(errno));
        return -1;
    }

    /* mount a tmpfs on top of the read-only root so that the container can write to /tmp. tmpfs is a temporary filesystem that lives in memory and is writable, while the underlying rootfs is read-only. */
    /* tmpfs is used for temporary files that do not need to be persisted, */
    snprintf(path, sizeof path, "%s/tmp", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /* the new tmpfs hides host devices, we are required to have null and zero devices */
    snprintf(path, sizeof path, "%s/dev", rootfs);
    /* mount /dev to prep for our bind mounts of /dev/null and /dev/zero. */
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }
    /* bind mount null and zero */
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;


     /* mount a fresh proc filesystem on top of /proc so the container sees only its own processes.  */
    /* This has to come before pivot_root(): mounting a new /proc fails with
     * EPERM once the host's /proc is no longer visible. */
    snprintf(path, sizeof path, "%s/proc", rootfs);
    if (mount("proc", path, "proc", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /* change the current working directory to rootfs so that pivot_root() can use "." as the new root. pivot_root requires the new root to be the current working directory. */
    if (chdir(rootfs) < 0) {
        fprintf(stderr, "container: chdir rootfs: %s\n", strerror(errno));
        return -1;
    }
    
    /* change rootfs of container to rootfs so container just sees / */
    /* since we cannot create a directory in the read-only rootfs to park the old root, we stack it on top of the new root at the same spot. this way, the old root is still there, but it is hidden under the new root and cannot be accessed by the container. */
    if (syscall(SYS_pivot_root, ".", ".") < 0) {
        fprintf(stderr, "container: pivot_root: %s\n", strerror(errno));
        return -1;
    }
    /* detach the old root so the container cannot access the host's files. MNT_DETACH = lazy unmount: it disappears right away even though we are still "inside" it (our cwd). */
    if (umount2(".", MNT_DETACH) < 0) {
        fprintf(stderr, "container: detach old root: %s\n", strerror(errno));
        return -1;
    }
    /* bc of lazy unmount we need to chdir into / so that we are not in the old root anymore */
    if (chdir("/") < 0) {
        fprintf(stderr, "container: chdir /: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* SPEC Part III: drops every capability and sets no_new_privs. The bounding
 * set goes first, because dropping from it needs CAP_SETPCAP.
 * container_setup() calls it after the privileged setup steps. */

/* capability versus privilege: a capability is a fine-grained permission that can be granted to a process, while a privilege is a broader concept that encompasses multiple capabilities. In Linux, capabilities are used to grant specific privileges to processes without giving them full root access. Dropping capabilities from the bounding set means that the process cannot gain those capabilities in the future, even if it tries to elevate its privileges. This is important for security, as it prevents the container from gaining additional privileges that could compromise the host system. */

static int drop_capabilities(void)
{
    /* EINVAL means the running kernel doesn't have this capability. */
    /* drop all capabilities from the bounding set, which is the set of capabilities that can be gained by a process.  */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
            fprintf(stderr, "container: drop capability %d from bounding set: %s\n",
                    cap, strerror(errno));
            return -1;
        }
    }

    /* clear caps we hold right now in capset */
    /* Clear effective, permitted and inheritable. Version 3 takes two data structs (capabilities 0-31 and 32-63).
    this is why data[2] */
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];
    memset(&header, 0, sizeof header); /* zero out the header struct */
    memset(data, 0, sizeof data); /* zero out the data struct, all zeros = no capabilities */
    header.version = _LINUX_CAPABILITY_VERSION_3; 
    header.pid = 0; /* PID 0 applies the capability change to this calling process. */
    /* apply the empty sets: replace this process's current capability sets with data. Because data is zeroed, the process keeps no capabilities. */
    if (syscall(SYS_capset, &header, data) < 0) {
        fprintf(stderr, "container: clear capabilities: %s\n", strerror(errno));
        return -1;
    }

    /* Stops a setuid program or file capabilities program from handing privileges back. done again in seccomp */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Isolates the container from the inside. Runs in the container's init, after
 * the parent releases us and before the command starts. The steps that need
 * privileges come first; capabilities and seccomp take them away at the end. */
int container_setup(struct container *c)
{
    /* bc of uts namespace container can have different hostname than host. allows container to have its own identity and not interfere with the host system. */
    if (sethostname(c->hostname, strlen(c->hostname)) < 0) {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    /* these below need root capabilities so they must be done before we drop capabilities and set no_new_privs. */

    /* private loopback interface so container can communicate with itself. local host */
    (void)container_network();

    /* configure container's end of the veth pair that container_net_host_setup() moved into namespace: address, netmask, link up, and a default route through the host's bridge. Needs CAP_NET_ADMIN. */
    /* result is that the container can communicate with the host and other containers on the same bridge network. */
    if (c->net_enabled)
        (void)container_net_config(c);

    /* setup the container's filesystem and pivots into it */
    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    /* drop every capability and sets no_new_privs. */
    if (drop_capabilities() < 0)
        return -1;

    /* create filter of banned syscalls  */
    /* Must be last: the filter blocks mount() and pivot_root(). */
    if (container_seccomp() < 0)
        return -1;

    return 0;
}

/* Creates the loopback interface so the container can communicate with itself.
 * This is required for the container to be able to use localhost.
 * container_setup() calls it for every container. */
int container_network(void)
{
    /* The socket is only a handle for the ioctls. */
    /* open a socket to talk to the kernel */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* ifreq is a struct used to configure network interfaces. */
    struct ifreq request;
    memset(&request, 0, sizeof request); /* zero out */
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1); /* set the interface name to "lo" (loopback) */

    /* Ask the kernel for lo's current settings (the flags). */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read lo flags: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* Turn on UP and RUNNING without changing the other flags. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

     /* Send the updated flags back so the kernel applies them. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring lo up: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}

/* Converts an IPv4 address from text into the binary form used by the kernel.
 * container_net_config() calls it for the interface address and route fields. */
static int make_ipv4_address(struct sockaddr *addr, const char *ip_text)
{
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr; /* convert to ipv4 struct */
    memset(ipv4, 0, sizeof *ipv4); /* zero out the struct */
    ipv4->sin_family = AF_INET; /* set the address family to AF_INET (IPv4) */
    /* convert the text representation of the IP address to binary form and store it in the sin_addr field of the sockaddr_in struct. */
    if (inet_pton(AF_INET, ip_text, &ipv4->sin_addr) != 1) {
        fprintf(stderr, "container: bad IPv4 address '%s'\n", ip_text);
        return -1;
    }
    return 0;
}

/* SPEC Part I (--net only): configures the container's end of the veth pair
 * that container_net_host_setup() moved into our namespace: address, netmask,
 * link up, and a default route through the host's bridge. Needs
 * CAP_NET_ADMIN. container_setup() calls it when networking is enabled. */
int container_net_config(struct container *c)
{
    /* open a socket to talk to the kernel. AF_INET = IPv4, SOCK_DGRAM = datagram socket (UDP), 0 = default protocol for the socket type. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* ifreq is a struct used to configure network interfaces. it is used to set the IP address, netmask, and flags for the interface. */
    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1); /* copy ceth0 into the ifr_name  */

    /* convert string address into ipv4 binary form */
    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;
    /* set the IP address of the interface using ioctl. SIOCSIFADDR = set interface address. */
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        fprintf(stderr, "container: set %s address: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* share the ifreq netmask storage as an IPv4 sockaddr. this is where we will store the netmask for the interface. */
    struct sockaddr_in *netmask = (struct sockaddr_in *)&request.ifr_netmask;

    /* zero out the netmask struct and set the address family to AF_INET (IPv4) */
    memset(netmask, 0, sizeof *netmask);
    netmask->sin_family = AF_INET;

    /* calculate the number of netmask bits for the network prefix length */
    uint32_t mask_bits = 0;
    if (c->net_prefix > 0)
        mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);

    /* convert the netmask bits to network byte order */
    netmask->sin_addr.s_addr = htonl(mask_bits);
    /* set the netmask of the interface using ioctl. SIOCSIFNETMASK = set interface netmask. */
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
        fprintf(stderr, "container: set %s netmask: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* get the current flags for the interface using ioctl */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read %s flags: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }
    /* set interface to up and running */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* set the flags for the interface using ioctl.  */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring %s up: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }
    /* ============== */
    /* Default route (0.0.0.0/0) through the gateway. */
    /* configure the default route (0.0.0.0/0), which tells the container */
    /* where to send packets when no, more specific route matches the destination. */
    struct rtentry route;

    memset(&route, 0, sizeof route); /* zero out */

    /* set the destination to 0.0.0.0, meaning this route matches any destination IP. */
    /* set the netmask to 0.0.0.0, meaning no destination IP bits need to match. */
    /* set the gateway to c->net_gw, the gateway IP that will forward packets */
    /* to destinations outside the container's local network. */
    if (make_ipv4_address(&route.rt_dst, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_genmask, "0.0.0.0") < 0 ||
        make_ipv4_address(&route.rt_gateway, c->net_gw) < 0)
        goto fail;

    /* mark the route as active (RTF_UP) and specify that packets should be */
    /* forwarded through a gateway (RTF_GATEWAY). */
    route.rt_flags = RTF_UP | RTF_GATEWAY;

    /* add the default route to the kernel's routing table using ioctl. */
    /* SIOCADDRT = add a route to the routing table. */
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

/* The audit architecture id for the CPU we're building for. */
#if defined(__x86_64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "container_seccomp() only supports x86_64 and aarch64"
#endif

/* The SPEC's denylist of system calls. */
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

/* calculate the number of denied syscalls by dividing the size of the array by the size of one element. */
#define NUM_DENIED_SYSCALLS (sizeof denied_syscalls / sizeof denied_syscalls[0])

/* SPEC Part III: installs a seccomp filter that fails the denied system calls  and allows everything else. A call made under a different
 * architecture kills the process, because the same number means a different
 * call there.
 *
 * BPF_STMT and BPF_JUMP expand to initializer lists, which is why each one is
 * cast to a compound literal before being assigned. container_setup() calls
 * this last, after the setup system calls that the filter blocks. */
int container_seccomp(void)
{
    /* 4 fixed instructions, 2 per denied call, and 1 final allow. */
    /*
     * Make room for the instructions in the filter:
     *
     * - 4 fixed instructions load and check the architecture, kill the process
     *   for the wrong architecture, and load the syscall number.
     * - 2 instructions per denied call check for that syscall number and
     *   return EPERM if it matches.
     * - 1 final instruction allows everything that did not match.
     */
    struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1]; 
    size_t count = 0;

    /* Kill the process if the architecture isn't ours. */

    /* load the architecture field from the seccomp_data struct into the accumulator.  */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                               offsetof(struct seccomp_data, arch));
    /* check if the architecture matches OUR_AUDIT_ARCH. If it does, jump to the next instruction (1), otherwise jump to the instruction that kills the process (0). */
    filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, OUR_AUDIT_ARCH, 1, 0);

    /* if the architecture does not match, kill the process */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* Load the syscall number from the seccomp_data struct into the accumulator. This is used to check if the syscall is in the denylist. */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                               offsetof(struct seccomp_data, nr));

    /* For each denied call: return EPERM on a match, otherwise skip to the
     * next pair. */
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++) {
        /* check if the syscall number matches the denied syscall. If it does, jump to the instruction that returns EPERM (1), otherwise jump to the next instruction (0). */
        filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                   (unsigned int)denied_syscalls[i], 0, 1);
        /* if matches the denied syscall, return EPERM. error  */
        filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                   SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    /* if the syscall is not in the denylist, allow it to proceed. */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    /* create a sock_fprog struct that contains the filter program and its length. will be passed to the kernel when installing the seccomp filter. */
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };

    /* Required to install a filter without CAP_SYS_ADMIN, admin capabilties. */
    /* prevent the process from gaining new privileges through setuid or file capabilities. This is a security measure to ensure that the seccomp filter cannot be bypassed by elevating privileges. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /* glibc has no seccomp() wrapper. */
    /* install the seccomp filter using the seccomp() system call. */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0) {
        fprintf(stderr, "container: install seccomp filter: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Converts a waitpid() status into a shell-style exit code: the exit status,
 * or 128 + the signal number if a signal killed the process. container_init()
 * and container_run() call it after waiting for their child. */
static int status_to_exit_code(int status)
{
    /* if the process exited normally, return its exit status (0-255) */
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    /* if the process was terminated by a signal, return 128 + the signal number. */
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

/* SPEC Part IV: child_entry() calls this after clone() creates the container
 * process, which becomes PID 1 inside the new PID namespace. It waits for the
 * host parent to finish setup, prepares the container, forks the requested
 * command, and remains PID 1 to reap children until that command exits. */
int container_init(struct container *c)
{
    /*
     * Close container init's copy of the pipe's write end. It only reads from
     * this pipe while the host parent writes to it. If the host parent gives
     * up and closes its write end without sending a byte, read() returns EOF.
     */
    char release_byte;
    close(c->sync[1]);

    /*
     * Wait until the host parent finishes UID/GID mapping, cgroup entry, and
     * optional host-side networking. One byte means setup succeeded and
     * container init may continue; EOF or an error means it was not released.
     */
    if (read(c->sync[0], &release_byte, 1) != 1) {
        fprintf(stderr, "container: parent did not release the container\n");
        return 1;
    }

    /* The synchronization is complete, so close the remaining read end. */
    close(c->sync[0]);

    /*
     * Finish the setup that must run from inside the new namespaces: set the
     * hostname, configure networking, enter the root filesystem, drop
     * capabilities, and install the seccomp filter.
     */
    if (container_setup(c) < 0)
        return 1;

    /*
     * Fork so container init can remain PID 1 and reap processes. The new
     * child will become the requested command, while the parent branch stays
     * in this function as container init.
     */
    pid_t command_pid = fork();
    if (command_pid < 0) {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }

    /*
     * fork() returns 0 only in the newly created command child. 
     * execvp() then replaces this child with c->argv without creating another
     * process, so the requested command keeps the same PID, normally PID 2.
     */
    if (command_pid == 0) {
        execvp(c->argv[0], c->argv);

        /* execvp() returns only if it fails; 127 is the shell-style exec error. */
        fprintf(stderr, "container: exec %s: %s\n", c->argv[0], strerror(errno));
        _exit(127);
    }

    /*
     * Only container init reaches this loop. As PID 1, it adopts orphaned
     * descendants and must wait for them so exited processes do not remain as
     * zombies. It stops when the original requested command has exited.
     */
    for (;;) {
        int status;

        // wait for any child to change state
        pid_t reaped = waitpid(-1, &status, 0);
        // if waitpid() returns an error, check if it was interrupted by a signal (EINTR). If so, continue the loop and try again. Otherwise, print an error message and return 1 to indicate failure.
        if (reaped < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }
        /* If the reaped child is the command we started, return its exit code. */
        if (reaped == command_pid)
            return status_to_exit_code(status);
    }
}

/* ---- the whole lifecycle: main.c calls only this ----------------------- */

/* Adapts container_init() to the signature clone() expects. clone() calls this
 * as the new container process's entry point. */
static int child_entry(void *arg)
{
    return container_init((struct container *)arg);
}

/* Runs one container from start to finish and returns its exit code. The
 * parent does the setup that has to happen from outside, then releases the
 * child (the container's init) through the sync pipe. The provided main()
 * calls this function. */
int container_run(struct container *c)
{
    /* create the container's cgroup and set its limits.  */
    if (container_cgroup_init(c) < 0)
        return 1;

    /* create a pipe for synchronizing the parent and child. parent writes, child waits */
    if (pipe(c->sync) < 0) {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        container_cleanup(c);
        return 1;
    }

    /*
     * clone() needs a separate child stack for local variables and function
     * calls. This child has separate memory from the parent because CLONE_VM
     * is not passed.  
     */
    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (stack == NULL) {
        fprintf(stderr, "container: out of memory for the child stack\n");
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /*
     * The stack grows downwards, so pass the top of the allocated buffer to
     * clone() as the starting point for the child process's stack.
     */
    pid_t child = clone(child_entry, stack + CONTAINER_STACK_SIZE,
                        container_namespaces() | SIGCHLD, c);
    if (child < 0) { /* error handling */
        fprintf(stderr, "container: clone: %s\n", strerror(errno));
        free(stack);
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /* If setup fails we still have to unblock and reap the child, so record
     * the result instead of returning. */
    /* write the uid_map and gid_map for the child process to set up user namespace mappings. */
    int setup_ok = (container_write_idmaps(c, child) == 0);

    /* enter the child process into the container's cgroup to apply resource limits and isolation. */
    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    /* setup the container's network interface if networking is enabled. This configures the container's end of the veth pair that was created in the parent process. allows container-to-host and container-to-container */
    if (c->net_enabled)
        (void)container_net_host_setup(c, child);

    close(c->sync[0]); /* close the read end */

    /* write a byte to the sync pipe to signal the child process that it can proceed with its initialization. If writing fails, print an error message and set setup_ok to 0 to indicate that setup failed. */
    if (setup_ok && write(c->sync[1], "x", 1) != 1) {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }

    close(c->sync[1]); /* close the write end of the sync pipe */

    int status;

    /*
     * Wait for the child process to exit and retrieve its exit status.
     *
     * - If a signal interrupts waitpid(), retry it.
     * - If waitpid() fails for another reason, print the error, free the
     *   allocated stack, and return 1.
     * - When the child exits, continue with the normal cleanup below.
     */
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            free(stack);
            return 1;
        }
    }
    free(stack);

    if (c->net_enabled)
        (void)container_net_host_teardown(c);

    (void)container_cleanup(c);

    /* If setup failed, the command never ran. */
    if (!setup_ok)
        return 1;
    return status_to_exit_code(status);
}

/* removes the container's cgroup. Runs in the parent after the
 * init has been reaped; the kernel has already torn down everything else.
 * container_run() calls it after the container exits and on setup failures. */
int container_cleanup(struct container *c)
{
    /* Nothing to remove if the cgroup was never created. */
    if (c->cg_path[0] == '\0')
        return 0;

    /* remove c group directory, ENOENT means it is already gone, which counts as success. */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT) {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }
    return 0;
}
