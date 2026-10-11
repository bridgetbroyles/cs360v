# Foundations: What the Runtime Is Building and Why

Read this file before the function-by-function documents. It introduces each
idea before using its Linux name.

## First: a container is not a virtual machine

A virtual machine emulates or virtualizes hardware. It normally boots a complete
guest operating-system kernel. The host and guest have separate kernels.

A container does **not** boot another kernel and does not emulate a CPU. A
containerized command is still a normal Linux process executing directly on the
host’s Linux kernel. The kernel is asked to give that process restricted views
of selected resources.

```text
Virtual machine                     Container
---------------                     ---------
host kernel                         host Linux kernel
└── virtual hardware                └── ordinary Linux process
    └── guest kernel                    with isolated views and restrictions
        └── guest processes
```

People sometimes describe a container as a “lightweight VM” because both can
provide an isolated environment. That is only an analogy. The implementation is
fundamentally different, and this difference explains why containers start
quickly and usually require fewer resources.

## What problem are we solving?

Suppose we run `/bin/sh` as an ordinary host process. By default it shares the
host’s process-ID numbering and process list, hostname, mounted filesystems,
user/group IDs, network interfaces, routing table, access to system calls, and
available memory/process capacity.

That is not a container. It is simply a program on the host.

This runtime starts a command but changes those relationships. The command gets
its own process-number view, hostname, mount table, and network stack. It receives
a chosen filesystem as `/`. Its process count and memory are limited. Finally,
its privileges and allowed system calls are reduced.

The host kernel “knows” it is managing all these processes, but knowledge is not
the issue. Isolation controls what the process itself can observe and change.
For example, the kernel knows both process A and process B exist while a PID
namespace can prevent A from seeing or signaling B.

## Namespaces: isolated views of specific resources

A namespace makes a group of processes see its own instance or view of one kind
of kernel-managed resource. It does not copy the entire computer.

This project creates five namespaces:

| Namespace flag | Resource whose view changes | What the container sees | Why isolation is needed |
|---|---|---|---|
| `CLONE_NEWUSER` | User/group identity mappings | UID 0 inside, mapped to an ordinary host user | Allows namespace-scoped setup without granting host-root authority. |
| `CLONE_NEWPID` | Process IDs and visible processes | Its init as PID 1 and only its descendants | Prevents inspection/signaling of unrelated host processes and gives the container its own init hierarchy. |
| `CLONE_NEWNS` | Mount table | Container-specific mounts and root filesystem | Lets the runtime build `/tmp`, `/dev`, and `/proc` without changing host mounts. |
| `CLONE_NEWUTS` | Hostname and NIS domain name | The requested container hostname | Gives software a separate machine identity without changing the host hostname. |
| `CLONE_NEWNET` | Interfaces, addresses, routes, and firewall state | Initially only its own disabled loopback | Prevents automatic use of host interfaces and permits a separately configured connection. |

“UTS” is a historical Unix term. For this project, the important fact is simple:
the UTS namespace gives the container its own hostname.

A namespace is not automatically a security boundary for every resource. Each
namespace covers a particular category, which is why the project also uses
cgroups, capabilities, and seccomp.

## Why a user-ID map is necessary

Linux permissions use numeric user IDs. UID 0 is root. Many setup operations—
changing a hostname, configuring an interface, and mounting filesystems—normally
require root-like capabilities.

We want the child to perform those operations **inside its new namespaces**, but
we do not want to give it host root.

A new user namespace gives the same process two related identities:

```text
inside the user namespace: UID 0 (container root)
outside on the host:       your ordinary UID, for example 501 or 1000
```

The mapping does not fool or hide anything from the host. The host kernel creates
and enforces it. It deliberately grants UID 0 authority only over resources
governed by that user namespace and associated namespaces. From the host’s point
of view, the process remains the ordinary unprivileged user.

Without an ID map, a new user namespace begins with no usable mapping. The child
would not have a valid identity for setup and could do almost nothing. The parent
therefore writes `0 <host UID> 1`, meaning: begin at ID 0 inside, map one ID to
`<host UID>` outside.

Container root does not merely *seem* ordinary. When it reaches outside the
namespace’s scope, it actually has only the host permissions of the mapped user.

## Why the container needs a filesystem

A process cannot usually run from memory alone. It needs an executable and often
libraries, configuration, commands, and conventional paths such as `/proc`,
`/dev`, and `/tmp`.

The filesystem is not an entire virtual disk or full operating-system install.
For this project, `make-rootfs.sh` builds a small directory tree based mostly on
statically linked BusyBox. The runtime makes that directory appear as `/` inside
the container.

Disk storage and RAM are different measurements:

- The rootfs occupies storage in host files.
- It does not all load into memory at once.
- Linux loads only executable pages and file data that processes actually use,
  and the host kernel may share read-only pages/cache.
- `/tmp` and `/dev` use small in-memory `tmpfs` mounts, but grow only as content
  is created.

Containers are often small because they share the host kernel and can use minimal
root filesystems. They still need a filesystem tree so `/bin/runner` has meaning
and so the process cannot browse the host’s `/`.

This project makes the rootfs read-only, adds writable `/tmp`, exposes only
`/dev/null` and `/dev/zero`, mounts a process view at `/proc`, then uses
`pivot_root()` to make the rootfs `/` and detach the host root.

## What a cgroup is

“cgroup” means **control group**. It is a Linux kernel mechanism that groups
processes for resource accounting and limits.

A cgroup is represented through a special virtual filesystem. Creating a child
cgroup looks like creating a directory. Files inside configure behavior:

```text
pids.max          maximum number of processes
memory.max        maximum memory usage
memory.swap.max   maximum swap usage
cgroup.procs      PIDs belonging to the group
```

Writing container init’s PID to `cgroup.procs` places it in the group. Children
inherit membership, so the command and its descendants receive the same limits.
Namespaces answer “what can this process see?” Cgroups answer “how many resources
may this process group consume?” They solve different problems.

## What seccomp is

Programs ask the Linux kernel to perform operations through **system calls** such
as `open`, `read`, `mount`, or `reboot`.

Seccomp means **secure computing**. It installs a kernel-enforced filter that
examines each system call before the kernel performs it. This project uses a
denylist. Ordinary calls continue, while dangerous calls such as `mount`,
`pivot_root`, `ptrace`, and `reboot` fail with `EPERM` (“operation not permitted”).

The filter is inherited by children and remains after `execvp()`, so the command
cannot remove it. Seccomp differs from capabilities:

- capabilities decide whether a process has categories of root power;
- seccomp can block a system call itself, regardless of user.

The project uses both as layers of defense.

## Why there are multiple child processes

Yes, there can be multiple children and descendants.

At minimum, this project has three process roles:

```text
host runtime (parent; runs container_run)
└── container init (clone child; PID 1 inside; runs container_init)
    └── requested command (fork child; normally PID 2 inside)
```

The requested command may create more processes. If one becomes orphaned, Linux
reparents it to PID 1 inside the namespace.

### Why not make container init directly become the command?

`execvp()` replaces its caller. If init directly called it, the requested command
would become PID 1 and no separate supervisor would remain.

PID 1 adopts orphan processes and must call `waitpid()` to collect their exit
information. Ordinary programs often do not do this correctly. Uncollected exited
processes become zombies.

Therefore init calls `fork()`:

- the fork child calls `execvp()` and becomes the requested command;
- the original init stays PID 1 and reaps children/orphans;
- when the main command finishes, init returns its exit status.

## What networking means here

Yes, the project performs real networking, but only optional, deliberately
configured connectivity.

An **interface** is a kernel networking endpoint with a name, addresses, and
state. Inside this container:

- `lo` is private loopback for `localhost` communication;
- `ceth0` is the usual optional container-side virtual Ethernet interface;
- `cvbr0` is a bridge on the host connected to the other end.

Without `--net`, the container has only loopback. Processes inside can use
`localhost`, but there is no configured path to the host or outside network.

With `--net`, provided code creates two connected virtual interfaces, like two
ends of a virtual cable:

```text
host bridge cvbr0 (10.44.0.1)
        ⇅ virtual Ethernet pair
container interface ceth0 (10.44.0.2)
```

The host helper creates and moves the interface. Our child code assigns its IP,
netmask, default route, and turns it on. The network test uses this connection to
reach a server running in the container.

## What the synchronization pipe is

A pipe is a kernel byte stream with two file descriptors:

```text
sync[0] = read end
sync[1] = write end
```

The timing problem is:

1. The parent cannot write `/proc/<child>/uid_map` until `clone()` returns the
   child PID.
2. The child must not continue before the ID map, cgroup membership, and optional
   host networking are ready.

The child immediately reads one byte from `sync[0]`. With no byte available, the
kernel puts it to sleep without busy-waiting. The parent finishes, writes one byte
to `sync[1]`, and the child wakes.

If parent setup fails, it closes the write end without sending a byte. The child
reads end-of-file and exits rather than running partially configured. The pipe
carries almost no data; its purpose is ordering and failure signaling.

## The complete central story

1. Provided `main()` parses options into `struct container` and calls
   `container_run()`.
2. The parent creates cgroup limits and a synchronization pipe.
3. `clone()` creates container init with five namespaces.
4. Init blocks on the pipe.
5. The parent maps container root to the caller’s UID/GID, moves init into the
   cgroup, and optionally constructs host networking.
6. The parent writes one byte to release init.
7. Init sets hostname/networking, enters the rootfs, drops capabilities, and
   installs seccomp.
8. Init forks. The new child becomes the command through `execvp()`; init remains
   PID 1 to reap children.
9. The host parent waits, removes network/cgroup state, and returns the command’s
   exit status.

## `struct container`: the plan for one run

```c
struct container {
    char *const *argv;
    const char *name;
    const char *hostname;
    const char *rootfs;
    long pids_max;
    long mem_max;
    const char *cgroup_base;
    int net_enabled;
    const char *net_ifname;
    const char *net_ip;
    int net_prefix;
    const char *net_gw;
    int sync[2];
    char cg_path[PATH_MAX];
    char net_host_if[16];
};
```

| Field | Meaning |
|---|---|
| `argv` | NULL-terminated command and arguments; `argv[0]` is the executable. |
| `name` | Container identifier and cgroup-directory name. |
| `hostname` | Name placed in the private UTS namespace. |
| `rootfs` | Host directory that becomes container `/`. |
| `pids_max` | Maximum process count; negative means unlimited. |
| `mem_max` | Maximum memory bytes; negative means unlimited. |
| `cgroup_base` | Parent directory in the cgroup virtual filesystem. |
| `net_enabled` | Nonzero when `--net` was requested. |
| `net_ifname` | Child interface name, normally `ceth0`. |
| `net_ip` | Child interface address, normally `10.44.0.2`. |
| `net_prefix` | Prefix length; 24 corresponds to `255.255.255.0`. |
| `net_gw` | Host bridge/default gateway, normally `10.44.0.1`. |
| `sync[0]`, `sync[1]` | Read and write ends of the release pipe. |
| `cg_path` | Full created cgroup path used by enter and cleanup. |
| `net_host_if` | Host-side veth name set by provided networking code. |

## C/Linux vocabulary used later

- **File descriptor:** small integer representing an open file, socket, or pipe
  endpoint in one process.
- **System call:** controlled request from a program to the kernel.
- **`errno`:** code explaining a failed operation; `strerror(errno)` makes it
  readable.
- **`PATH_MAX`:** constant used to size path-character arrays.
- **`memset(&x, 0, sizeof x)`:** initializes every structure byte to zero before
  sending it to the kernel.
- **`ioctl()`:** general device/socket-control system call; here it configures
  network interfaces and routes.
- **Inheritance:** children inherit namespaces, cgroup membership, mounts,
  restrictions, and seccomp. Setup in init therefore controls the command.

Most setup functions return `0` on success and `-1` on failure.
`container_init()` and `container_run()` instead return a command exit code.

## What was provided versus implemented

Provided code includes `main()` in `main.c`, `write_file()` in `util.c`, the two
host-network functions in `net.c`, `container.h`, build scripts, rootfs builder,
and tests. The specification required 11 functions in `container.c`. Helen’s
implementation added 7 private helpers. The folder README lists both groups.
