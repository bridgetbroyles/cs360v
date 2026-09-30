important, shellcontainer.c contains the original comments of container file so that you can determine what was intended to be learned and how specific things were specified for studying. 

# 00 — Container Fundamentals

## Table of contents

- [What a container is](#what-a-container-is)
- [Container versus virtual machine](#container-versus-virtual-machine)
- [The host, runtime, init, and command](#the-host-runtime-init-and-command)
- [The five kinds of isolation](#the-five-kinds-of-isolation)
- [Why ordering matters](#why-ordering-matters)
- [The security boundary](#the-security-boundary)

## What a container is

A container is not one special Linux object. It is an ordinary group of Linux
processes to which the kernel gives restricted views and restricted resources.
This project assembles those restrictions itself.

The command still executes directly on the host Linux kernel. It is isolated
because the kernel presents it with separate process IDs, hostname, mount table,
root directory, network stack, user view, resource limits, and syscall policy.

That is why the implementation uses normal Linux system calls such as `clone`,
`mount`, `pivot_root`, `ioctl`, `prctl`, and `waitpid`.

## Container versus virtual machine

| Virtual machine | Container |
|---|---|
| Emulates or virtualizes hardware. | Isolates ordinary processes. |
| Usually runs its own kernel. | Shares the host Linux kernel. |
| Guest addresses and devices are virtualized. | Processes call the host kernel directly. |
| Strong boundary but more startup/memory cost. | Lightweight, but correct kernel isolation is essential. |

Project 1 built a small virtual machine monitor. Project 2 does not emulate a
CPU. Instead, it asks the Linux kernel to create an isolated environment around
a process.

## The host, runtime, init, and command

| Role | Meaning |
|---|---|
| Host | The Ubuntu VM and its Linux kernel. |
| Runtime parent | `runtime/container`, before and after `clone()`. It owns host-side setup and cleanup. |
| Container init | The process made by `clone()`. It becomes PID 1 inside the new PID namespace. |
| Command | The requested program, forked by init. It normally becomes PID 2. |

The runtime itself is not the command. The init is not the command either.
Keeping init alive matters because PID 1 adopts and reaps orphan processes.

```text
Ubuntu host
└── runtime parent
    └── container init        container PID: 1
        └── requested command container PID: 2
```

## The five kinds of isolation

| Flag | Namespace | What becomes private |
|---|---|---|
| `CLONE_NEWUSER` | User | UID/GID mappings and namespace capabilities. |
| `CLONE_NEWPID` | PID | Process-number view; init becomes PID 1. |
| `CLONE_NEWNS` | Mount | Mount table and root-filesystem changes. |
| `CLONE_NEWUTS` | UTS | Hostname. |
| `CLONE_NEWNET` | Network | Interfaces, addresses, routes, and network view. |

```c
return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS |
       CLONE_NEWUTS | CLONE_NEWNET;
```

Bitwise OR combines independent flag bits without losing any selection.

## Why ordering matters

```text
create cgroup
    ↓
clone child into empty namespaces
    ↓
parent gives child UID/GID mappings and cgroup membership
    ↓
parent optionally moves a veth into child's network namespace
    ↓
parent releases child through a pipe
    ↓
child configures hostname, network, mounts, and new root
    ↓
child drops capabilities and installs seccomp
    ↓
child forks and executes the command
```

Examples of ordering failures:

- Without ID mappings, the child cannot perform setup.
- If it runs before cgroup entry, it can create descendants outside the limit.
- If capabilities are dropped before mounting, mounts fail.
- If seccomp blocks `mount` before setup, the filesystem cannot be built.
- If `/proc` is mounted after detaching the old root, it can fail with `EPERM`.

The synchronization pipe enforces this order instead of relying on scheduling.

## The security boundary

The command sees UID 0, but that does not mean unrestricted host root. The user
namespace maps its UID 0 to one outer UID. The runtime then adds more layers:

```text
namespace isolation → limited view
cgroup limits       → limited resource consumption
capability drop     → no privileged kernel operations
seccomp             → selected syscalls return EPERM
read-only root      → command cannot modify its image
```

No one layer replaces the others. Dropping capabilities, for example, does not
stop the unprivileged `ptrace(PTRACE_TRACEME)` test; seccomp stops it.

