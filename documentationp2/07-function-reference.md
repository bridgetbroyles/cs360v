# 07 — Function-by-Function Code Guide

## Table of contents

- [Private helpers](#private-helpers)
- [`container_namespaces`](#container_namespaces)
- [`container_write_idmaps`](#container_write_idmaps)
- [`container_cgroup_init`](#container_cgroup_init)
- [`container_cgroup_enter`](#container_cgroup_enter)
- [`container_network`](#container_network)
- [`container_net_config`](#container_net_config)
- [`container_setup`](#container_setup)
- [`container_seccomp`](#container_seccomp)
- [`container_init`](#container_init)
- [`container_run`](#container_run)
- [`container_cleanup`](#container_cleanup)

This chapter is designed for quick code navigation. The comments immediately
above each function in `container.c` give its job, caller, and critical rule;
this chapter explains the important statements inside it.

## Private helpers

### `report_errno`

Called after a system call fails. It saves `errno`, prints the required prefix,
restores `errno`, and returns `-1`:

```c
int saved_errno = errno;
fprintf(stderr, "container: %s: %s\n", operation, strerror(saved_errno));
errno = saved_errno;
return -1;
```

Saving matters because functions used while formatting an error may change
`errno`.

### `format_path`

Called wherever a rootfs, procfs, or cgroup path is assembled:

```c
int written = snprintf(out, out_size, "%s/%s", directory, name);
if (written < 0 || (size_t)written >= out_size)
    /* fail with ENAMETOOLONG */;
```

`snprintf` returns how many characters it wanted to write. A return greater than
or equal to the destination size means truncation occurred.

### `create_bind_target`

Called twice by `container_setup`. `O_CREAT` creates the hidden `/dev` target,
`O_CLOEXEC` prevents this helper descriptor from leaking through `exec`, and
closing it before `mount` means only the pathname is retained.

### `set_interface_address`

Called by `container_net_config` for the address and netmask. It fills an
`ifreq`, parses text with `inet_pton`, copies the resulting `sockaddr_in` into
`ifr_addr`, and performs the requested ioctl.

### `drop_capabilities`

Called only by `container_setup`, after pivoting. It drops every bounding-set
bit, submits two zeroed version-3 capability words, and sets no-new-privileges.

### `status_code`

Called in both process layers. `WIFEXITED` and `WIFSIGNALED` must be checked
before using their matching extraction macros.

### `child_entry`

Called by the `clone` library machinery. It exists because `clone` requires a
generic `void *` argument callback, while the project API uses
`struct container *`.

### `wait_for_pid`

Called by the runtime parent. `waitpid` may return `-1/EINTR` when a signal
temporarily interrupts it; retrying is not a second wait—it completes the same
logical operation.

## `container_namespaces`

**Called by:** `container_run`, as part of `clone` flags.

```c
return CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS |
       CLONE_NEWUTS | CLONE_NEWNET;
```

It performs no system call itself. It centralizes the policy of which resources
are isolated.

## `container_write_idmaps`

**Called by:** parent-side `container_run`, while init is blocked.

The function first constructs `/proc/<host child PID>`. The PID here is the
parent's host-visible PID, because the parent must address the child's procfs
mapping files from outside the new namespace.

```c
snprintf(mapping, sizeof mapping, "0 %u 1", (unsigned)getuid());
write_file(uid_map_path, mapping);
write_file(setgroups_path, "deny");
snprintf(mapping, sizeof mapping, "0 %u 1", (unsigned)getgid());
write_file(gid_map_path, mapping);
```

`c` is unused because every required value comes from the child PID and the
parent's current UID/GID; `(void)c` documents that intentionally.

## `container_cgroup_init`

**Called by:** `container_run`, before the pipe and clone.

The function enables controllers in the parent, stores the complete group path,
and accepts `mkdir`'s `EEXIST`:

```c
write_file(subtree_control, "+pids +memory");
mkdir(c->cg_path, 0755); /* EEXIST accepted */
```

Each negative configured limit becomes the literal text `max`; otherwise the
decimal value is written. Cgroup control files accept textual values, not a C
binary `long`.

Writing zero to `memory.swap.max` ensures the kernel enforces the memory limit by
OOM-killing rather than hiding excess use in swap.

## `container_cgroup_enter`

**Called by:** `container_run`, after ID maps and before release.

```c
snprintf(pid_text, sizeof pid_text, "%d", (int)child);
write_file(cgroup_procs_path, pid_text);
```

The PID is again host-visible because `cgroup.procs` belongs to the host cgroup
filesystem. Membership automatically propagates to later descendants.

## `container_network`

**Called by:** `container_setup`, unconditionally and best-effort.

It opens a control socket, reads existing loopback flags, ORs two flags, writes
them, and closes the socket. Reading before writing avoids accidentally clearing
unrelated flags.

Every early error closes the socket. `SOCK_CLOEXEC` is defense in depth in case
future code reaches `exec` unexpectedly while the descriptor is open.

## `container_net_config`

**Called by:** `container_setup` only if `c->net_enabled`.

The prefix is validated before a shift so values outside `0..32` cannot cause
undefined C behavior. The address and mask are installed using the helper. The
interface's current flags are then preserved while adding `UP` and `RUNNING`.

For the route:

```c
dst       = 0.0.0.0;
genmask   = 0.0.0.0;
gateway   = c->net_gw;
rt_flags  = RTF_UP | RTF_GATEWAY;
rt_dev    = c->net_ifname;
```

All paths converge on `out:` so the socket is closed once regardless of which
operation failed.

## `container_setup`

**Called by:** container PID 1 after its release byte arrives.

This is an orchestration function. Each statement depends on prior state:

```c
sethostname(...);                    /* UTS namespace */
container_network();                 /* still has CAP_NET_ADMIN */
container_net_config(c);             /* only --net */
mount(... MS_PRIVATE ...);           /* contain mount propagation */
mount(rootfs, rootfs, ... MS_BIND);   /* make rootfs a mount */
mount(... MS_REMOUNT | MS_RDONLY);    /* protect image */
mount tmpfs on tmp and dev;
bind /dev/null and /dev/zero;
mount proc before losing old root;
chdir(rootfs);
pivot_root(".", ".");
umount2(".", MNT_DETACH);
chdir("/");
drop_capabilities();
container_seccomp();                 /* always last */
```

An error returns `-1` to `container_init`, which returns failure to the runtime
parent. Namespace-local mounts disappear when that child exits.

## `container_seccomp`

**Called by:** `container_setup` after capability removal.

The BPF array first loads `seccomp_data.arch`. A matching architecture jumps
over the kill instruction. Then it loads `seccomp_data.nr`, checks each deny
rule, and ends with `SECCOMP_RET_ALLOW`.

```c
struct sock_fprog program = {
    .len = (unsigned short)(sizeof filter / sizeof filter[0]),
    .filter = filter,
};
```

`len` counts BPF instructions, not bytes. The program lives on the stack only
during installation; the kernel copies it.

## `container_init`

**Called by:** `child_entry`, itself invoked by `clone`.

It has three stages:

1. Synchronize: close write end, read exactly one release byte, close read end.
2. Isolate and launch: call `container_setup`, then `fork` and `execvp`.
3. Reap: wait for all child exits and return when the requested command exits.

It deliberately does not close arbitrary inherited descriptors before `exec`.
The tests pass descriptor 3 to the observer and use it to hold the container
open while inspecting host cgroup state.

## `container_run`

**Called by:** provided `main.c`; this is the only public entry into our runtime.

Important tracked state:

```c
void *stack = NULL;
pid_t child = -1;
int pipe_created = 0;
int network_attempted = 0;
```

Each variable answers whether cleanup owns a resource. After a successful close,
the corresponding descriptor becomes `-1`. After a successful wait, `child`
becomes `-1`, preventing a second wait or kill.

Host network setup is intentionally best-effort because provided `net.c`
documents that contract. The child-side configuration will still fail clearly
if `--net` was requested but no veth arrived.

The clone stack cannot be freed immediately after `clone`; the child may still
be using it. It is freed only after the child is reaped or killed and reaped.

## `container_cleanup`

**Called by:** the common `container_run` cleanup block.

An empty path means initialization never established a target. Otherwise the
function calls `rmdir`. It accepts `ENOENT`, but errors such as `EBUSY` or
`ENOTEMPTY` are reported because they indicate leaked cgroup membership/state.

