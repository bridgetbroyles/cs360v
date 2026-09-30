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
#include <fcntl.h>      /* open() and its O_CREAT / O_WRONLY / O_CLOEXEC flags  */
#include <signal.h>     /* SIGCHLD, the signal clone() asks for when the child exits */
#include <sys/mount.h>  /* mount(), umount2() and the MS_* / MNT_* flags        */
#include <sys/stat.h>   /* mkdir(), for creating the container's cgroup folder  */
#include <sys/syscall.h>/* SYS_pivot_root, for calling pivot_root via syscall() */
#include <sys/wait.h>   /* waitpid() and the WIFEXITED / WEXITSTATUS helpers    */
#include <sys/ioctl.h>  /* ioctl(), the "send a device a command" system call   */
#include <sys/prctl.h>  /* prctl(), for PR_CAPBSET_DROP and PR_SET_NO_NEW_PRIVS */
#include <linux/capability.h> /* CAP_LAST_CAP and the structs capset() takes  */
#include <linux/filter.h>     /* struct sock_filter and the BPF_* instruction macros */
#include <linux/seccomp.h>    /* struct seccomp_data and the SECCOMP_* constants */
#include <linux/audit.h>      /* AUDIT_ARCH_*, the ids for each CPU architecture */
#include <stddef.h>           /* offsetof(), to find fields inside seccomp_data */
#include <sys/socket.h> /* socket(), which gives us a handle to talk to the network stack */
#include <net/if.h>     /* struct ifreq and the IFF_UP / IFF_RUNNING flags      */
#include <net/route.h>  /* struct rtentry and RTF_* flags, for adding a route   */
#include <netinet/in.h> /* struct sockaddr_in, the form an IPv4 address takes   */
#include <arpa/inet.h>  /* inet_pton() and htonl(), for converting addresses    */

/* ---- Part I: namespaces ----------------------------------------------- */

/* Returns the set of namespaces the container gets its own private copy of.
 *
 * A "namespace" is the kernel's way of giving a process its own view of some
 * shared resource. Each CLONE_NEW* flag below asks clone() to create a fresh,
 * empty copy of one kind of resource for the new process, instead of letting
 * it share the host's.
 *
 * The flags are single bits, so OR-ing them together (the `|` operator)
 * combines them into one number that says "all of these at once". */
int container_namespaces(void)
{
    return CLONE_NEWUSER   /* its own user ids: root inside is NOT root outside  */
         | CLONE_NEWPID    /* its own process ids: our init becomes PID 1        */
         | CLONE_NEWNS     /* its own mount table: mounts we make stay inside    */
         | CLONE_NEWUTS    /* its own hostname                                   */
         | CLONE_NEWNET;   /* its own network stack: starts with only loopback   */
}

/* Tells the kernel how user/group ids inside the container line up with real
 * ids outside it. This runs in the PARENT (the runtime), not in the container.
 *
 * Why this is needed: a brand-new user namespace starts with an EMPTY mapping,
 * meaning no id inside it corresponds to anything outside. Until we fill the
 * map in, the container's process is effectively "nobody" and cannot do much.
 *
 * Each map file takes a line of the form "<inside-id> <outside-id> <count>".
 * We write "0 <our-id> 1", which reads as: "starting at id 0 inside, map 1 id
 * onto <our-id> outside". So the container's root (id 0) is really just us. */
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;            /* not needed here; this silences the "unused" warning */

    /* Scratch buffers to build each file path and each line we write.
     * 64 bytes is plenty for "/proc/<pid>/setgroups" and "0 <id> 1". */
    char path[64];
    char mapping[64];

    /* --- User ids -------------------------------------------------------
     * getuid() is the real user id of whoever ran this runtime. We map the
     * container's root (0) onto that id, with a range of just 1 id. */
    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
    if (write_file(path, mapping) < 0)
        return -1;      /* write_file() already printed what went wrong */

    /* --- setgroups ------------------------------------------------------
     * The kernel refuses to let us write gid_map until we have turned off the
     * setgroups() call for this namespace. (Otherwise the container could use
     * setgroups() to drop a group that was restricting it on the host.)
     * Writing "deny" here switches it off, which unlocks gid_map. */
    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    /* --- Group ids ------------------------------------------------------
     * Same idea as the user ids: the container's group 0 is our real group. */
    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    return 0;           /* all three files written: the child can now act as root inside */
}

/* ---- Part V: cgroup --------------------------------------------------- */

/* Writes one limit into a file in the container's cgroup, for example
 * pids.max or memory.max.
 *
 * The cgroup files take either a number or the word "max" (meaning "no
 * limit"). Our config uses -1 (any negative number) to mean "no limit", so
 * this converts: a negative `limit` is written as "max", anything else as the
 * number itself. `file_name` is the file inside c->cg_path to write to.
 *
 * Returns 0 on success, -1 on failure (write_file() prints why). */
static int write_cgroup_limit(struct container *c, const char *file_name, long limit)
{
    char path[PATH_MAX + 32];   /* the cgroup folder plus "/<file_name>"        */
    char value[32];             /* a long is at most 20 digits, so 32 is plenty */

    snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);
    if (limit < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", limit);   /* %ld = print a long */
    return write_file(path, value);
}

/* SPEC Part V: creates this container's cgroup and sets its limits. Runs in
 * the parent, before the container starts.
 *
 * Background: a "control group" (cgroup) is how Linux limits how much of the
 * machine a group of processes may use. On cgroup v2, every cgroup is simply a
 * folder under /sys/fs/cgroup. The files inside it are not real files. They
 * are settings: writing a number into memory.max, for example, makes the
 * kernel enforce that memory limit on every process in the cgroup.
 *
 * Each kind of limit is handled by a "controller" (pids, memory, cpu, ...),
 * and a controller only works in a cgroup if its PARENT cgroup has turned it
 * on for its children ("delegated" it). So the steps are:
 *   1. turn on the pids and memory controllers in the base (parent) cgroup;
 *   2. create our own cgroup folder under it;
 *   3. write our limits into that folder.
 *
 * Returns 0 on success, -1 on failure. */
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    /* --- 1. Delegate the controllers ------------------------------------------
     * cgroup.subtree_control lists the controllers a cgroup turns on for its
     * children. Writing "+pids +memory" adds those two (the "+" means "turn
     * on"). If they are already on, writing this again is harmless. */
    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    /* --- 2. Create our cgroup folder ------------------------------------------
     * The folder is named after the container (c->name, set with --name), and
     * we save its full path in c->cg_path, because container_cgroup_enter()
     * and container_cleanup() need it later. Creating a folder here is all
     * it takes: the kernel fills it with the settings files automatically.
     *
     * 0755 is the usual folder permission: the owner can change it, and
     * everyone can look inside.
     *
     * EEXIST ("already exists") is treated as success. If an earlier run
     * crashed or was killed before it could clean up, its folder is still
     * here, and the SPEC requires that the next run still works. An empty
     * leftover cgroup is just as good as a new one, because we overwrite all
     * its limits in step 3 anyway. */
    snprintf(c->cg_path, sizeof c->cg_path, "%s/%s", c->cgroup_base, c->name);
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    /* --- 3. Write the limits ------------------------------------------------
     * pids.max:        how many processes (and threads) the container may
     *                  have at once. Past that, fork() fails with EAGAIN,
     *                  which is what stops a "fork bomb".
     * memory.max:      how many bytes of memory it may use. Past that, the
     *                  kernel's "OOM (out-of-memory) killer" kills a process.
     * memory.swap.max: how much it may push out to swap (disk). We set 0.
     *                  Otherwise, hitting memory.max would just move memory
     *                  to disk instead of stopping the process, and the limit
     *                  wouldn't really limit anything. */
    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
        return -1;

    return 0;
}

/* SPEC Part V: puts the container's init process into the container's cgroup.
 * Runs in the parent, after clone() and before the child is released.
 *
 * cgroup.procs lists the processes in a cgroup. Writing a PID into it MOVES
 * that process into the cgroup. From then on, every process it creates (its
 * children, their children, and so on) starts in the same cgroup. So moving
 * just the init is enough to put the whole container under the limits. We do
 * it before releasing the child, so the command can never run unlimited, not
 * even for a moment.
 *
 * Returns 0 on success, -1 on failure. */
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX + 32];   /* the cgroup folder plus "/cgroup.procs"      */
    char pid_text[16];          /* the PID as text; PIDs are at most 7 digits  */

    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);
    return write_file(path, pid_text);
}

/* ---- Parts I/II/III: isolation, run inside the container init --------- */

/* Makes one of the host's device files (for example /dev/null) appear inside
 * the container at <rootfs>/dev/<name>.
 *
 * Background: a "device node" like /dev/null is a special file that talks to
 * a driver in the kernel. Normally you create one with mknod(), but the kernel
 * forbids mknod() inside a user namespace (otherwise a container could make
 * itself a device node for the host's hard disk). So instead we "bind mount"
 * the host's existing node into the container. A bind mount makes one path
 * show the exact same file as another path, like a window onto it.
 *
 * `rootfs` is the container's root folder, `name` is e.g. "null" or "zero".
 * Returns 0 on success, -1 on failure (after printing why). */
static int bind_device(const char *rootfs, const char *name)
{
    /* PATH_MAX (from <limits.h>) is the longest path Linux allows, so these
     * buffers can hold any path we might build. */
    char host_path[PATH_MAX];      /* the real device, e.g. "/dev/null"          */
    char target_path[PATH_MAX];    /* where it goes, e.g. "rootfs/dev/null"      */
    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    /* A bind mount needs something already at the target path to sit on top
     * of. Our /dev is a brand-new empty tmpfs, so we create an empty file:
     *   O_CREAT   - create the file if it does not exist
     *   O_WRONLY  - open() needs some access mode; write-only is fine
     *   O_CLOEXEC - close this descriptor automatically if we exec(); it is
     *               ours, so we don't want the command to inherit it
     *   0666      - permissions: readable and writable by everyone, which is
     *               what /dev/null and /dev/zero normally have */
    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "container: create %s: %s\n", target_path, strerror(errno));
        return -1;
    }
    close(fd);      /* we only needed the file to exist, not to write to it */

    /* Now lay the host's real device over the empty file. MS_BIND means "make
     * target_path show host_path". The filesystem-type and data arguments are
     * unused for a bind mount, so they are NULL. */
    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0) {
        fprintf(stderr, "container: bind %s: %s\n", host_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* SPEC Part II: builds the container's filesystem and then switches the
 * container into it, so that from inside, `rootfs` looks like "/".
 *
 * All the mounts here happen inside the container's own mount namespace
 * (CLONE_NEWNS), so the host never sees any of them.
 *
 * Each step depends on the one before it, so the order matters. Every step
 * prints a "container: ..." error and returns -1 if it fails. (The test
 * harness ignores lines starting with "container: ", so these messages don't
 * confuse it.) */
static int setup_filesystem(const char *rootfs)
{
    /* Scratch buffer for building paths like "<rootfs>/tmp". */
    char path[PATH_MAX];

    /* --- Step 1: make our mounts private ---------------------------------
     * Linux can set up mounts to be "shared", meaning a mount made in one
     * namespace gets copied into others. Ubuntu shares "/" by default. If we
     * left it that way, the mounts below could leak back out to the host.
     * MS_PRIVATE turns sharing off, and MS_REC applies it to every mount
     * under "/" too, not just "/" itself. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }

    /* --- Step 2: turn the rootfs folder into a read-only mount ------------
     * pivot_root() (step 6) only accepts a *mount point* as the new root, and
     * a plain folder isn't one. Bind-mounting the folder onto itself turns it
     * into a mount point without changing what's in it. MS_REC also brings
     * along anything already mounted inside that folder. */
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        fprintf(stderr, "container: bind rootfs: %s\n", strerror(errno));
        return -1;
    }
    /* Now change that new mount's settings to read-only. MS_REMOUNT means
     * "change an existing mount instead of making a new one", MS_BIND says
     * it is the bind mount we just made, and MS_RDONLY is the setting we want.
     * From here on, nothing in the container can modify its own image. */
    if (mount(NULL, rootfs, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        fprintf(stderr, "container: remount rootfs read-only: %s\n", strerror(errno));
        return -1;
    }

    /* --- Step 3: a writable /tmp -----------------------------------------
     * The root is read-only now, but programs expect to be able to write
     * temporary files. tmpfs is a filesystem that lives entirely in memory,
     * so we mount a fresh, empty, writable one on <rootfs>/tmp. It vanishes
     * when the container exits. (The first argument, "tmpfs", is just a label
     * that shows up in mount listings.) Mounting on top of a folder in a
     * read-only filesystem is allowed; only changing its files is not. */
    snprintf(path, sizeof path, "%s/tmp", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /tmp: %s\n", strerror(errno));
        return -1;
    }

    /* --- Step 4: a minimal /dev ------------------------------------------
     * We want the container to see only /dev/null and /dev/zero, nothing
     * else from the host. So we cover <rootfs>/dev with a fresh empty tmpfs
     * (this hides whatever was there before), then bind in just those two
     * devices from the host. See bind_device() above for why we bind instead
     * of creating them. */
    snprintf(path, sizeof path, "%s/dev", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /dev: %s\n", strerror(errno));
        return -1;
    }
    /* `||` stops at the first failure, so if "null" fails we don't try "zero". */
    if (bind_device(rootfs, "null") < 0 || bind_device(rootfs, "zero") < 0)
        return -1;

    /* --- Step 5: our own /proc -------------------------------------------
     * /proc is a fake filesystem the kernel generates, listing processes.
     * A fresh proc mount shows the processes of the PID namespace we are in,
     * which is the container's (CLONE_NEWPID), so `ps` inside will only list
     * the container's own processes.
     *
     * This MUST happen before step 6. The kernel only lets a user namespace
     * mount a new /proc if a full /proc is already visible to it (so it can't
     * use a new one to see things it couldn't before). The host's /proc is
     * visible right now, but step 6 hides it, after which this would fail
     * with EPERM ("operation not permitted"). */
    snprintf(path, sizeof path, "%s/proc", rootfs);
    if (mount("proc", path, "proc", 0, NULL) < 0) {
        fprintf(stderr, "container: mount /proc: %s\n", strerror(errno));
        return -1;
    }

    /* --- Step 6: switch roots --------------------------------------------
     * pivot_root(new_root, put_old) makes new_root the new "/" and moves the
     * old root to put_old. The trick used here, from the SPEC:
     *   - chdir() into the rootfs, so "." means the rootfs;
     *   - pivot_root(".", "."): the new root is ".", and the old root gets
     *     stacked on top of it at "." as well;
     *   - umount2(".", MNT_DETACH) then peels that old root off the top,
     *     leaving just the rootfs underneath. MNT_DETACH means "detach it now
     *     even if something is still using it", which is what we want.
     * After this the host's files are completely gone from the container's
     * view.
     *
     * glibc has no pivot_root() function, so we ask the kernel directly
     * through the generic syscall() function with its system call number. */
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
    /* Finally move to the top of the new root, so the command starts in "/"
     * and not in some leftover position from before the switch. */
    if (chdir("/") < 0) {
        fprintf(stderr, "container: chdir /: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* SPEC Part III: takes away every special power ("capability") the
 * container's root has, so that being root inside the container stops meaning
 * anything.
 *
 * Background: Linux splits root's powers into about 40 separate capabilities,
 * each numbered from 0 up to CAP_LAST_CAP. For example CAP_NET_ADMIN (change
 * network settings), CAP_SYS_ADMIN (mount filesystems and much more), and
 * CAP_KILL (signal any process). A process has several SETS of these:
 *   - effective:   the ones it is actually using right now. This is what the
 *                  kernel checks when the process tries something privileged;
 *   - permitted:   the ones it is allowed to switch on in `effective`;
 *   - inheritable: the ones it can pass on through exec();
 *   - bounding:    a ceiling. No capability outside this set can ever be
 *                  gained again, not even by running a program marked to
 *                  grant it.
 * Our init is root in its user namespace, so it starts with all of them.
 *
 * We remove them in three stages, and the order matters:
 *   1. empty the bounding set. Removing an entry from it needs CAP_SETPCAP,
 *      which stage 2 takes away, so this must come first;
 *   2. clear effective, permitted and inheritable, all at once;
 *   3. set "no new privileges", so exec()ing a setuid program can't hand
 *      privileges back.
 * The command we start afterwards inherits this powerless state.
 *
 * Returns 0 on success, -1 on failure. */
static int drop_capabilities(void)
{
    /* --- 1. Empty the bounding set ------------------------------------------
     * PR_CAPBSET_DROP removes one capability from the bounding set, so we go
     * through every number from 0 to CAP_LAST_CAP (the highest one our
     * headers know about).
     *
     * EINVAL ("invalid argument") means the running kernel doesn't have a
     * capability with that number (for example, if the kernel is older than
     * our headers). There's nothing to drop then, so we skip it. Any other
     * error is a real failure. */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
            fprintf(stderr, "container: drop capability %d from bounding set: %s\n",
                    cap, strerror(errno));
            return -1;
        }
    }

    /* --- 2. Clear effective, permitted and inheritable -----------------------
     * capset() sets all three at once. glibc has no wrapper for it, so we call
     * it through syscall(), and it takes two arguments:
     *   - a header saying which format we're using and which process. Version
     *     3 is the current format; pid 0 means "this process";
     *   - the data: one struct per 32 capabilities, each holding the three
     *     sets as bit masks. There are more than 32 capabilities, so version 3
     *     uses TWO structs (capabilities 0-31, then 32-63).
     * We want every bit off, so we zero both structs with memset() and send
     * them as they are. */
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

    /* --- 3. No new privileges ------------------------------------------------
     * Normally, running a "setuid" program (one marked to run as its owner,
     * like `sudo`) can grant extra powers. PR_SET_NO_NEW_PRIVS switches that
     * off for this process and everything it starts, permanently. The
     * trailing zeros are unused arguments that prctl() requires to be 0. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Isolates the container from the inside. This runs in the container's init
 * process (PID 1), after the parent has written our id maps and released us,
 * and before we start the user's command.
 *
 * Order matters here: the steps near the top need root powers inside the
 * container, and the last steps (capabilities, seccomp, still TODO) take those
 * powers away. So everything that needs power has to come first.
 *
 * Returns 0 on success, -1 on failure. */
int container_setup(struct container *c)
{
    /* --- Hostname --------------------------------------------------------
     * Because the container has its own UTS namespace (CLONE_NEWUTS), this
     * renames only the container. The VM's own hostname is not affected.
     * sethostname() wants the name and its length in bytes. */
    if (sethostname(c->hostname, strlen(c->hostname)) < 0) {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    /* --- Loopback --------------------------------------------------------
     * Switch on "lo" so localhost works inside the container. This needs the
     * CAP_NET_ADMIN power, so it has to happen before capabilities are
     * dropped at the end of this function.
     *
     * The SPEC says this is best-effort: a container without loopback can
     * still run commands. So we deliberately ignore its return value (the
     * `(void)` says "yes, we mean to ignore it"). If it fails, it has already
     * printed why. */
    (void)container_network();

    /* --- The --net link (only if --net was given) -------------------------
     * Set up our end of the virtual cable the host plugged into us. This
     * also needs CAP_NET_ADMIN, so it too goes before the capability drop.
     *
     * Like the provided host side in net.c, this is best-effort: if it fails,
     * the error is printed but the command still runs, just without a working
     * connection to the host. So the return value is ignored here too. */
    if (c->net_enabled)
        (void)container_net_config(c);

    /* --- Filesystem ------------------------------------------------------
     * Build the container's own filesystem and switch into it. After this
     * returns, "/" inside the container is the rootfs folder. */
    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    /* --- Capabilities ------------------------------------------------------
     * Everything above needed root's powers (setting the hostname, the
     * network, every mount). Now that it's all done, take those powers away
     * for good, so the command runs as a powerless "root". */
    if (drop_capabilities() < 0)
        return -1;

    /* --- Seccomp ------------------------------------------------------------
     * Last of all: install the system call filter. It has to be the final
     * step, because it blocks calls like mount() and pivot_root() that the
     * steps above needed. The filter is passed on to the command when we
     * fork() and exec() it. */
    if (container_seccomp() < 0)
        return -1;

    return 0;
}

/* SPEC Part I: switches on the container's loopback interface, "lo".
 *
 * Background: a network "interface" is one connection point a machine can
 * send traffic through (like a Wi-Fi card or Ethernet port). "lo", the
 * loopback interface, is a fake one that just delivers traffic back to the
 * same machine. It is what makes "localhost" / 127.0.0.1 work.
 *
 * Because the container has its own network namespace (CLONE_NEWNET), it gets
 * its own fresh "lo", and the kernel creates it switched OFF ("down"). Until
 * we switch it on, even a program talking to itself over localhost fails.
 *
 * How: every interface has a set of on/off "flags". We read lo's current
 * flags, turn on the two that mean "up", and write them back. Linux does this
 * through ioctl(), a general-purpose "send a command to a device" system call.
 * These networking ioctls have to be sent through a socket, so we open one
 * purely as a handle to the network stack; it is never used to send data.
 *
 * Changing an interface needs the CAP_NET_ADMIN power, which our init has as
 * root inside the container, so container_setup() calls this BEFORE capabilities
 * are dropped. The SPEC calls this "best-effort": a container without loopback
 * still works, so we return -1 on failure but the caller doesn't abort.
 *
 * Returns 0 on success, -1 on failure. */
int container_network(void)
{
    /* --- Open a socket to send the commands through ------------------------
     * AF_INET means IPv4 and SOCK_DGRAM means the UDP style of socket; the
     * SPEC asks for this particular kind, which is the usual choice for these
     * ioctls. The last argument, 0, means "the default protocol for this kind".
     * On success socket() returns a file descriptor (a small number naming
     * the socket); on failure it returns -1. */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* --- Name the interface we're asking about ------------------------------
     * struct ifreq ("interface request") is the form these ioctls use: we
     * fill in which interface we mean (ifr_name), and the kernel reads or
     * fills in the other field, here the flags (ifr_flags).
     * memset() zeroes the whole struct first, so no leftover garbage bytes
     * get sent to the kernel. strncpy() copies in "lo" without overflowing
     * the name field, which holds IFNAMSIZ (16) bytes. */
    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);

    /* --- Read lo's current flags --------------------------------------------
     * SIOCGIFFLAGS = "Socket I/O Control: Get InterFace FLAGS". The kernel
     * writes lo's current flags into request.ifr_flags. We read them first so
     * that when we write them back below, we keep any flags already set
     * instead of wiping them out. */
    if (ioctl(sock, SIOCGIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: read lo flags: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* --- Turn on "up" and "running" -----------------------------------------
     * `|=` switches the given bits on and leaves every other bit unchanged.
     *   IFF_UP      - the interface is enabled (this is the one that matters)
     *   IFF_RUNNING - the interface is working and ready to pass traffic
     * The SPEC asks for both. */
    request.ifr_flags |= IFF_UP | IFF_RUNNING;

    /* --- Write the flags back -----------------------------------------------
     * SIOCSIFFLAGS = "...Set InterFace FLAGS": the kernel applies the flags
     * we put in request.ifr_flags to lo. After this, localhost works. */
    if (ioctl(sock, SIOCSIFFLAGS, &request) < 0) {
        fprintf(stderr, "container: bring lo up: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    /* We're done with the socket. Closing it also means the user's command
     * won't inherit an extra open descriptor it doesn't know about. */
    close(sock);
    return 0;
}

/* Fills in `addr` as an IPv4 socket address holding `ip_text`, which is an
 * address written as text, like "10.44.0.2".
 *
 * Background: the networking ioctls don't take addresses as text. They take a
 * `struct sockaddr`, a generic "some kind of network address" struct. For IPv4
 * we fill in the IPv4-specific version, `struct sockaddr_in`, which has:
 *   - sin_family: which kind of address it is (AF_INET = IPv4);
 *   - sin_addr:   the 4-byte address itself.
 * inet_pton() ("presentation to network") converts the text form into those 4
 * bytes, in the byte order networks use. It returns 1 on success.
 *
 * Returns 0 on success, -1 if `ip_text` isn't a valid IPv4 address. */
static int make_ipv4_address(struct sockaddr *addr, const char *ip_text)
{
    /* A sockaddr_in fits inside a sockaddr (they're the same size for IPv4),
     * so we can view the caller's sockaddr as a sockaddr_in and fill it in. */
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
    memset(ipv4, 0, sizeof *ipv4);     /* start clean: no leftover bytes     */
    ipv4->sin_family = AF_INET;         /* "this is an IPv4 address"          */
    if (inet_pton(AF_INET, ip_text, &ipv4->sin_addr) != 1) {
        fprintf(stderr, "container: bad IPv4 address '%s'\n", ip_text);
        return -1;
    }
    return 0;
}

/* SPEC Part I (--net only): sets up the container's end of its network link
 * to the host.
 *
 * Background: with --net, the provided container_net_host_setup() (net.c)
 * creates a "veth pair", which works like a virtual network cable with two
 * ends. One end stays on the host, plugged into a bridge called cvbr0 (a
 * virtual network switch) that has the address 10.44.0.1. The other end is
 * moved into the container's network namespace and named c->net_ifname
 * ("ceth0"). The picture from the SPEC:
 *
 *     host: [ cvbr0 10.44.0.1/24 ]---veth---[ ceth0 10.44.0.2/24 ] :container
 *
 * But the container's end arrives blank: no address, switched off, and the
 * container doesn't know where to send traffic. This function fixes all that,
 * with the same kind of ioctl() commands container_network() uses for lo:
 *   1. give ceth0 its IP address (c->net_ip, "10.44.0.2");
 *   2. give it a netmask built from c->net_prefix (24), which tells it that
 *      every 10.44.0.x address is directly reachable on this cable;
 *   3. switch it on;
 *   4. add a "default route" through the gateway c->net_gw (10.44.0.1, the
 *      host's bridge): "for any address you don't know how to reach, send it
 *      to the gateway". This lets the container reply to anyone who
 *      reaches it through the host.
 *
 * Needs the CAP_NET_ADMIN power, so container_setup() calls this before
 * capabilities are dropped. Returns 0 on success, -1 on failure. */
int container_net_config(struct container *c)
{
    /* Open a socket purely as a handle for sending the ioctl commands, the
     * same as in container_network(). */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "container: socket: %s\n", strerror(errno));
        return -1;
    }

    /* The request form that names which interface we mean ("ceth0"). We
     * reuse this one struct for steps 1 to 3, changing only the field that
     * each step needs. */
    struct ifreq request;
    memset(&request, 0, sizeof request);
    strncpy(request.ifr_name, c->net_ifname, IFNAMSIZ - 1);

    /* --- 1. Set the IP address ----------------------------------------------
     * SIOCSIFADDR = "Set InterFace ADDRess". The address goes in ifr_addr. */
    if (make_ipv4_address(&request.ifr_addr, c->net_ip) < 0)
        goto fail;
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        fprintf(stderr, "container: set %s address: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* --- 2. Set the netmask -------------------------------------------------
     * A prefix like "/24" means "the first 24 bits of the address identify
     * the local network". The netmask is the same idea written as an
     * address: 24 one-bits followed by 8 zero-bits = 255.255.255.0.
     *
     * To build it we start with 32 one-bits (0xFFFFFFFF) and shift left by
     * (32 - prefix), which pushes in that many zero-bits on the right.
     * A prefix of 0 is handled separately because shifting a 32-bit number
     * by 32 is not allowed in C. htonl() ("host to network, long") then puts
     * the bytes in network order, which is what the kernel expects. */
    struct sockaddr_in *netmask = (struct sockaddr_in *)&request.ifr_netmask;
    memset(netmask, 0, sizeof *netmask);
    netmask->sin_family = AF_INET;
    uint32_t mask_bits = 0;
    if (c->net_prefix > 0)
        mask_bits = 0xFFFFFFFFu << (32 - c->net_prefix);
    netmask->sin_addr.s_addr = htonl(mask_bits);
    /* SIOCSIFNETMASK = "Set InterFace NETMASK". */
    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
        fprintf(stderr, "container: set %s netmask: %s\n",
                c->net_ifname, strerror(errno));
        goto fail;
    }

    /* --- 3. Switch the interface on ---------------------------------------
     * Exactly like loopback: read the current flags (SIOCGIFFLAGS), turn on
     * "up" and "running", and write them back (SIOCSIFFLAGS). */
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

    /* --- 4. Add the default route ------------------------------------------
     * A route is a rule saying "to reach these addresses, send traffic here".
     * It's described with a struct rtentry:
     *   - rt_dst:     which addresses the rule covers. 0.0.0.0...
     *   - rt_genmask: ...with mask 0.0.0.0 means "every address", which is
     *                 what makes this the DEFAULT route (the catch-all);
     *   - rt_gateway: where to send that traffic: the host's bridge, 10.44.0.1;
     *   - rt_flags:   RTF_UP = the route is active; RTF_GATEWAY = send to the
     *                 gateway, not directly to the destination.
     * SIOCADDRT = "ADD RouTe". The interface is not named here: the kernel
     * works it out, because 10.44.0.1 is on ceth0's local network (step 2). */
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
    /* Every failure above jumps here with `goto`, so the socket is always
     * closed exactly once, no matter which step went wrong. */
    close(sock);
    return -1;
}

/* The id of the CPU architecture we are compiled for, as seccomp reports it.
 * The compiler defines __x86_64__ or __aarch64__ to say which CPU it is
 * building for, and #if picks the matching id at compile time. The course VM
 * on an Apple Silicon Mac is aarch64 (64-bit ARM); an Intel machine is x86_64.
 * On anything else, #error stops the build with a clear message. */
#if defined(__x86_64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "container_seccomp() only supports x86_64 and aarch64"
#endif

/* The system calls the container is NOT allowed to make: the SPEC's denylist.
 * Each __NR_* name is that system call's number on the CPU we're building for
 * (from <sys/syscall.h>). What each one could otherwise be used for:
 *   ptrace                 - spy on or control another process
 *   mount / umount2        - change what filesystems are attached where
 *   pivot_root / chroot    - change which folder counts as "/"
 *   setns / unshare        - join or create namespaces (to escape ours)
 *   reboot                 - restart or power off the machine
 *   swapon / swapoff       - change the machine's swap space
 *   kexec_load             - load and boot a different kernel
 *   init_module, finit_module, delete_module - load or remove kernel code */
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

/* How many entries denied_syscalls has: the array's total size in bytes
 * divided by the size of one entry. Adding a name above updates this. */
#define NUM_DENIED_SYSCALLS (sizeof denied_syscalls / sizeof denied_syscalls[0])

/* SPEC Part III: installs a seccomp filter that blocks the dangerous system
 * calls listed above.
 *
 * Background: a "system call" is how a program asks the kernel to do
 * something (open a file, start a process, mount a filesystem, ...). Seccomp
 * lets a process give the kernel a small filter PROGRAM that runs on every
 * system call it makes from then on, and decides whether to allow it. The
 * filter can never be removed, and it is passed on to every child and
 * through exec(), so it covers the user's command too.
 *
 * The filter is written in "BPF", a tiny instruction language the kernel
 * runs. It has one working register called A, which can hold one value.
 * The instructions we use:
 *   - BPF_LD  (load):  copy a field of the current system call into A. The
 *     kernel describes the call as a `struct seccomp_data`, whose fields
 *     include `arch` (which CPU convention was used) and `nr` (which system
 *     call it is, as a number);
 *   - BPF_JMP (jump):  compare A with a number and skip ahead by one amount
 *     if equal ("jt", jump-if-true) or another if not ("jf"). 0 means "go on
 *     to the very next instruction";
 *   - BPF_RET (return): stop and give the verdict: allow the call, fail it
 *     with an error, or kill the process.
 * BPF_STMT(...) writes an instruction with no jump, and BPF_JUMP(...) writes
 * a compare-and-jump. Both macros expand to a `{ ... }` initializer list, and
 * C only accepts that form when a variable is first declared, not in a later
 * `x = ...` assignment. Putting `(struct sock_filter)` in front turns the list
 * into a ready-made value (a "compound literal") that can be assigned, which
 * is why every instruction below starts with it.
 *
 * Our program looks like this:
 *     load arch
 *     if arch == our architecture, skip 1          (go past the kill)
 *     return KILL                                  (foreign convention)
 *     load nr
 *     if nr == ptrace, go on;  else skip 1         (go past the return)
 *     return ERRNO(EPERM)                          (ptrace is denied)
 *     if nr == mount,  go on;  else skip 1
 *     return ERRNO(EPERM)
 *     ...one pair like that for every denied call...
 *     return ALLOW                                 (not on the list)
 *
 * Why check the architecture? Some CPUs can run system calls from more than
 * one convention (for example, 64-bit x86 can also run old 32-bit calls), and
 * the same number means a DIFFERENT call in each one. Our list uses the
 * numbers for our main convention only, so a call from another convention
 * could sneak past it. Killing those outright closes that gap.
 *
 * Returns 0 on success, -1 on failure. */
int container_seccomp(void)
{
    /* Room for the whole program: 4 fixed instructions before the list
     * (load arch, compare, kill, load nr), 2 per denied call (compare,
     * return), and 1 at the end (allow). */
    struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1];
    size_t count = 0;   /* how many instructions we've written so far */

    /* --- Check the architecture ------------------------------------------------
     * offsetof(struct seccomp_data, arch) is where the `arch` field sits
     * inside seccomp_data. BPF_W means "load a 32-bit word" and BPF_ABS means
     * "from that exact position". */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                               offsetof(struct seccomp_data, arch));
    /* If A equals our architecture, skip 1 (the kill below); if not, skip 0,
     * which lands on the kill. BPF_K means "compare with this constant". */
    filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, OUR_AUDIT_ARCH, 1, 0);
    /* A foreign convention: kill the whole process immediately. */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);

    /* --- Load the system call number ------------------------------------------ */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                               offsetof(struct seccomp_data, nr));

    /* --- One compare + return pair per denied call ----------------------------
     * If A equals this call's number, go on to the next instruction (jt = 0),
     * which returns the error. If not, skip it (jf = 1) and land on the next
     * pair's compare.
     *
     * SECCOMP_RET_ERRNO means "don't run the call; make it fail with the
     * error number in the low bits". OR-ing in EPERM puts that number there.
     * `& SECCOMP_RET_DATA` keeps EPERM inside those low bits, as the kernel
     * requires. */
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++) {
        filter[count++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                   (unsigned int)denied_syscalls[i], 0, 1);
        filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
                                   SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }

    /* --- Anything not on the list is allowed ------------------------------------ */
    filter[count++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    /* The kernel takes the program as a length plus a pointer to the
     * instructions. */
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };

    /* --- No new privileges ------------------------------------------------------
     * The kernel only lets a process WITHOUT CAP_SYS_ADMIN install a filter if
     * "no new privileges" is on. That rule stops a filter from being used to
     * trick a setuid program into misbehaving. drop_capabilities() already
     * turned it on, but the SPEC asks for it here too. Setting it twice is
     * harmless, and it keeps this function correct if it's ever called on
     * its own. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }

    /* --- Install the filter -----------------------------------------------------
     * glibc has no seccomp() wrapper, so we call it through syscall().
     * SECCOMP_SET_MODE_FILTER means "install this BPF program"; the 0 is for
     * optional flags we don't need. From this line on, every system call
     * this process and its children make goes through the filter. */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0) {
        fprintf(stderr, "container: install seccomp filter: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Turns the raw `status` number that waitpid() fills in into the exit code a
 * shell would show (what `echo $?` prints).
 *
 * waitpid() packs several facts into one int, and the W* macros unpack it:
 *   - WIFEXITED:   true if the process ended normally (returned from main or
 *                  called exit). WEXITSTATUS then gives the code it exited
 *                  with, 0-255.
 *   - WIFSIGNALED: true if a signal killed it (e.g. SIGTERM, SIGKILL).
 *                  WTERMSIG then gives the signal's number. By shell
 *                  convention we report that as 128 + the signal number, so
 *                  a process killed by SIGTERM (15) reports 143.
 * The final `return 1` is a fallback for any other case; it shouldn't happen
 * with the way we call waitpid(). */
static int status_to_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

/* SPEC Part IV: the container's "init" process.
 *
 * This function runs in the child that clone() created, and because that
 * child is in a new PID namespace, it is PID 1 inside the container. PID 1 is
 * special in Linux: whenever a process's parent dies, the kernel hands the
 * orphan to PID 1, and PID 1 is responsible for "reaping" it (collecting its
 * exit status with waitpid()) when it finishes. A finished process that nobody
 * reaps stays around as a "zombie". So a proper init has to keep reaping.
 *
 * Its job, in order:
 *   1. wait for the parent to finish setting us up;
 *   2. isolate ourselves (container_setup);
 *   3. start the user's command as our child (so the command is PID 2);
 *   4. reap children until the command exits, then return its exit code.
 *
 * The value this returns becomes the init's exit code, which container_run()
 * passes on as the whole container's exit code. */
int container_init(struct container *c)
{
    /* --- 1. Wait to be released ------------------------------------------
     * c->sync is a pipe: a one-way channel where bytes written to sync[1]
     * come out of sync[0]. The parent will write one byte once it has written
     * our id maps (and later, put us in the cgroup). read() blocks, meaning it
     * pauses right here, until that byte arrives.
     *
     * We close sync[1] (the write end) first because we will never write to
     * it. This matters: if the parent fails and closes its write end without
     * writing, read() can only notice "nobody can ever write here again" (it
     * returns 0) once EVERY copy of the write end is closed, including ours.
     * Otherwise we would wait forever. */
    char release_byte;          /* where read() puts the byte; its value doesn't matter */
    close(c->sync[1]);
    if (read(c->sync[0], &release_byte, 1) != 1) {
        /* read() gave 0 (the parent closed the pipe without writing, which is
         * how it tells us setup failed) or -1 (an error). Either way, stop. */
        fprintf(stderr, "container: parent did not release the container\n");
        return 1;
    }
    close(c->sync[0]);          /* done with the pipe entirely */

    /* --- 2. Isolate ourselves ---------------------------------------------
     * Hostname, filesystem, and (later) network, capabilities, seccomp. */
    if (container_setup(c) < 0)
        return 1;

    /* --- 3. Start the command ---------------------------------------------
     * fork() makes a copy of this process. It returns TWICE: in the copy (the
     * child) it returns 0, and in the original (us, the parent) it returns the
     * child's PID. A negative value means the fork failed.
     *
     * Since we are PID 1, the first child we fork gets PID 2. */
    pid_t command_pid = fork();
    if (command_pid < 0) {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }
    if (command_pid == 0) {
        /* This branch runs only in the child. execvp() replaces this process
         * with the user's command, e.g. "/bin/busybox sh". c->argv[0] is the
         * program and c->argv is the full argument list, ending in NULL. The
         * "p" in execvp means it will also search PATH if the program name
         * has no slash in it.
         *
         * If execvp() succeeds, it never returns: this process IS the command
         * now. So reaching the next line means it failed (e.g. no such file). */
        execvp(c->argv[0], c->argv);
        fprintf(stderr, "container: exec %s: %s\n", c->argv[0], strerror(errno));
        /* 127 is the conventional "command not found" exit code (shells use it
         * too). We use _exit() rather than exit() because this is a forked copy:
         * _exit() quits immediately without running the parent's cleanup code
         * a second time. */
        _exit(127);
    }

    /* --- 4. Reap until the command exits -----------------------------------
     * Only the parent (init) gets here. waitpid(-1, ...) waits for ANY child
     * to finish, which includes orphans the kernel has handed to us, not just
     * the command. Each call collects one finished child, so no zombies build
     * up. We loop forever until the finished child is the command itself. */
    for (;;) {
        int status;                                 /* filled in by waitpid() */
        pid_t reaped = waitpid(-1, &status, 0);     /* the PID that finished  */
        if (reaped < 0) {
            /* EINTR means a signal interrupted the wait before any child
             * finished. That's harmless, so just wait again. */
            if (errno == EINTR)
                continue;
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }
        /* If it was an orphan, we've reaped it and just loop around. If it
         * was the command, we're done: return its exit code. When we (PID 1)
         * exit, the kernel automatically kills anything else still running in
         * the container. */
        if (reaped == command_pid)
            return status_to_exit_code(status);
    }
}

/* ---- the whole lifecycle: main.c calls only this ----------------------- */

/* clone() needs a function to run in the new child, and that function must
 * take a single `void *` argument and return an int. container_init() takes
 * a `struct container *` instead, so this small wrapper sits in between: it
 * receives the container as a `void *` and converts it back. Whatever it
 * returns becomes the child's exit code. */
static int child_entry(void *arg)
{
    return container_init((struct container *)arg);
}

/* The whole life of one container, start to finish. main.c calls this, and
 * whatever it returns becomes the runtime's exit code.
 *
 * Two processes are involved:
 *   - the PARENT is this runtime, running normally on the host;
 *   - the CHILD is created by clone() in fresh namespaces and becomes the
 *     container's init (see container_init above).
 * Some setup can only be done from outside (the parent writes the child's id
 * maps), so the child waits on a pipe until the parent says "go". */
int container_run(struct container *c)
{
    /* Keep the "container: " prefix on anything you print here: the test
     * harness reads the container's output and skips lines starting with it. */

    /* --- Create the cgroup (Part V) -----------------------------------------
     * Make the container's cgroup folder and write its limits first, before
     * anything else exists, so the limits are ready by the time the container
     * joins it. If this fails, nothing has been started yet, so we can simply
     * give up. */
    if (container_cgroup_init(c) < 0)
        return 1;

    /* --- The release pipe --------------------------------------------------
     * pipe() creates a one-way channel and stores its two ends in c->sync:
     * sync[0] for reading and sync[1] for writing. The child will block
     * reading sync[0] until we write to sync[1]. We create it BEFORE clone()
     * so that the child gets its own copy of both ends. */
    if (pipe(c->sync) < 0) {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        /* The cgroup folder already exists, so remove it before giving up.
         * The same goes for every early exit below. */
        container_cleanup(c);
        return 1;
    }

    /* --- The child's stack -------------------------------------------------
     * Unlike fork(), clone() makes us provide the memory the child uses for
     * its stack (where function calls keep their local variables). We
     * allocate CONTAINER_STACK_SIZE bytes (1 MiB, set in container.h). */
    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (stack == NULL) {
        fprintf(stderr, "container: out of memory for the child stack\n");
        /* Clean up what we've made so far: both ends of the pipe, and the
         * cgroup folder. */
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /* --- Create the container's init process -----------------------------
     * clone() is like fork(), but lets us choose what the child gets its own
     * copy of. Arguments:
     *   - child_entry: the function the child starts running;
     *   - stack + CONTAINER_STACK_SIZE: the child's stack. Stacks grow
     *     DOWNWARD in memory on x86 and ARM, so we pass the address of the
     *     END (top) of the buffer, not the start;
     *   - flags: the namespaces from container_namespaces(), plus SIGCHLD,
     *     which tells the kernel to notify us the normal way when the child
     *     exits, so that waitpid() below works for it;
     *   - c: passed to child_entry as its `void *arg`.
     * In the parent, clone() returns the child's PID (as seen from the host,
     * not the 1 it sees itself as). */
    pid_t child = clone(child_entry, stack + CONTAINER_STACK_SIZE,
                        container_namespaces() | SIGCHLD, c);
    if (child < 0) {
        fprintf(stderr, "container: clone: %s\n", strerror(errno));
        /* No child was created, so undo everything made before clone(). */
        free(stack);
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }

    /* --- Set the child up from the outside ----------------------------------
     * The child is now paused on the pipe. We record whether each setup step
     * worked in `setup_ok` (1 = yes, 0 = no) instead of returning right away,
     * because we still have to tell the child to quit and wait for it; just
     * returning would leave it stuck waiting on the pipe. */
    int setup_ok = (container_write_idmaps(c, child) == 0);

    /* Put the child into its cgroup, so its limits apply from the moment it
     * is released. `setup_ok &&` means: only try this if the id maps worked;
     * if they didn't, we're going to abort anyway. The result goes back into
     * setup_ok, so it stays 1 only if BOTH steps succeeded. */
    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    /* --- The host side of --net (only if --net was given) -------------------
     * The PROVIDED container_net_host_setup() (in net.c) creates the bridge
     * and the virtual cable (veth pair), keeps one end on the host, and moves
     * the other end into the child's network namespace. It has to run now:
     * after clone() (the child's namespace must exist to move a cable end
     * into it) and before we release the child (so the cable is there when
     * the child runs container_net_config()).
     *
     * net.c treats this as best-effort (it logs failures and carries on), so
     * we don't count a failure here against setup_ok. */
    if (c->net_enabled)
        (void)container_net_host_setup(c, child);

    /* --- Release the child -------------------------------------------------
     * We close our read end first since we never read from the pipe.
     * If setup went fine, we write one byte ("x"; any byte would do) and the
     * child's read() returns, letting it continue. If setup failed, we skip
     * the write. Closing our write end then makes the child's read() return
     * 0 ("end of file"), which it treats as "setup failed, exit". Either way,
     * the child never gets stuck. */
    close(c->sync[0]);
    if (setup_ok && write(c->sync[1], "x", 1) != 1) {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }
    close(c->sync[1]);

    /* --- Wait for the container to finish ----------------------------------
     * The child (the container's init) exits with the command's exit code.
     * waitpid() blocks until it does, and fills in `status`. If a signal
     * interrupts the wait (EINTR), we just wait again; any other error is
     * real. */
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            free(stack);
            return 1;
        }
    }
    /* The child is gone, so nothing uses its stack any more. (The child had
     * its own copy of our memory, so this frees only our copy.) */
    free(stack);

    /* --- Remove the host end of the --net cable ----------------------------
     * The container is gone, and its network namespace (and its end of the
     * cable) went with it. The PROVIDED teardown removes the host's end in
     * case anything is left over. The bridge itself stays for next time. */
    if (c->net_enabled)
        (void)container_net_host_teardown(c);

    /* --- Remove the cgroup (Part VI) -----------------------------------------
     * The container has finished, so its cgroup is empty and can go. If this
     * fails, it prints a warning, but we still return the command's exit code:
     * the command itself ran fine, and that's what the caller asked about. */
    (void)container_cleanup(c);

    /* If setup failed, the command never ran, so report a plain failure (1)
     * rather than whatever the child exited with. Otherwise, pass on the
     * command's own exit code (or 128 + signal if it was killed). */
    if (!setup_ok)
        return 1;
    return status_to_exit_code(status);
}

/* ---- Part VI: teardown ------------------------------------------------- */

/* SPEC Part VI: tidies up on the host after the container has finished.
 * Runs in the parent, after the container's init has been reaped.
 *
 * What's left to clean? Almost nothing, because the kernel already did most
 * of it: when the container's last process exited, its namespaces went away,
 * and the mounts made inside its mount namespace went with them. The one thing
 * that lives on after the processes is the cgroup folder we created in
 * container_cgroup_init(), so that's what we remove.
 *
 * A cgroup folder can only be removed once no processes are left in it, and
 * that's true now: our init has exited, and when PID 1 of a PID namespace
 * exits, the kernel kills and collects everything else in the container
 * before it finishes.
 *
 * Note that we use rmdir(), not a recursive delete. The settings files inside
 * a cgroup folder aren't real files and can't be deleted one by one; the
 * kernel removes them itself when the empty folder is removed.
 *
 * Returns 0 on success, -1 on failure. */
int container_cleanup(struct container *c)
{
    /* If the cgroup path was never filled in (for example, cgroup setup
     * failed before saving it), there is nothing of ours to remove. */
    if (c->cg_path[0] == '\0')
        return 0;

    /* ENOENT ("no such file or directory") means the folder is already gone,
     * e.g. someone removed it by hand. The goal is "the folder doesn't exist",
     * and it doesn't, so the SPEC says to treat that as success. */
    if (rmdir(c->cg_path) < 0 && errno != ENOENT) {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }
    return 0;
}
