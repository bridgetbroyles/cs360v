# Filesystem Isolation, Capabilities, and Setup

## Why a container needs a root filesystem

The container shares the host kernel, but its programs still need executable
files and meaningful paths such as `/bin`, `/proc`, `/dev`, and `/tmp`. The
rootfs is a small directory tree, not a second kernel and not necessarily a full
Linux distribution or virtual disk. This project builds it mostly from BusyBox.

The rootfs’s storage size is not equal to the container’s RAM use. Files remain
on host storage and the kernel brings needed pages into memory on demand. The
memory cgroup limits runtime memory; it does not require loading the entire
filesystem into RAM.

A mount tells the kernel that a filesystem or existing path should appear at a
particular location. Because the child has a mount namespace, these mount-table
changes affect the container’s view without changing the host’s view.

## Why capabilities are separate

UID 0 traditionally has every root power. Linux capabilities divide those
powers into individual permissions, such as administering a network interface.
The user namespace temporarily gives container root enough namespace-scoped
capabilities to build its environment. After setup, this file removes them so
the requested command cannot repeat or undo privileged setup.

## `bind_device()`

**Assignment status:** Added private helper. The specification required the two
device bind mounts but did not prescribe this function.

**Purpose:** Make one host device, `/dev/null` or `/dev/zero`, available at the
equivalent location under the future container root.

**Used by:** `setup_filesystem()` calls it twice.

```c
static int bind_device(const char *rootfs, const char *name)
{
    char host_path[PATH_MAX];
    char target_path[PATH_MAX];
    snprintf(host_path, sizeof host_path, "/dev/%s", name);
    snprintf(target_path, sizeof target_path, "%s/dev/%s", rootfs, name);

    int fd = open(target_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "container: create %s: %s\n",
                target_path, strerror(errno));
        return -1;
    }
    close(fd);

    if (mount(host_path, target_path, NULL, MS_BIND, NULL) < 0) {
        fprintf(stderr, "container: bind %s: %s\n",
                host_path, strerror(errno));
        return -1;
    }
    return 0;
}
```

### Variables and lines

- `rootfs` is the host path to the directory that will become container `/`.
- `name` is either `null` or `zero`.
- `host_path` becomes `/dev/null` or `/dev/zero`.
- `target_path` becomes `<rootfs>/dev/null` or `<rootfs>/dev/zero`.
- `open()` creates an ordinary empty target file. `O_CREAT` creates it,
  `O_WRONLY` opens it for writing, and `O_CLOEXEC` ensures the descriptor would
  close during `exec` if still open. Mode `0666` requests read/write permission,
  subject to the process umask.
- `fd` is the returned file descriptor. A negative value reports failure.
- `close(fd)` releases the descriptor; the file itself remains as a mount point.
- `mount(..., MS_BIND, ...)` overlays the existing host device at that target.
  The last and third arguments are unused for a bind mount, so they are `NULL`.
- The function returns `0` only after the bind succeeds.

- /dev/null and /dev/zero are special Linux devices that programs expect to exist. Your container needs them because it has its own filesystem, so it won't automatically see the host's /dev devices.

The target must be created after mounting tmpfs on `<rootfs>/dev`, because that
tmpfs hides any placeholder files that originally existed in the rootfs.

## `setup_filesystem()`

**Assignment status:** Added private helper. All of its operations were required
inside `container_setup()`, but the specification did not require separating
them into a function.

**Purpose:** Build the container-specific mount layout, switch `/` to the chosen
rootfs, and make the old host root unreachable.

**Used by:** `container_setup()`.

### The goal before the code

Before this function, the child is still looking at the host filesystem.
`rootfs` is a host directory containing the small filesystem image for the
container, such as `/images/miniroot`. This function makes that directory look
like `/` to the command, then removes the old host root so the command cannot
walk back to host files.

A **mount** tells Linux which filesystem should appear at a path. Before the
root switch, the code prepares three mounts below the host path `rootfs`:

| Host path before the switch | Path the command sees afterward | Why it exists |
| --- | --- | --- |
| `<rootfs>/tmp` | `/tmp` | writable throwaway working space |
| `<rootfs>/dev` | `/dev` | only the two safe device files the project needs |
| `<rootfs>/proc` | `/proc` | process information for this container's PID view |

### Complete sequence and code explanation

```c
static int setup_filesystem(const char *rootfs)
{
    char path[PATH_MAX];
```

- `rootfs` is the host path that will become `/`.
- `path` is reused for `<rootfs>/tmp`, `<rootfs>/dev`, and `<rootfs>/proc`.

```c
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        fprintf(stderr, "container: make / private: %s\n", strerror(errno));
        return -1;
    }
```

Linux can mark a mount as **shared**, which means a new mount below it can be
copied to a peer mount in another mount namespace. That would be dangerous here:
mounting a temporary filesystem at the container's `/tmp` could also alter the
host's mount layout. `MS_PRIVATE` turns off that forwarding. `MS_REC` applies
the setting to `/` and every mount below it, rather than protecting only `/`.

So the namespace gives the child a separate mount table, and this call makes
sure later changes to that table stay in the child.

```c
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        /* report and return */
    }
    if (mount(NULL, rootfs, NULL,
              MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) < 0) {
        /* report and return */
    }
```

- `pivot_root()` requires the new root to be a mount point. Bind-mounting the
  directory onto itself turns it into one; it does not copy the files.
- `MS_REC` includes nested mounts.
- The second call remounts that bind read-only. `MS_REMOUNT` changes an existing
  mount, and `MS_RDONLY` removes ordinary write access to the image. This keeps
  a command from changing supplied programs or configuration in the rootfs,
  while the next step still gives it a safe place to write temporary files.

```c
    snprintf(path, sizeof path, "%s/tmp", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        /* report and return */
    }
```

`/tmp` is the conventional location for short-lived working files. For example,
an editor may write a temporary save file there; a program may create a lock,
socket, or intermediate output there. Those files should not change the
container image or be kept for the next run.

A **tmpfs** is a fresh filesystem whose contents live in memory (and may use
swap), rather than in the rootfs image. This call mounts an empty tmpfs at
`<rootfs>/tmp`. Before `pivot_root`, that is the host-visible preparation path;
afterward, the same mount is what the command calls `/tmp`. It is writable even
though `/` is read-only, and it disappears when the container ends. The first
and third `"tmpfs"` identify the source label and filesystem type.

```c
    snprintf(path, sizeof path, "%s/dev", rootfs);
    if (mount("tmpfs", path, "tmpfs", 0, NULL) < 0) {
        /* report and return */
    }
    if (bind_device(rootfs, "null") < 0 ||
        bind_device(rootfs, "zero") < 0)
        return -1;
```

- `/dev` contains special files that connect programs to kernel or hardware
  services. A fresh `/dev` avoids exposing host disks, terminals, and other
  sensitive devices.
- User-namespace root cannot use `mknod()` to construct these device nodes, so
  the implementation exposes only the safe, needed host nodes through binds.
- `/dev/null` accepts and discards bytes; `/dev/zero` supplies zero bytes. They
  are common basic devices and are the only ones this project intentionally
  makes visible.
- If either device bind fails, setup stops rather than continuing with an
  incomplete `/dev`.

```c
    snprintf(path, sizeof path, "%s/proc", rootfs);
    if (mount("proc", path, "proc", 0, NULL) < 0) {
        /* report and return */
    }
```

- `/proc` is a kernel-provided virtual filesystem; commands such as `ps` read it
  to learn which processes exist. A fresh proc filesystem reflects the current
  PID namespace. Without it, `/proc` could still describe host processes even
  though PID lookup is isolated.
- This must happen before detaching the old root; under these user-namespace
  rules, mounting proc after losing the old proc view can fail with `EPERM`.

```c
    if (chdir(rootfs) < 0) { /* report and return */ }
    if (syscall(SYS_pivot_root, ".", ".") < 0) {
        /* report and return */
    }
    if (umount2(".", MNT_DETACH) < 0) { /* report and return */ }
    if (chdir("/") < 0) { /* report and return */ }
    return 0;
}
```

- `chdir(rootfs)` makes the desired root the current directory. The code uses
  `.` below because it now names that exact prepared rootfs mount.
- glibc does not provide a normal `pivot_root()` wrapper, so `syscall()` invokes
  kernel operation `SYS_pivot_root` directly.
- Using `"."` for both new root and old-root attachment temporarily stacks the
  old host root at the current location after the pivot.
- `umount2(".", MNT_DETACH)` lazily detaches that old root, removing the route
  by which the container could otherwise reach host files.
- `chdir("/")` moves to the new root directory so later relative paths begin
  inside the container rather than at the detached mount point.

The order is essential: privilege is still available, proc is mounted before
the pivot, and the old root is detached only after the switch succeeds.

## `drop_capabilities()`

**Assignment status:** Added private helper. Capability removal was required by
Part III of the specification, but this helper name and split were optional.

**Purpose:** Remove all pieces of root authority and prevent later programs from
regaining privileges.

**Used by:** `container_setup()` after networking and mounts, before seccomp.

```c
static int drop_capabilities(void)
{
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
            fprintf(stderr, ...);
            return -1;
        }
    }
```

- A Linux capability is one specific root power, such as network administration.
- `cap` visits every capability number known by the build headers.
- `PR_CAPBSET_DROP` removes the capability from the bounding set, the ceiling on
  capabilities this process and later executed programs may obtain.
- An older running kernel may not recognize a newer capability number. It
  reports `EINVAL`, which is safe to ignore because that capability does not
  exist there. Other errors abort.

```c
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct data[2];
    memset(&header, 0, sizeof header);
    memset(data, 0, sizeof data);
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
```

- `header` describes the capability API version and target process.
- Version 3 uses two data structures: one covers capability bits 0–31 and the
  other 32–63.
- Zeroing `data` means every effective, permitted, and inheritable bit is clear.
- `pid = 0` means the calling process itself.

```c
    if (syscall(SYS_capset, &header, data) < 0) {
        /* report and return */
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        /* report and return */
    }
    return 0;
}
```

- `SYS_capset` applies the zeroed capability sets. It is invoked through
  `syscall()` because the project uses kernel structures directly.
- `PR_SET_NO_NEW_PRIVS` guarantees later `exec` cannot grant privileges through
  setuid bits or file capabilities. This setting cannot be undone and is also a
  prerequisite for installing seccomp without `CAP_SYS_ADMIN`.

## `container_setup()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Coordinate all setup performed from inside container init before
the command is started.

**Used by:** `container_init()` after the parent releases it.

```c
int container_setup(struct container *c)
{
    if (sethostname(c->hostname, strlen(c->hostname)) < 0) {
        fprintf(stderr, "container: sethostname: %s\n", strerror(errno));
        return -1;
    }

    (void)container_network();

    if (c->net_enabled)
        (void)container_net_config(c);

    if (setup_filesystem(c->rootfs) < 0)
        return -1;

    if (drop_capabilities() < 0)
        return -1;

    if (container_seccomp() < 0)
        return -1;

    return 0;
}
```

### Line/block explanation

- `sethostname()` changes the hostname visible in this UTS namespace. The
  second argument is the character count, not including a terminating NUL.
- `(void)container_network()` deliberately discards the result. Loopback is
  best-effort according to the project behavior: diagnostics may be printed,
  but failure does not stop the command.
- Optional veth configuration is also treated as best-effort. The host-side
  network helper itself is provided code and may fail on hosts without support.
- `setup_filesystem()` is mandatory. Without a successful pivot, running the
  command could expose the host filesystem or fail to find the rootfs command.
- `drop_capabilities()` runs only after hostname, networking, and mounts because
  those operations require capabilities.
- `container_seccomp()` is last because it blocks mount and `pivot_root`, which
  the filesystem setup has just used.
- Any mandatory failure returns `-1`; otherwise the command may safely start.

This function is primarily an **ordering function**. Most complexity lives in
the helpers, but its sequence is part of the security design.
