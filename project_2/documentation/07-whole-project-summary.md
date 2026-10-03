# Whole Project Summary

## What this project builds

Project 2 builds a small Linux container runtime. The user gives it a root
filesystem and a command. The runtime starts the command with isolated views of
users, process IDs, mounts, hostname, and networking; limits its resource use;
removes privileges; filters dangerous system calls; reports the command’s exit
status; and cleans up afterward.

A container is not a miniature virtual machine. There is no emulated CPU and no
separate guest kernel. Container processes execute directly on the host Linux
kernel, but kernel features change what they can see and do.

## What was provided and what was implemented

The course provided:

- `main.c`: parses arguments and fills `struct container`;
- `container.h`: defines the structure and required function declarations;
- `util.c`: implements `write_file()`;
- `net.c`: implements host-side veth setup and teardown;
- the rootfs builder, runner, and tests.

The specification required 11 functions in `container.c`. Helen’s implementation
completed all 11 and introduced 7 private helpers to organize repeated or lengthy
work. The official Ubuntu test suite reports **32 passed, 0 failed** for this
branch.

## The complete lifecycle

```text
provided main()
└── container_run(c)                         parent on host
    ├── container_cgroup_init(c)
    │   └── write_cgroup_limit(...)          added helper
    ├── pipe(c->sync)
    ├── allocate clone stack
    ├── clone(child_entry, flags, c)
    │   └── child_entry(c)                    added adapter
    │       └── container_init(c)             PID 1 inside container
    │           ├── block on sync pipe
    │           ├── container_setup(c)
    │           │   ├── sethostname(...)
    │           │   ├── container_network()
    │           │   ├── container_net_config(c), if --net
    │           │   │   └── make_ipv4_address(...)
    │           │   ├── setup_filesystem(rootfs)
    │           │   │   └── bind_device(...) twice
    │           │   ├── drop_capabilities()
    │           │   └── container_seccomp()
    │           ├── fork()
    │           │   └── command child: execvp(argv[0], argv)
    │           └── waitpid(-1) loop; reap and return command status
    ├── container_write_idmaps(c, child)
    ├── container_cgroup_enter(c, child)
    ├── provided container_net_host_setup(), if --net
    ├── write one byte to release child
    ├── waitpid(container init)
    ├── provided container_net_host_teardown(), if --net
    ├── container_cleanup(c)
    └── return command status
```

## Step-by-step story

### 1. `main()` prepares configuration

Provided `main.c` reads the command line and records values such as rootfs path,
hostname, cgroup limits, network settings, and command arguments in one
`struct container`. It calls `container_run()`.

### 2. The parent prepares resource control

`container_cgroup_init()` enables the pids and memory controllers, creates a
directory named for this container, and writes the requested limits. This is
done before the child is created so failures occur early and the child can be
placed under limits before it runs.

### 3. The parent creates a synchronization pipe

The parent must modify the child’s user-ID maps and cgroup membership after
`clone()` gives it a PID, but the child must not begin privileged setup before
those changes finish. The pipe solves this ordering problem. The child blocks on
a one-byte read; the parent releases it only after setup succeeds.

### 4. `clone()` creates isolated container init

`container_namespaces()` selects user, PID, mount, UTS, and network namespaces.
`clone()` starts `child_entry()` on an explicitly allocated stack. Inside its PID
namespace, this process is PID 1.

### 5. The parent completes outside-only setup

`container_write_idmaps()` maps UID/GID 0 inside to the caller’s ordinary IDs
outside. Thus the process is root only within the scope of its user namespace.

`container_cgroup_enter()` writes the child PID to `cgroup.procs`. Future
descendants inherit membership and resource limits.

In `--net` mode, provided `container_net_host_setup()` creates the bridge and
veth connection and moves one endpoint into the child’s network namespace.

### 6. The parent releases container init

Writing one byte wakes the child. If critical parent setup failed, the parent
does not write; it closes the pipe, the child sees EOF, and no command runs.

### 7. The child builds its isolated environment

`container_setup()` orders operations according to privilege needs:

1. set the private hostname;
2. bring up loopback and optionally configure `ceth0`;
3. make mounts private;
4. bind and remount the rootfs read-only;
5. add writable `/tmp`, minimal `/dev`, and namespace-aware `/proc`;
6. pivot into the rootfs and detach the host root;
7. remove all capabilities and set `no_new_privs`;
8. install seccomp last.

At this point the process sees a container-specific environment and has fewer
powers than namespace root initially possessed.

### 8. Init launches and supervises the command

`container_init()` forks. The new child calls `execvp()` to become the requested
program. The original process stays PID 1 and calls `waitpid(-1)` repeatedly.
That matters because orphan descendants are reparented to PID 1; without reaping,
they would remain as zombies.

When the main command exits, init converts its wait status and returns the same
meaningful exit code.

### 9. The parent tears down host-side state

The parent waits for container init, frees the custom stack, removes optional
network state, and removes the now-empty cgroup. Namespace and private mount
state disappear automatically when the final container process exits.

## Why the order matters

| Ordering rule | Failure prevented |
|---|---|
| Child waits until UID maps exist | A new user namespace begins without usable IDs. |
| Child enters cgroup before release | The command never runs briefly without limits. |
| Host veth is moved before release | The child sees the interface when it configures networking. |
| Mount propagation becomes private first | Container mount changes cannot leak to the host. |
| `/proc` is mounted before pivot/detach | User-namespace proc mount avoids `EPERM` and reflects the PID namespace. |
| Network and filesystem setup precede capability removal | Those operations require capabilities. |
| Seccomp is installed last | It denies the mount and pivot calls setup itself needs. |
| Parent waits before cgroup removal | A cgroup containing processes cannot be removed. |

## Security model

No single mechanism is sufficient:

- **Namespaces** reduce visibility but do not impose memory/process limits.
- **User mapping** makes container root unprivileged on the host.
- **Read-only root and minimal `/dev`** reduce filesystem/device exposure.
- **Cgroups** limit resource consumption but do not hide resources.
- **Capabilities** remove individual root powers.
- **Seccomp** denies especially dangerous kernel entry points.
- **`no_new_privs`** prevents later executable files from restoring privilege.

The project demonstrates defense in depth: several independent kernel features
combine to form the container boundary.

## Important data flow

`struct container *c` is the central configuration object. Fields flow through
the program as follows:

| Source field/state | Main consumers |
|---|---|
| `argv` | `container_init()` → `execvp()` |
| `hostname` | `container_setup()` → `sethostname()` |
| `rootfs` | `container_setup()` → `setup_filesystem()` |
| `pids_max`, `mem_max` | `container_cgroup_init()` → cgroup control files |
| `cgroup_base`, `name` | `container_cgroup_init()` → `cg_path` |
| `cg_path` | cgroup limit helper, enter, and cleanup |
| network config fields | parent host setup and child `container_net_config()` |
| `sync[0]`, `sync[1]` | parent release protocol and child wait |

## Exit-status flow

```text
requested command exits or receives a signal
        ↓ waitpid() in container_init
status_to_exit_code()
        ↓ container_init returns that code
clone child exits with that code
        ↓ waitpid() in container_run
status_to_exit_code()
        ↓ container_run returns it
provided main() returns it to the shell
```

This is why a normal command exit and a signal death are both preserved through
two process boundaries.

## How the implementation maps to the grading areas

- Namespace tests exercise flag selection, ID maps, hostname, PID visibility,
  mount isolation, and networking.
- Filesystem tests exercise pivoting, read-only root, writable `/tmp`, minimal
  devices, private mount propagation, and fresh `/proc`.
- Process tests exercise PID 1 behavior, orphan adoption, zombie reaping, and
  exit-status propagation.
- Cgroup tests inspect configured files and verify fork-bomb/memory enforcement.
- Security tests inspect empty capability sets and invoke a denied syscall.
- Lifecycle tests cover stale cgroups, simultaneous containers, cleanup, and
  AddressSanitizer memory safety.

## Short interview explanation

“The runtime creates a child with five new namespaces and blocks it on a pipe.
The parent maps container root to the caller, places the child in a limited
cgroup, and optionally creates a veth. After release, the child sets its
hostname and network, builds and pivots into a read-only rootfs with private
`/tmp`, `/dev`, and `/proc`, drops every capability, and installs a seccomp
denylist. It then stays as PID 1 while a forked child execs the requested
command, so it can reap orphans. The parent waits, tears down networking and the
cgroup, and returns the command’s exit status.”
