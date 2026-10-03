# Foundations and Data Structures

## The central story

The program starts as a normal process on the Ubuntu host. It creates a child
with separate views of users, process IDs, mounts, hostname, and networking.
The parent performs setup that must happen from outside that child. The child
then builds its private filesystem, removes its powers, launches the requested
command, and remains alive as the container’s PID 1. When the command finishes,
the parent removes the remaining host-side resources.

There is no single Linux “make a container” function. This project combines:

- namespaces for isolated views of system resources;
- a user-ID map so root inside is an ordinary user outside;
- mounts and `pivot_root()` for a private filesystem;
- a cgroup for process and memory limits;
- capabilities and seccomp for restricting power;
- an init process for launching and reaping processes;
- a pipe so the parent can finish setup before the child continues.

## Parent, container init, and command

Three process roles matter:

```text
host runtime (parent, runs container_run)
└── container init (clone child, PID 1 inside, runs container_init)
    └── requested command (fork child, usually PID 2 inside)
```

The runtime and container init begin as two executions of the same program, but
the init is created with new namespaces. The init later uses `fork()` to create
the requested command. It does not directly replace itself with the command,
because PID 1 must remain available to adopt and reap orphan processes.

## `struct container`

`container.h` defines one structure that carries configuration and runtime state
through the entire call graph:

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

### Configuration fields filled by provided `main.c`

| Field | Meaning |
|---|---|
| `argv` | A NULL-terminated array containing the command and its arguments. `argv[0]` is the program name. |
| `name` | The container name, also used as the cgroup directory name. |
| `hostname` | The hostname visible inside the UTS namespace. |
| `rootfs` | Host path to the directory that will become `/` inside the container. |
| `pids_max` | Maximum number of processes; a negative value means unlimited. |
| `mem_max` | Maximum memory in bytes; a negative value means unlimited. |
| `cgroup_base` | Parent cgroup directory under which this container’s cgroup is created. |
| `net_enabled` | Nonzero only when the user supplied `--net`. |
| `net_ifname` | Interface name inside the container, such as `ceth0`. |
| `net_ip` | IPv4 address assigned inside the container. |
| `net_prefix` | Prefix length such as 24, equivalent to netmask `255.255.255.0`. |
| `net_gw` | Default gateway address on the host bridge. |

### Runtime fields filled during execution

| Field | Meaning |
|---|---|
| `sync[0]` | Read end of the parent-to-child synchronization pipe. |
| `sync[1]` | Write end of the synchronization pipe. |
| `cg_path` | Full path to the container’s cgroup; saved so cleanup can remove it. |
| `net_host_if` | Host-side veth name, filled by the provided networking code. |

The same structure is visible to the parent and the cloned child at creation
time. After `clone()`, each process has its own memory, just like `fork()`.
Therefore, parent changes made later are not general shared-memory updates. The
pipe is used for ordering, while the child already has the configuration values
it needs.

## Common C and Linux concepts

### File descriptors

Linux represents open files, sockets, and pipes with small integers called file
descriptors. A negative result usually means failure, and `errno` records why.
This file opens sockets, regular files, and a pipe, then closes each descriptor
when its job is finished.

### Return-value convention

Most setup functions return `0` for success and `-1` for failure. The two
lifecycle functions, `container_init()` and `container_run()`, instead return a
command-style exit code from 0 through 255. Zero normally means success.

### `errno` and `strerror(errno)`

When a system call fails, it normally returns `-1` and stores a reason code in
the process-global variable `errno`. `strerror(errno)` converts that number to a
readable message. Lines printed by the runtime begin with `container:` so the
test harness can distinguish diagnostics from command output.

### `PATH_MAX` arrays

Code such as `char path[PATH_MAX]` reserves a fixed stack buffer large enough
for a normal filesystem path. `snprintf()` writes formatted text into the array
without exceeding the stated capacity. Its return value should ideally be
checked for truncation; this implementation relies on the assignment’s bounded
input paths.

### `memset()`

Kernel-facing structures often contain fields the program does not use.
`memset(&value, 0, sizeof value)` starts every byte at zero so unused fields do
not contain unpredictable stack data.

### `ioctl()`

`ioctl(fd, request, pointer)` sends a device- or socket-specific command to the
kernel. Networking functions use it to read interface flags, assign addresses,
and add a route.

### Inheritance is central

The requested command inherits properties from container init:

- namespaces and cgroup membership;
- root filesystem and current mounts;
- hostname and network namespace;
- dropped capabilities;
- seccomp filter;
- open descriptors not already closed.

This inheritance is why setup happens before `fork()` and `execvp()`.

## Which code was provided

The assignment supplied `main.c`, `container.h`, `util.c`, and `net.c`.
Specifically:

- `main()` parses arguments, fills `struct container`, and calls
  `container_run()`;
- `write_file()` writes text into procfs/cgroup control files;
- `container_net_host_setup()` builds the host bridge and veth pair;
- `container_net_host_teardown()` removes the host-side network connection.

The student’s implementation is `container.c`: its 11 required functions plus
the 7 private helpers listed in the folder README.
