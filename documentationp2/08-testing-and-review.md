# 08 — Testing, Debugging, and Interview Review

## Table of contents

- [How to run the project](#how-to-run-the-project)
- [What every official test proves](#what-every-official-test-proves)
- [Failure-to-function lookup](#failure-to-function-lookup)
- [Design tradeoffs](#design-tradeoffs)
- [Interview questions and answers](#interview-questions-and-answers)
- [Verified result](#verified-result)

## How to run the project

Project 2 must run inside the Project 0 Ubuntu VM because macOS does not expose
Linux namespaces, cgroup v2, or seccomp.

```bash
cd project_2
make -C runtime
./make-rootfs.sh ./rootfs
sudo ./tests/run_tests.sh
```

For an interactive inspection:

```bash
sudo ./runtime/container --hostname box --rootfs ./rootfs \
  --pids-max 64 --mem-max 128M -- /bin/sh
```

Inside, useful checks include:

```bash
echo $$                       # command should normally be PID 2
hostname                      # box
ps                            # only container processes
touch /should-fail            # read-only filesystem
touch /tmp/works              # writable tmpfs
grep Cap /proc/self/status    # CapBnd and CapEff should be zero
```

## What every official test proves

| Test | What it verifies |
|---|---|
| compile | Only provided libraries/headers are needed. |
| exec | Spawn, setup, pivot, and `execvp` work together. |
| exit_status | Normal command status survives command → init → runtime. |
| signal_exit | Signal death becomes `128 + signal`. |
| uts_hostname | UTS namespace and `sethostname` work. |
| pid_small | Fresh PID namespace exists. |
| user_ns | A one-ID UID map was written. |
| parented | Command is child of container PID 1. |
| reaping | PID 1 reaps an orphan, leaving no zombie. |
| root_replaced | Host tree is hidden and BusyBox image is visible. |
| own_proc | Procfs reflects only container PIDs. |
| root_readonly | Root bind remount includes `MS_RDONLY`. |
| tmp_writable | Separate tmpfs overlays `/tmp`. |
| dev_present | `/dev/null` and `/dev/zero` are bound character devices. |
| no_mount_leak | Root propagation was made recursive-private. |
| own_netns | Network namespace does not expose host interfaces. |
| loopback_up | `lo` flags were read and updated. |
| net_reachable | Veth address, mask, up flag, and default route all work. |
| in_cgroup | Init/command see the named cgroup path. |
| pids_max | Correct textual PID limit was written. |
| memory_max | Correct textual byte limit was written. |
| member | A live process appears in `cgroup.procs`. |
| pids_enforced | Fork eventually returns `EAGAIN`. |
| mem_enforced | Memory hog is OOM-killed with status 137. |
| capbnd_empty | Bounding capability set is zero. |
| capeff_empty | Effective capability set is zero. |
| syscall_filtered | `ptrace(PTRACE_TRACEME)` returns `EPERM`. |
| stale_cgroup | `mkdir(...)=EEXIST` is accepted and limits are refreshed. |
| concurrent | State is per-container rather than global/fixed. |
| sanitizer_clean | No detected memory or undefined-behavior bug. |
| cgroup_removed | Cgroup existed during execution and disappeared afterward. |
| exit_status_ok | Cleanup did not overwrite command status. |

## Failure-to-function lookup

| Symptom | Inspect first |
|---|---|
| Nothing executes | `container_run`, sync pipe, `container_init`, pivot, `execvp`. |
| Child setup says permission denied | ID maps and release ordering. |
| Command is PID 1 | Missing init `fork`; command was executed too early. |
| Zombies remain | `waitpid(-1)` loop in `container_init`. |
| Host `/usr` visible | `pivot_root` or old-root detach. |
| Host gains new mounts | `MS_REC | MS_PRIVATE`. |
| `/proc` shows host PIDs | Procfs mount timing or PID namespace flag. |
| Root is writable | Bind remount flags. |
| `/dev/null` is regular | Target created but bind mount failed/missing. |
| Only network test fails | Prefix mask, interface flags, or default route. |
| Fork bomb reaches 400 | Cgroup entry or `pids.max`. |
| Memory hog returns normally | `memory.max`, swap max, or cgroup membership. |
| CapBnd nonzero | Bounding-set loop. |
| ptrace allowed | Seccomp filter/install ordering. |
| Cgroup remains | Child descendants still alive or `rmdir` failure. |
| Sanitizer reports leak | Clone stack, socket, pipe, or failure cleanup. |

## Design tradeoffs

### `clone` versus `unshare`

`clone` creates the child directly in all namespaces and gives a clear parent
PID for maps/cgroup setup. `unshare` could transform an existing process, but a
new PID namespace affects only subsequently created children, making lifecycle
control less direct.

### Parent/child pipe versus timing

A pipe is a kernel synchronization primitive with exact ordering. Sleeping or
assuming the parent runs first creates a race that can fail under load.

### PID-1 wrapper versus direct exec

Directly executing the command is simpler, but then it becomes PID 1, receives
special signal behavior, and may fail to reap orphans. The wrapper costs one
small process and gives correct init semantics.

### Denylist seccomp versus allowlist

The assignment uses a denylist so normal BusyBox/library syscalls continue to
work across architectures. A production sandbox often prefers an allowlist,
which is stricter but much harder to maintain and more likely to break legitimate
workloads.

### `pivot_root` versus `chroot`

`pivot_root` changes the mount-tree root and lets the old root be detached.
`chroot` only changes pathname resolution and is weaker when mount references or
privileges remain.

### Best-effort host networking

Provided host network creation does not abort the core container, preserving
the ability to run isolated workloads when bridge setup is unavailable. In
`--net` mode, child veth configuration still returns a clear failure rather than
pretending connectivity exists.

## Interview questions and answers

**Why does the child wait immediately after `clone`?**

Its user namespace initially has empty UID/GID maps, and it must enter the
cgroup and optionally receive a veth before it can safely run. The pipe creates
a strict parent-before-child order.

**Why is the clone stack passed at its end?**

The supported stacks grow downward, so the initial stack pointer is the high
address `stack + CONTAINER_STACK_SIZE`.

**Why mount procfs before pivoting?**

In this user-namespace setup, the kernel permits the new proc mount while an
existing procfs is visible. Detaching the old root first can make the mount fail
with `EPERM`.

**Why can root inside be unprivileged outside?**

User namespaces translate IDs. Inside UID 0 maps to exactly one outside UID,
and namespace capabilities do not grant unrestricted host capabilities.

**Why are capabilities insufficient without seccomp?**

Some dangerous or unwanted syscalls do not require a capability in every mode.
The test's unprivileged `ptrace(PTRACE_TRACEME)` demonstrates this.

**Why does moving init into the cgroup cover the command?**

Cgroup membership is inherited across fork. Init enters before release, so the
command and every later descendant start within the same limits.

**What is destroyed automatically?**

When the last process exits, namespace-local mount and network state disappear.
The host-side cgroup directory and optional veth cleanup remain the runtime's
responsibility.

**How is a setup failure prevented from hanging?**

The common cleanup block closes pipe ends, sends SIGKILL to any cloned init,
waits for it, tears down attempted host networking, removes the cgroup, and frees
the stack.

## Verified result

The exact official suite was executed in the provided Ubuntu 24.04 ARM64 VM:

```text
32 passed, 0 failed (of 32)
```

This included compile warnings, functional namespace/filesystem/network tests,
real PID and memory enforcement, stale/concurrent scenarios, and ASan/UBSan.

