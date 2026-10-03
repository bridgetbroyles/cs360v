/* container.c: STUDENT IMPLEMENTATION FILE for Project 2.
 *
 * You implement a minimal container runtime here. main.c parses the command
 * line and calls container_run(); everything after that is yours.
 *
 * The TODOs below are the checklist: what to call and in what order. SPEC.md
 * explains what each mechanism is and why the order matters, and is the
 * contract if the two ever disagree.
 *
 * As shipped, container_run() returns 1 and nothing runs, so no checks pass.
 * Start by getting the command to execute: that needs container_run() and
 * container_init() to spawn it and container_setup() to pivot into the rootfs,
 * because the command lives inside the rootfs.
 *
 * Provided: main.c (argument parsing), util.c (write_file()), net.c (the --net
 * host side), container.h (the struct, the declarations, the stack size).
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

/* ---- Part I: namespaces ----------------------------------------------- */

/* The namespaces the container gets its own copy of. */
int container_namespaces(void)
{
    return CLONE_NEWUSER
         | CLONE_NEWPID
         | CLONE_NEWNS
         | CLONE_NEWUTS
         | CLONE_NEWNET;
}

/* Runs in the parent. Maps root inside the container onto our own uid/gid
 * outside it. */
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;

    char path[64];
    char mapping[64];

    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
    if (write_file(path, mapping) < 0)
        return -1;

    /* The kernel won't accept gid_map until setgroups is denied. */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    return 0;
}

/* ---- Part V: cgroup --------------------------------------------------- */

/* Writes one limit into a file in the container's cgroup. A negative limit
 * means "no limit" and is written as "max". */
static int write_cgroup_limit(struct container *c, const char *file_name, long limit)
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

/* SPEC Part V: creates the container's cgroup and sets its limits. Runs in
 * the parent, before the container starts. */
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    /* The controllers only work in our cgroup if the parent delegates them. */
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    /* EEXIST is fine: a cgroup left over from a crashed run is reused, and
     * its limits are overwritten below. */
    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s", c->cgroup_base, c->name);
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    /* Swap is set to 0 so that memory.max can't be dodged by swapping out. */
    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;
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

    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);
    return write_file(path, pid_text);
}

/* ---- Parts I/II/III: isolation, run inside the container init --------- */

/* Bind-mounts the host's /dev/<name> at <rootfs>/dev/<name>. mknod() isn't
 * allowed in a user namespace, so we bind the host's node instead. */
static int bind_device(const char *rootfs, const char *name)
{
    char host_path[PATH_MAX];
    char target_path[PATH_MAX];
    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    /* A bind mount needs an existing file to mount over. */
    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "container: create %s: %s\n", target_path, strerror(errno));
        return -1;
    }
    close(fd);

    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0) {
        fprintf(stderr, "container: bind %s: %s\n", host_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* SPEC Part II: builds the container's filesystem and pivots into it, so that
 * `rootfs` becomes "/". The order of the steps matters. */
static int setup_filesystem(const char *rootfs)
{
    char path[PATH_MAX];

    /* Stop our mounts from propagating back to the host. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /* pivot_root() needs a mount point, so bind the rootfs onto itself, then
     * remount it read-only. */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }
    if (mount(NULL, rootfs, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        fprintf(stderr, "container: remount rootfs read-only: %s\n", strerror(errno));
        return -1;
    }

    /* A writable /tmp on top of the read-only root. */
    snprintf(path, sizeof path, "%s/tmp", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /* A minimal /dev with only null and zero. */
    snprintf(path, sizeof path, "%s/dev", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;

    /* This has to come before pivot_root(): mounting a new /proc fails with
     * EPERM once the host's /proc is no longer visible. */
    snprintf(path, sizeof path, "%s/proc", rootfs);
    if (mount("proc", path, "proc", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /* pivot_root(".", ".") stacks the old root on top of the new one, and
     * umount2() then detaches it. glibc has no pivot_root() wrapper. */
    if (chdir(rootfs) < 0) {
        fprintf(stderr, "container: chdir rootfs: %s\n", strerror(errno));
        return -1;
    }
    if (syscall(SYS_pivot_root, ".", ".") < 0) {
        fprintf(stderr, "container: pivot_root: %s\n", strerror(errno));
        return -1;
    }
    if (umount2(".", MNT_DETACH) < 0) {
        fprintf(stderr, "container: detach old root: %s\n", strerror(errno));
        return -1;
    }
    if (chdir("/") < 0) {
        fprintf(stderr, "container: chdir /: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* SPEC Part III: drops every capability and sets no_new_privs. The bounding
 * set goes first, because dropping from it needs CAP_SETPCAP. */
static int drop_capabilities(void)
{
    /* EINVAL means the running kernel doesn't have this capability. */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
            fprintf(stderr, "container: drop capability %d from bounding set: %s\n",
                    cap, strerror(errno));
            return -1;
        }
    }

    /* Clear effective, permitted and inheritable. Version 3 takes two data
     * structs (capabilities 0-31 and 32-63). */
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];
    memset(&header, 0, sizeof header);
    memset(data, 0, sizeof data);
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
    if (syscall(SYS_capset, &header, data) < 0) {
        fprintf(stderr, "container: clear capabilities: %s\n", strerror(errno));
        return -1;
    }

    /* Stops a setuid program from handing privileges back. */
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
    if (sethostname(c->hostname, strlen(c->hostname)) < 0) {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    /* Networking is best-effort per the SPEC: a failure is printed but the
     * command still runs. */
    (void)container_network();

    if (c->net_enabled)
        (void)container_net_config(c);

    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    if (drop_capabilities() < 0)
        return -1;

    /* Must be last: the filter blocks mount() and pivot_root(). */
    if (container_seccomp() < 0)
        return -1;

    return 0;
}

/* SPEC Part I: brings up the loopback interface, which starts down in a new
 * network namespace. Needs CAP_NET_ADMIN. */
int container_network(void)
{
    /* The socket is only a handle for the ioctls. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);

    /* Read the current flags first so we don't clear any that are set. */
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

/* Fills in `addr` with the IPv4 address in `ip_text` (e.g. "10.44.0.2").
 * Returns -1 if the text isn't a valid address. */
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

/* SPEC Part I (--net only): configures the container's end of the veth pair
 * that container_net_host_setup() moved into our namespace: address, netmask,
 * link up, and a default route through the host's bridge. Needs
 * CAP_NET_ADMIN. */
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

    /* Build the netmask from the prefix length. A prefix of 0 is handled
     * separately because shifting a 32-bit value by 32 is undefined. */
    struct sockaddr_in *netmask = (struct sockaddr_in *)&request.ifr_netmask;
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

    /* Default route (0.0.0.0/0) through the gateway. */
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

#define NUM_DENIED_SYSCALLS (sizeof denied_syscalls / sizeof denied_syscalls[0])

/* SPEC Part III: installs a seccomp filter that fails the denied system calls
 * with EPERM and allows everything else. A call made under a different
 * architecture kills the process, because the same number means a different
 * call there.
 *
 * BPF_STMT and BPF_JUMP expand to initializer lists, which is why each one is
 * cast to a compound literal before being assigned. */
int container_seccomp(void)
{
    /* 4 fixed instructions, 2 per denied call, and 1 final allow. */
    struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1];
    size_t count = 0;

    /* Kill the process if the architecture isn't ours. */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                               offsetof(struct seccomp_data, arch));
    filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, OUR_AUDIT_ARCH, 1, 0);
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                               offsetof(struct seccomp_data, nr));

    /* For each denied call: return EPERM on a match, otherwise skip to the
     * next pair. */
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++) {
        filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                   (unsigned int)denied_syscalls[i], 0, 1);
        filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                   SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };

    /* Required to install a filter without CAP_SYS_ADMIN. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /* glibc has no seccomp() wrapper. */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0) {
        fprintf(stderr, "container: install seccomp filter: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Converts a waitpid() status into a shell-style exit code: the exit status,
 * or 128 + the signal number if a signal killed the process. */
static int status_to_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

/* SPEC Part IV: the container's init (PID 1). Waits for the parent to release
 * it, runs container_setup(), starts the command, and reaps children until
 * the command exits. Returns the command's exit code. */
int container_init(struct container *c)
{
    /* Close our copy of the write end first, so that read() sees EOF if the
     * parent gives up without writing. */
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
        fprintf(stderr, "container: exec %s: %s\n", c->argv[0], strerror(errno));
        _exit(127);
    }

    /* As PID 1 we also inherit orphans, so reap every child until the command
     * itself exits. */
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

/* ---- the whole lifecycle: main.c calls only this ----------------------- */

/* Adapts container_init() to the signature clone() expects. */
static int child_entry(void *arg)
{
    return container_init((struct container *)arg);
}

/* Runs one container from start to finish and returns its exit code. The
 * parent does the setup that has to happen from outside, then releases the
 * child (the container's init) through the sync pipe. */
int container_run(struct container *c)
{
    /* Keep the "container: " prefix on anything you print here: the test
     * harness reads the container's output and skips lines starting with it. */

    if (container_cgroup_init(c) < 0)
        return 1;

    if (pipe(c->sync) < 0) {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        container_cleanup(c);
        return 1;
    }

    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (stack == NULL) {
        fprintf(stderr, "container: out of memory for the child stack\n");
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /* The stack grows down, so clone() gets the top of the buffer. */
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

    /* If setup fails we still have to unblock and reap the child, so record
     * the result instead of returning. */
    int setup_ok = (container_write_idmaps(c, child) == 0);

    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    /* Best-effort, like net.c itself. */
    if (c->net_enabled)
        (void)container_net_host_setup(c, child);

    /* Release the child. If setup failed we skip the write, and closing the
     * pipe makes the child's read() return 0 so that it exits. */
    close(c->sync[0]);
    if (setup_ok && write(c->sync[1], "x", 1) != 1) {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }
    close(c->sync[1]);

    int status;
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

/* ---- Part VI: teardown ------------------------------------------------- */

/* SPEC Part VI: removes the container's cgroup. Runs in the parent after the
 * init has been reaped; the kernel has already torn down everything else. */
int container_cleanup(struct container *c)
{
    /* Nothing to remove if the cgroup was never created. */
    if (c->cg_path[0] == '\0')
        return 0;

    /* ENOENT means it is already gone, which counts as success. */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT) {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }
    return 0;
}
