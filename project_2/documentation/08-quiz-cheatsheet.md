# Project 2 Quiz Cheat Sheet

## One-sentence project summary

This program uses ordinary Linux processes plus kernel isolation features to run
a command with isolated identities, processes, files, hostname, networking,
resource limits, and allowed operations—without creating a virtual machine.

## Architecture at a glance

Read each numbered box with the matching definition directly below it.

```text
[1] HOST PARENT: main() -> container_run()
      |
      +-- [2] isolation: namespaces + UID/GID map + cgroup + sync pipe
      |
      `-- clone() --> [3] CONTAINER INIT: PID 1 inside
                          |
                          +-- [4] setup: hostname + network + rootfs
                          +-- [5] restrict: capabilities + seccomp
                          |
                          `-- fork() --> [6] COMMAND CHILD: PID 2 inside
                                            execvp() replaces it with command

[7] optional network: command -> ceth0 -> veth pair -> host bridge cvbr0
```

1. **Host parent:** the runtime process. `main()` reads options and
   `container_run()` coordinates creation, waiting, and cleanup.
2. **Isolation/control:** namespaces change what the child sees; UID/GID maps
   connect its container identity to an ordinary host identity; the cgroup
   limits resource use; the pipe stops the child until parent setup finishes.
3. **Container init:** the child made by `clone()`. It appears as PID 1 in its
   new PID namespace and must launch and reap processes.
4. **Setup:** give init its private hostname and network view, then replace its
   visible `/` with the supplied rootfs.
5. **Restrictions:** capabilities are individual root-like powers; seccomp is a
   syscall filter. Both are applied after setup so the command inherits them.
6. **Command child:** init must stay PID 1 to reap orphaned processes, so it
   forks a child. `execvp()` replaces that child with the requested program.
7. **Optional network:** `ceth0` is the container interface; a veth pair is a
   virtual cable; `cvbr0` is the host-side virtual switch/gateway.

Unlike Project 1, there is no emulated CPU, guest kernel, Virtio queue, or vhost
backend. Every process uses the host Linux kernel.

## Exact startup story: who performs each step?

The diagram uses `[A]`, `[B]`, and `[C]`; their explanations are immediately
below it.

```text
provided main()
    |
    | parses options into struct container; calls container_run()
    v
host parent: create cgroup + synchronization pipe
    |
    | clone() creates init in five new namespaces
    v
cloned child: child_entry() calls container_init(); it is PID 1 inside
    |
    | closes write end and blocks reading sync pipe
    ^
    | parent writes UID/GID maps, puts init in cgroup,
    | and optionally creates host-side networking
    |
parent writes one byte  ------------------------------->  container_init() wakes
                                                            |
                                                            v
                                                      container_setup() [A]
                                                            |
                +-------------------------------------------+------------------+
                | set hostname; bring up lo; configure ceth0 if --net         |
                | prepare/pivot to rootfs; drop capabilities; install seccomp |
                +-------------------------------------------+------------------+
                                                            |
                                                            v
                                                container_init() calls fork() [B]
                                                /                         \
                              init parent stays PID 1             command child is PID 2
                              and calls waitpid(-1)                calls execvp(argv[0], argv)
                                                \                         /
                                                 command runs, then exits
                                                            |
                                   init reaps command [C] and returns its exit code
                                                            |
                                   host parent reaps init, removes veth/cgroup,
                                   and returns the same code to the host shell
```

**Why the pipe?** The child must not run while it has an unfinished identity
map, no resource limits, or half-created networking. One byte means “parent
setup succeeded”; closing without writing means “stop safely.”

### [A] What `container_setup()` actually does

- **Set hostname:** `container_setup()` calls `sethostname()`. Because init has
  a UTS namespace, this changes only the container's hostname.
- **Bring up loopback:** `container_network()` activates `lo`, so programs in
  the container can communicate through `localhost`.
- **Configure veth:** only with `--net`, `container_net_config()` assigns an IP,
  netmask, active flags, and a default route to `ceth0`. The host helper already
  created and moved that interface into the network namespace.
- **Enter rootfs:** `setup_filesystem()` makes the supplied directory become
  `/`, adds private `/tmp`, `/dev`, and `/proc`, and detaches the host root.
- **Drop capabilities:** remove the remaining individual root-like powers after
  setup no longer needs them.
- **Install seccomp:** attach a syscall filter inherited by the future command.

### [B] Why init forks and where `execvp()` happens

`container_init()` calls `fork()`, producing two processes that initially run
the same code. In the new child, `command_pid == 0`, so that child executes:

```c
execvp(c->argv[0], c->argv);
```

`execvp()` does **not** create another process. It replaces the command child's
program with the requested executable while keeping its PID, namespaces,
cgroup, open descriptors, capabilities, and seccomp filter. For example, if
`argv` is `{ "/bin/echo", "hello", NULL }`, PID 2 stops executing container
runtime code and becomes `/bin/echo hello`. If replacement succeeds,
`execvp()` never returns.

### [C] Who reaps which process?

There are two reapers:

- **Container init** calls `waitpid(-1, ...)` to reap the requested command and
  any orphaned descendants adopted by PID 1.
- **Host parent** calls `waitpid(child, ...)` to reap container init. It then
  tears down networking/cgroup state and returns the decoded command status.

## Core vocabulary

| Term | Meaning here | Why it matters |
| --- | --- | --- |
| Host | The real Linux machine running the container program. | It must remain protected. |
| Container | An isolated process environment, not a virtual machine. | It shares the host kernel. |
| Namespace | A kernel feature that gives processes a separate view of one kind of resource. | It provides isolation. |
| cgroup | A kernel group that limits or accounts for a set of processes. | It prevents resource exhaustion. |
| init / PID 1 | The first process in the container's PID namespace. | It launches and reaps the command. |
| rootfs | Directory image that becomes `/` for the command. | It limits visible files. |
| mount | A filesystem attached at a path. | It builds the command's filesystem view. |
| tmpfs | A temporary memory-backed filesystem. | It supplies writable `/tmp` and `/dev`. |
| capability | A small, specific root-like permission. | They are dropped after setup. |
| seccomp | Kernel syscall filter. | It blocks dangerous operations even after setup. |
| veth pair | Two linked virtual Ethernet interfaces. | One end is host-side; one goes into the container. |
| bridge | Virtual host-side switch. | It connects container veths to each other/host routing. |

## Parts of the assignment

| Part | Architecture responsibility |
| --- | --- |
| I | Create namespaces; map identities; set hostname; configure loopback and optional veth networking. |
| II | Build the private filesystem, pivot into rootfs, and fork/execute the command. |
| III | Restrict the command by dropping capabilities and installing seccomp. |
| IV | Keep init as PID 1 so it launches and reaps processes. |
| V | Create the cgroup, apply PID/memory limits, and add init to it. |
| VI | Wait for exit and remove host-side cgroup/network state. |

## Who calls every method?

| Function | Caller | Runs in | Job |
| --- | --- | --- | --- |
| `container_run()` | provided `main()` | host parent | coordinates the whole lifecycle |
| `container_cgroup_init()` | `container_run()` | host parent, before clone | creates limits directory/files |
| `write_cgroup_limit()` | `container_cgroup_init()` | host parent | formats and writes one cgroup limit |
| `container_namespaces()` | `container_run()` | host parent | supplies flags to `clone()` |
| `child_entry()` | kernel/`clone()` callback | cloned child | adapts callback to `container_init()` |
| `container_write_idmaps()` | `container_run()` | host parent, child blocked | writes UID/GID mappings |
| `container_cgroup_enter()` | `container_run()` | host parent, child blocked | moves init into its cgroup |
| `container_net_host_setup()` | `container_run()` | host parent, `--net` only | provided helper creates bridge/veth |
| `container_init()` | `child_entry()` | container PID 1 | waits, sets up, forks, and reaps |
| `container_setup()` | `container_init()` | container init | orders all child-side setup |
| `container_network()` | `container_setup()` | container init | activates private loopback |
| `container_net_config()` | `container_setup()` | container init, `--net` only | configures `ceth0` and route |
| `make_ipv4_address()` | `container_net_config()` | container init | converts address text for network ioctls |
| `setup_filesystem()` | `container_setup()` | container init | prepares mounts and switches `/` |
| `bind_device()` | `setup_filesystem()` | container init | exposes `/dev/null` or `/dev/zero` |
| `drop_capabilities()` | `container_setup()` | container init | removes root-like powers |
| `container_seccomp()` | `container_setup()` | container init | installs syscall filter |
| `status_to_exit_code()` | init and host wait paths | both | decodes `waitpid()` status |
| `container_net_host_teardown()` | `container_run()` | host parent, after wait | provided helper removes host veth |
| `container_cleanup()` | `container_run()` | host parent, after wait/error | removes empty cgroup |
| `write_file()` | ID-map and cgroup functions | host parent | provided helper writes text to kernel control files |

**Remember:** `container_namespaces()` chooses flags; `clone()` actually creates
the namespaced child. `container_setup()` runs in init, not in the host parent.

## The five namespaces

| Namespace | What the command sees | Project purpose |
| --- | --- | --- |
| User (`CLONE_NEWUSER`) | Its own UID/GID mapping. | It can be root *inside* without becoming host root. |
| PID (`CLONE_NEWPID`) | Its own process-numbering tree. | Init is PID 1; host processes are not visible. |
| Mount (`CLONE_NEWNS`) | Its own mount table. | Its `/`, `/tmp`, `/dev`, and `/proc` can differ from the host. |
| UTS (`CLONE_NEWUTS`) | Its own hostname. | Setting a container name does not rename the host. |
| Network (`CLONE_NEWNET`) | Its own interfaces, addresses, routes, and loopback. | Networking is separate from the host's network view. |

**Inside vs. host identity:** If the launcher is host UID 1000, the map
`0 1000 1` means container UID 0 maps to host UID 1000 for one ID. “Root” is
therefore meaningful inside the container but has only the launcher's ordinary
host permissions.

## Resource limits: cgroups

- `pids.max`: maximum number of processes for init and all descendants.
- `memory.max`: maximum memory the group may use.
- `memory.swap.max = 0`: do not use swap to evade the memory limit.
- Writing init's **host PID** to `cgroup.procs` makes descendants inherit the
  cgroup.

**Namespace vs. cgroup:** a namespace changes what a process can *see*; a
cgroup controls how much it can *consume*. They solve different problems.

## Filesystem essentials

- Make mounts `MS_PRIVATE` first: otherwise mounts made in the container can
  propagate to a shared host mount.
- Bind-mount `rootfs` onto itself because `pivot_root()` needs a mount point.
- Remount the rootfs read-only: a command cannot alter the supplied image.
- Mount tmpfs at `/tmp`: programs need disposable writable scratch files.
- Mount a fresh `/dev`, then bind only `/dev/null` (discard output) and
  `/dev/zero` (read zero bytes): do not expose all host devices.
- Mount fresh `/proc`: tools such as `ps` then see this PID namespace.
- `pivot_root()` changes the command's `/`; detach the old root immediately so
  host files cannot remain reachable.

## Optional `--net` traffic

```text
container namespace                                      host namespace

program -> IP stack -> ceth0 (10.44.0.2/24)
                         |
                         +====== virtual Ethernet pair ====== vh<PID>
                                                              |
                                                        bridge cvbr0
                                                        10.44.0.1/24
                                                              |
                                              host service or further routing
```

- `lo` is loopback: it makes `localhost` work *inside* the container.
- `ceth0` is the container end of the virtual Ethernet pair.
- The host-side helper creates the bridge/veth and moves `ceth0` into the
  child network namespace; our code assigns address, netmask, flags, and route.
- A default route says: “for destinations not on my local subnet, send packets
  to this gateway” (normally the host bridge).

### Quiz priority for host networking

The exact `ip` command sequence is low priority because `net.c` was provided,
not implemented by us. Know its contract instead:

- `container_net_host_setup()` creates/reuses `cvbr0`, creates the veth pair,
  attaches the host end to the bridge, and moves the other end into the child's
  network namespace as `ceth0`.
- Our `container_net_config()` configures `ceth0` from inside the container.
- `container_net_host_teardown()` removes leftover host-side veth state; the
  shared bridge remains for later containers.

## Commands worth knowing for explanation/debugging

Everything after `--` is stored in `c->argv` and becomes the command passed to
`execvp()`. The executable path is resolved **inside the new rootfs**, not in the
host filesystem.

The exact BusyBox command list is also low priority because `make-rootfs.sh` is
provided. The important point is that the requested executable must exist in
the rootfs. `/bin/sh` is useful for manual inspection, `/bin/runner` is the
provided optional network server, and the tests install `/observer` to report
what the container sees.

Useful quiz checks:

| Command inside | Expected observation | Feature demonstrated |
| --- | --- | --- |
| `echo $$` | command is normally PID 2 | init remains PID 1 |
| `id` | UID/GID 0 in the container | user namespace mapping |
| `hostname` | configured container hostname | UTS namespace |
| `ps` | only container processes | PID namespace + fresh `/proc` |
| `ls /dev` | only `null` and `zero` | minimal device view |
| `touch /foo` | read-only failure | protected rootfs |
| `touch /tmp/foo` | succeeds | writable tmpfs |
| `mount ...` | `EPERM` | capability drop/seccomp |

## Seccomp: dangerous syscalls and response

Seccomp is installed **last** because setup itself needs mount-related calls.
The BPF filter allows normal syscalls but returns `EPERM` for:

| Dangerous operation | Why deny it? |
| --- | --- |
| `ptrace` | Inspect/control other processes. |
| `mount`, `umount2`, `pivot_root`, `chroot` | Change or escape filesystem isolation. |
| `setns`, `unshare` | Join or create namespaces to alter isolation. |
| `reboot`, `swapon`, `swapoff`, `kexec_load` | Change host-wide machine state. |
| `init_module`, `finit_module`, `delete_module` | Load or remove kernel modules. |

The filter also checks CPU architecture first. Otherwise syscall numbers could
mean different operations on a different ABI.

## Why capabilities are dropped too

Namespaces and seccomp are not the whole defense. After setup, the program:

1. clears capability sets;
2. removes capabilities from the bounding set; and
3. sets `no_new_privs`, so a later setuid/file-capability program cannot regain
   privilege.

This follows the **least privilege** idea: retain power only long enough to
perform setup.

## Likely quiz questions: short answers

**What is the architecture?** A host parent orchestrates one cloned container
init. Namespaces isolate views, a cgroup limits resources, rootfs/mounts isolate
files, capabilities and seccomp restrict power, and init forks/reaps the actual
command. Optional networking uses a veth pair connected to host bridge `cvbr0`.

**Which process is PID 1?** The child created by `clone()` is host PID `<PID>`
but PID 1 in the new PID namespace. The requested command it forks is normally
PID 2 there.

**Why not have init call `execvp()` directly?** That would replace init, leaving
no dedicated PID 1 to adopt and reap orphaned descendants. Init therefore
forks; only the new command child calls `execvp()`.

**Who reaps the requested command?** Container init. **Who reaps container
init?** The host parent in `container_run()`.

**What survives `execvp()`?** The process keeps its PID, namespaces, cgroup,
open file descriptors, capability restrictions, and seccomp filter; only its
program code/data are replaced.

**Why must parent setup precede the release byte?** The child initially lacks a
usable ID mapping and cgroup membership, and optional host networking is not yet
ready. Running early would create a race and a partly isolated command.

**Why does setup order matter?** Filesystem and network setup need privileges
and syscalls that later capability/seccomp restrictions remove. Restrictions
must therefore be last.

**What is inherited by command descendants?** Namespaces, cgroup membership,
mount/root view, capability restrictions, seccomp filter, and network view.

**What is provided versus implemented?** Provided `main.c` parses arguments;
`util.c` supplies `write_file()`; `net.c` performs host bridge/veth setup with
`ip`. The required lifecycle and in-container isolation are implemented in
`container.c`.

**What happens on normal `exit 7`?** Command exits 7; init reaps and returns 7;
host parent reaps init, decodes 7, cleans up, and returns 7 to the shell. A
signal exit becomes `128 + signal number`.
