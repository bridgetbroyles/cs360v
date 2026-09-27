/* container.c: student implementation of the Project 2 container runtime.
 *
 * Quick map of this file
 * - report_errno(): prints a consistently prefixed system-call error.
 * - format_path(): builds a path and rejects truncation.
 * - create_bind_target(): creates a file a device bind mount can cover.
 * - set_interface_address(): assigns an IPv4 address or mask with ioctl.
 * - drop_capabilities(): removes all capabilities before the command runs.
 * - status_code(): converts waitpid status to a shell-style exit status.
 * - child_entry(): adapts container_init() to clone()'s callback type.
 * - wait_for_pid(): waits through EINTR for one specific host process.
 * - container_namespaces(): selects the five isolation namespaces.
 * - container_write_idmaps(): maps container root to the runtime's host user.
 * - container_cgroup_init(): creates a cgroup and applies its limits.
 * - container_cgroup_enter(): moves the container init into its cgroup.
 * - container_setup(): configures hostname, network, rootfs, and sandboxing.
 * - container_network(): brings up loopback in the new network namespace.
 * - container_net_config(): configures the optional container-side veth.
 * - container_seccomp(): installs the dangerous-system-call denylist.
 * - container_init(): acts as PID 1, launches the command, and reaps children.
 * - container_run(): drives the complete parent-side lifecycle.
 * - container_cleanup(): removes the empty cgroup after exit.
 */
#define _GNU_SOURCE
#include "container.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <net/if.h>
#include <net/route.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define CONTAINER_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CONTAINER_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "Project 2 supports x86-64 and AArch64 Linux"
#endif

#define ERROR_EXIT_STATUS 1

/*
 * What: Prints a failed operation and its saved errno with `container:`.
 * Called by: Helpers and lifecycle functions after a Linux call fails.
 * Important: Call it before another operation can overwrite errno.
 */
static int report_errno(const char *operation)
{
    int saved_errno = errno;
    fprintf(stderr, "container: %s: %s\n", operation, strerror(saved_errno));
    errno = saved_errno;
    return -1;
}

/*
 * What: Joins two path components while detecting an overlong result.
 * Called by: Filesystem and cgroup code that builds runtime paths.
 * Important: A truncated path could modify the wrong host file, so it fails.
 */
static int format_path(char *out, size_t out_size,
                       const char *directory, const char *name)
{
    int written = snprintf(out, out_size, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return report_errno("path is too long");
    }
    return 0;
}

/*
 * What: Creates an empty file in the new /dev tmpfs for a bind mount.
 * Called by: container_setup() for /dev/null and /dev/zero.
 * Important: Mounting tmpfs hides the original rootfs placeholders.
 */
static int create_bind_target(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0)
        return report_errno(path);
    if (close(fd) != 0)
        return report_errno("close device bind target");
    return 0;
}

/*
 * What: Assigns either an IPv4 address or netmask to one interface.
 * Called by: container_net_config() while it has CAP_NET_ADMIN.
 * Important: `request` is SIOCSIFADDR or SIOCSIFNETMASK.
 */
static int set_interface_address(int sock, const char *ifname,
                                 const char *address, unsigned long request)
{
    struct ifreq ifr;
    struct sockaddr_in sin;
    memset(&ifr, 0, sizeof ifr);
    memset(&sin, 0, sizeof sin);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    sin.sin_family = AF_INET;
    if (inet_pton(AF_INET, address, &sin.sin_addr) != 1) {
        errno = EINVAL;
        return report_errno("invalid IPv4 address");
    }
    memcpy(&ifr.ifr_addr, &sin, sizeof sin);
    if (ioctl(sock, request, &ifr) != 0)
        return report_errno(request == SIOCSIFADDR ?
                            "set interface address" : "set interface netmask");
    return 0;
}

/*
 * What: Removes bounding, effective, permitted, and inheritable capabilities.
 * Called by: container_setup() after privileged setup operations finish.
 * Important: Calling this earlier would break mounts and network setup.
 */
static int drop_capabilities(void)
{
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) != 0)
            return report_errno("drop capability from bounding set");
    }

    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof data);
    if (syscall(SYS_capset, &header, data) != 0)
        return report_errno("clear process capabilities");
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return report_errno("set no_new_privs");
    return 0;
}

/*
 * What: Converts waitpid's encoded status to a normal process status.
 * Called by: container_init() and container_run() after child exits.
 * Important: A signal becomes 128 + signal, matching shell behavior.
 */
static int status_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return ERROR_EXIT_STATUS;
}

/*
 * What: Gives clone() its required `int (*)(void *)` callback.
 * Called by: clone() in container_run().
 * Important: Its return value becomes the init process's exit status.
 */
static int child_entry(void *argument)
{
    return container_init(argument);
}

/*
 * What: Waits for one host PID and retries interrupted waits.
 * Called by: container_run() on normal and error-cleanup paths.
 * Important: It waits for the cloned init, not arbitrary children.
 */
static int wait_for_pid(pid_t pid, int *status)
{
    pid_t result;
    do {
        result = waitpid(pid, status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return report_errno("wait for container init");
    return 0;
}

/*
 * What: Returns clone flags for user, PID, mount, UTS, and network isolation.
 * Called by: container_run() when it creates the container init.
 * Important: container_run() adds SIGCHLD separately.
 */
int container_namespaces(void)
{
    return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS |
           CLONE_NEWUTS | CLONE_NEWNET;
}

/*
 * What: Maps UID/GID 0 inside the user namespace to the caller outside.
 * Called by: container_run() in the parent immediately after clone().
 * Important: `setgroups` must be denied before writing gid_map.
 */
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;
    char proc_dir[64], path[96], mapping[64];
    int written = snprintf(proc_dir, sizeof proc_dir, "/proc/%d", (int)child);
    if (written < 0 || (size_t)written >= sizeof proc_dir) {
        errno = ENAMETOOLONG;
        return report_errno("child proc path is too long");
    }

    if (format_path(path, sizeof path, proc_dir, "uid_map") != 0)
        return -1;
    snprintf(mapping, sizeof mapping, "0 %u 1", (unsigned)getuid());
    if (write_file(path, mapping) != 0)
        return -1;

    if (format_path(path, sizeof path, proc_dir, "setgroups") != 0 ||
        write_file(path, "deny") != 0)
        return -1;

    if (format_path(path, sizeof path, proc_dir, "gid_map") != 0)
        return -1;
    snprintf(mapping, sizeof mapping, "0 %u 1", (unsigned)getgid());
    return write_file(path, mapping);
}

/*
 * What: Creates one cgroup v2 directory and installs PID/memory limits.
 * Called by: container_run() before it creates the container init.
 * Important: An existing directory is valid stale state and is reused.
 */
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX], value[64];
    if (format_path(path, sizeof path, c->cgroup_base,
                    "cgroup.subtree_control") != 0 ||
        write_file(path, "+pids +memory") != 0)
        return -1;

    if (format_path(c->cg_path, sizeof c->cg_path,
                    c->cgroup_base, c->name) != 0)
        return -1;
    if (mkdir(c->cg_path, 0755) != 0 && errno != EEXIST)
        return report_errno("create container cgroup");

    if (format_path(path, sizeof path, c->cg_path, "pids.max") != 0)
        return -1;
    if (c->pids_max < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", c->pids_max);
    if (write_file(path, value) != 0)
        return -1;

    if (format_path(path, sizeof path, c->cg_path, "memory.max") != 0)
        return -1;
    if (c->mem_max < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", c->mem_max);
    if (write_file(path, value) != 0)
        return -1;

    if (format_path(path, sizeof path, c->cg_path, "memory.swap.max") != 0)
        return -1;
    return write_file(path, "0");
}

/*
 * What: Moves the cloned init into the configured cgroup.
 * Called by: container_run() before releasing the child sync pipe.
 * Important: Future descendants inherit this cgroup membership.
 */
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX], pid_text[32];
    if (format_path(path, sizeof path, c->cg_path, "cgroup.procs") != 0)
        return -1;
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);
    return write_file(path, pid_text);
}

/*
 * What: Builds isolation and then removes privilege from container init.
 * Called by: container_init() after parent-side ID-map/cgroup setup.
 * Important: Network/filesystem work must precede capability and seccomp drops.
 */
int container_setup(struct container *c)
{
    char tmp[PATH_MAX], dev[PATH_MAX], proc[PATH_MAX];
    char null_path[PATH_MAX], zero_path[PATH_MAX];

    if (sethostname(c->hostname, strlen(c->hostname)) != 0)
        return report_errno("set container hostname");
    (void)container_network(); /* Loopback is explicitly best-effort. */
    if (c->net_enabled && container_net_config(c) != 0)
        return -1;

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return report_errno("make mount propagation private");
    if (mount(c->rootfs, c->rootfs, NULL, MS_BIND | MS_REC, NULL) != 0)
        return report_errno("bind rootfs onto itself");
    if (mount(NULL, c->rootfs, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) != 0)
        return report_errno("remount rootfs read-only");

    if (format_path(tmp, sizeof tmp, c->rootfs, "tmp") != 0 ||
        format_path(dev, sizeof dev, c->rootfs, "dev") != 0 ||
        format_path(proc, sizeof proc, c->rootfs, "proc") != 0)
        return -1;
    if (mount("tmpfs", tmp, "tmpfs", 0, "mode=1777") != 0)
        return report_errno("mount container /tmp");
    if (mount("tmpfs", dev, "tmpfs", 0, "mode=755") != 0)
        return report_errno("mount container /dev");

    if (format_path(null_path, sizeof null_path, dev, "null") != 0 ||
        format_path(zero_path, sizeof zero_path, dev, "zero") != 0 ||
        create_bind_target(null_path) != 0 ||
        create_bind_target(zero_path) != 0)
        return -1;
    if (mount("/dev/null", null_path, NULL, MS_BIND, NULL) != 0)
        return report_errno("bind /dev/null");
    if (mount("/dev/zero", zero_path, NULL, MS_BIND, NULL) != 0)
        return report_errno("bind /dev/zero");
    if (mount("proc", proc, "proc", 0, NULL) != 0)
        return report_errno("mount container /proc");

    if (chdir(c->rootfs) != 0)
        return report_errno("change directory to rootfs");
    if (syscall(SYS_pivot_root, ".", ".") != 0)
        return report_errno("pivot into rootfs");
    if (umount2(".", MNT_DETACH) != 0)
        return report_errno("detach old root");
    if (chdir("/") != 0)
        return report_errno("change directory to new root");

    if (drop_capabilities() != 0)
        return -1;
    return container_seccomp();
}

/*
 * What: Marks the fresh namespace's loopback interface up and running.
 * Called by: container_setup() before it drops CAP_NET_ADMIN.
 * Important: Failure is reported but treated as best-effort by the caller.
 */
int container_network(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return report_errno("open loopback control socket");
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "lo");
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
        report_errno("read loopback flags");
        close(sock);
        return -1;
    }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
        report_errno("bring loopback up");
        close(sock);
        return -1;
    }
    if (close(sock) != 0)
        return report_errno("close loopback control socket");
    return 0;
}

/*
 * What: Gives the optional veth an address, netmask, and default route.
 * Called by: container_setup() when `--net` was supplied.
 * Important: The host moves the veth here before releasing the child.
 */
int container_net_config(struct container *c)
{
    if (c->net_prefix < 0 || c->net_prefix > 32) {
        errno = EINVAL;
        return report_errno("invalid network prefix");
    }
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return report_errno("open veth control socket");
    int result = -1;
    char netmask[INET_ADDRSTRLEN];
    uint32_t mask = c->net_prefix == 0 ? 0 :
                    UINT32_MAX << (32 - (unsigned)c->net_prefix);
    struct in_addr mask_addr = { .s_addr = htonl(mask) };
    if (!inet_ntop(AF_INET, &mask_addr, netmask, sizeof netmask)) {
        report_errno("format interface netmask");
        goto out;
    }
    if (set_interface_address(sock, c->net_ifname, c->net_ip, SIOCSIFADDR) != 0 ||
        set_interface_address(sock, c->net_ifname, netmask, SIOCSIFNETMASK) != 0)
        goto out;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", c->net_ifname);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0) {
        report_errno("read veth flags");
        goto out;
    }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0) {
        report_errno("bring veth up");
        goto out;
    }

    struct rtentry route;
    memset(&route, 0, sizeof route);
    struct sockaddr_in *dst = (struct sockaddr_in *)&route.rt_dst;
    struct sockaddr_in *gateway = (struct sockaddr_in *)&route.rt_gateway;
    struct sockaddr_in *genmask = (struct sockaddr_in *)&route.rt_genmask;
    dst->sin_family = AF_INET;
    dst->sin_addr.s_addr = htonl(INADDR_ANY);
    genmask->sin_family = AF_INET;
    genmask->sin_addr.s_addr = htonl(INADDR_ANY);
    gateway->sin_family = AF_INET;
    if (inet_pton(AF_INET, c->net_gw, &gateway->sin_addr) != 1) {
        errno = EINVAL;
        report_errno("invalid default gateway");
        goto out;
    }
    route.rt_flags = RTF_UP | RTF_GATEWAY;
    route.rt_dev = (char *)c->net_ifname;
    if (ioctl(sock, SIOCADDRT, &route) != 0) {
        report_errno("add default route");
        goto out;
    }
    result = 0;
out:
    if (close(sock) != 0 && result == 0)
        return report_errno("close veth control socket");
    return result;
}

#define DENY_SYSCALL(number) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (number), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))

/*
 * What: Installs a BPF filter returning EPERM for dangerous system calls.
 * Called by: container_setup() as its final operation.
 * Important: The arch check prevents interpreting another ABI's syscall IDs.
 */
int container_seccomp(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (uint32_t)offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, CONTAINER_AUDIT_ARCH, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (uint32_t)offsetof(struct seccomp_data, nr)),
#ifdef __NR_ptrace
        DENY_SYSCALL(__NR_ptrace),
#endif
#ifdef __NR_mount
        DENY_SYSCALL(__NR_mount),
#endif
#ifdef __NR_umount2
        DENY_SYSCALL(__NR_umount2),
#endif
#ifdef __NR_pivot_root
        DENY_SYSCALL(__NR_pivot_root),
#endif
#ifdef __NR_chroot
        DENY_SYSCALL(__NR_chroot),
#endif
#ifdef __NR_setns
        DENY_SYSCALL(__NR_setns),
#endif
#ifdef __NR_unshare
        DENY_SYSCALL(__NR_unshare),
#endif
#ifdef __NR_reboot
        DENY_SYSCALL(__NR_reboot),
#endif
#ifdef __NR_swapon
        DENY_SYSCALL(__NR_swapon),
#endif
#ifdef __NR_swapoff
        DENY_SYSCALL(__NR_swapoff),
#endif
#ifdef __NR_kexec_load
        DENY_SYSCALL(__NR_kexec_load),
#endif
#ifdef __NR_kexec_file_load
        DENY_SYSCALL(__NR_kexec_file_load),
#endif
#ifdef __NR_init_module
        DENY_SYSCALL(__NR_init_module),
#endif
#ifdef __NR_finit_module
        DENY_SYSCALL(__NR_finit_module),
#endif
#ifdef __NR_delete_module
        DENY_SYSCALL(__NR_delete_module),
#endif
#ifdef __NR_create_module
        DENY_SYSCALL(__NR_create_module),
#endif
#ifdef __NR_query_module
        DENY_SYSCALL(__NR_query_module),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {
        .len = (unsigned short)(sizeof filter / sizeof filter[0]),
        .filter = filter,
    };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return report_errno("set no_new_privs for seccomp");
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) != 0)
        return report_errno("install seccomp filter");
    return 0;
}
#undef DENY_SYSCALL

/*
 * What: Runs as PID 1, starts the command, and reaps child/orphan exits.
 * Called by: child_entry() in the process created by container_run().
 * Important: It blocks until the parent finishes maps, cgroup, and networking.
 */
int container_init(struct container *c)
{
    if (close(c->sync[1]) != 0) {
        report_errno("close child sync write end");
        return ERROR_EXIT_STATUS;
    }
    char release_byte;
    ssize_t bytes_read;
    do {
        bytes_read = read(c->sync[0], &release_byte, 1);
    } while (bytes_read < 0 && errno == EINTR);
    if (bytes_read != 1) {
        if (bytes_read < 0)
            report_errno("wait for parent release");
        else
            fprintf(stderr, "container: parent closed sync pipe before release\n");
        close(c->sync[0]);
        return ERROR_EXIT_STATUS;
    }
    if (close(c->sync[0]) != 0) {
        report_errno("close child sync read end");
        return ERROR_EXIT_STATUS;
    }
    if (container_setup(c) != 0)
        return ERROR_EXIT_STATUS;

    pid_t command = fork();
    if (command < 0) {
        report_errno("fork container command");
        return ERROR_EXIT_STATUS;
    }
    if (command == 0) {
        execvp(c->argv[0], c->argv);
        fprintf(stderr, "container: exec '%s': %s\n",
                c->argv[0], strerror(errno));
        _exit(127);
    }

    for (;;) {
        int status;
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR)
                continue;
            report_errno("reap container child");
            return ERROR_EXIT_STATUS;
        }
        if (reaped == command)
            return status_code(status);
    }
}

/*
 * What: Creates, releases, waits for, and tears down one complete container.
 * Called by: provided main.c after command-line parsing.
 * Important: Failures kill a blocked child and release every owned resource.
 */
int container_run(struct container *c)
{
    void *stack = NULL;
    pid_t child = -1;
    int status = 0, result = ERROR_EXIT_STATUS;
    int pipe_created = 0, network_attempted = 0;

    c->cg_path[0] = '\0';
    c->net_host_if[0] = '\0';
    c->sync[0] = c->sync[1] = -1;
    if (container_cgroup_init(c) != 0)
        goto cleanup;
    if (pipe(c->sync) != 0) {
        report_errno("create parent-child sync pipe");
        goto cleanup;
    }
    pipe_created = 1;
    stack = malloc(CONTAINER_STACK_SIZE);
    if (!stack) {
        report_errno("allocate clone stack");
        goto cleanup;
    }
    child = clone(child_entry, (char *)stack + CONTAINER_STACK_SIZE,
                  container_namespaces() | SIGCHLD, c);
    if (child < 0) {
        report_errno("clone container init");
        goto cleanup;
    }
    if (close(c->sync[0]) != 0) {
        report_errno("close parent sync read end");
        c->sync[0] = -1;
        goto cleanup;
    }
    c->sync[0] = -1;
    if (container_write_idmaps(c, child) != 0 ||
        container_cgroup_enter(c, child) != 0)
        goto cleanup;

    if (c->net_enabled) {
        network_attempted = 1;
        (void)container_net_host_setup(c, child); /* Provided and best-effort. */
    }

    char release_byte = '1';
    ssize_t bytes_written;
    do {
        bytes_written = write(c->sync[1], &release_byte, 1);
    } while (bytes_written < 0 && errno == EINTR);
    if (bytes_written != 1) {
        if (bytes_written < 0)
            report_errno("release container init");
        else
            fprintf(stderr, "container: short write releasing container init\n");
        goto cleanup;
    }
    if (close(c->sync[1]) != 0) {
        report_errno("close parent sync write end");
        c->sync[1] = -1;
        goto cleanup;
    }
    c->sync[1] = -1;
    pipe_created = 0;
    if (wait_for_pid(child, &status) != 0)
        goto cleanup;
    child = -1;
    result = status_code(status);

cleanup:
    if (pipe_created) {
        if (c->sync[0] >= 0)
            close(c->sync[0]);
        if (c->sync[1] >= 0)
            close(c->sync[1]);
        c->sync[0] = c->sync[1] = -1;
    }
    if (child > 0) {
        kill(child, SIGKILL); /* SIGKILL works even for namespace PID 1. */
        (void)wait_for_pid(child, &status);
    }
    if (network_attempted)
        (void)container_net_host_teardown(c);
    if (c->cg_path[0])
        (void)container_cleanup(c);
    free(stack);
    return result;
}

/*
 * What: Removes the now-empty cgroup directory for this container.
 * Called by: container_run() after the cloned init is reaped.
 * Important: ENOENT is harmless because prior cleanup may have removed it.
 */
int container_cleanup(struct container *c)
{
    if (!c->cg_path[0])
        return 0;
    if (rmdir(c->cg_path) != 0 && errno != ENOENT)
        return report_errno("remove container cgroup");
    return 0;
}
