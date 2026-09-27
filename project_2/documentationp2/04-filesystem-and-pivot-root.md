# 04 — Building the Container Filesystem

## Table of contents

- [Goal and starting point](#goal-and-starting-point)
- [Private propagation](#private-propagation)
- [Read-only root image](#read-only-root-image)
- [Writable tmp](#writable-tmp)
- [Minimal dev](#minimal-dev)
- [Private proc](#private-proc)
- [Pivoting into the new root](#pivoting-into-the-new-root)

## Goal and starting point

`c->rootfs` names a host directory created by `make-rootfs.sh`. It contains a
static BusyBox and mount points such as `proc`, `dev`, and `tmp`. A directory is
not automatically a process root; `container_setup()` constructs mounts and
then uses `pivot_root`.

Final view:

```text
/
├── bin/        BusyBox and command links; read-only
├── etc/        image content; read-only
├── proc/       new procfs for the container PID namespace
├── dev/        tmpfs containing bound /dev/null and /dev/zero
└── tmp/        writable tmpfs
```

## Private propagation

```c
mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
```

- `NULL` source/type means change properties of an existing mount tree.
- `/` selects the current root mount tree.
- `MS_REC` applies recursively.
- `MS_PRIVATE` prevents changes from propagating out of this mount namespace.

Without this step, container mounts could appear in the host's mount table.

## Read-only root image

`pivot_root` requires the new root to be a mount point, not merely a directory:

```c
mount(c->rootfs, c->rootfs, NULL, MS_BIND | MS_REC, NULL);
```

A bind mount exposes the same directory tree as a distinct mount. It is then
remounted read-only:

```c
mount(NULL, c->rootfs, NULL,
      MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);
```

- `MS_REMOUNT` changes mount flags.
- `MS_BIND` identifies this as a bind remount.
- `MS_RDONLY` makes the image immutable.

## Writable tmp

```c
mount("tmpfs", rootfs_tmp, "tmpfs", 0, "mode=1777");
```

This mounts new memory-backed storage over the image's `tmp` directory. Mode
`1777` means every user may create files, while the sticky bit prevents users
from deleting each other's files. It remains writable even though the
underlying root image is read-only because it is a separate mount.

## Minimal dev

Exposing the host's entire `/dev` would expose many devices. Instead:

```c
mount("tmpfs", rootfs_dev, "tmpfs", 0, "mode=755");
create_bind_target(rootfs_dev_null);
create_bind_target(rootfs_dev_zero);
mount("/dev/null", rootfs_dev_null, NULL, MS_BIND, NULL);
mount("/dev/zero", rootfs_dev_zero, NULL, MS_BIND, NULL);
```

The new `/dev` tmpfs hides placeholders that existed in the image, so the code
creates fresh empty target files. A bind mount then covers each regular target
with the real character device. `mknod` is intentionally avoided because it is
not permitted in this user-namespace arrangement.

## Private proc

```c
mount("proc", rootfs_proc, "proc", 0, NULL);
```

Procfs reflects the PID namespace of the process mounting it. Mounting a new
instance gives the command a `/proc` containing only its container processes.
It must happen before the old root is detached because this kernel permission
path requires an existing procfs to remain visible during the mount.

## Pivoting into the new root

```c
chdir(c->rootfs);
syscall(SYS_pivot_root, ".", ".");
umount2(".", MNT_DETACH);
chdir("/");
```

Line by line:

1. `chdir(c->rootfs)` makes the future root the working directory.
2. `pivot_root(".", ".")` swaps the process's root to that mount. The direct
   syscall is needed because glibc has no wrapper.
3. `umount2(".", MNT_DETACH)` lazily detaches the old root. This removes host
   paths from the container view without requiring every old reference to close
   synchronously.
4. `chdir("/")` places the process in the new root rather than retaining a
   working-directory reference to the old tree.

After this point, `/bin/busybox` refers to the file inside the rootfs image, and
host paths such as `/usr` are absent.

