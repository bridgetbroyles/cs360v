# Namespaces and Identity

## `container_namespaces()`

**Assignment status:** One of the 11 functions explicitly required by the
specification.

**Purpose:** Return the flags that tell `clone()` which resources the child must
see through isolated namespaces.

**Used by:** `container_run()` passes the result to `clone()` and separately ORs
in `SIGCHLD`.

```c
int container_namespaces(void)
{
    return CLONE_NEWUSER
         | CLONE_NEWPID
         | CLONE_NEWNS
         | CLONE_NEWUTS
         | CLONE_NEWNET;
}
```

### Line-by-line explanation

- `int container_namespaces(void)` takes no arguments and returns one integer.
  The integer is a bitmask: individual bits represent independent options.
- `return CLONE_NEWUSER` starts the expression with a new user namespace. This
  gives the child its own UID/GID interpretation.
- `| CLONE_NEWPID` adds a PID namespace. The first child created inside becomes
  PID 1 there, and container processes cannot see ordinary host processes.
- `| CLONE_NEWNS` adds a mount namespace. Despite the less descriptive name,
  `NEWNS` means a separate mount table.
- `| CLONE_NEWUTS` adds a UTS namespace, primarily isolating the hostname.
- `| CLONE_NEWNET` adds a network namespace with its own interfaces and routes.
- `;` ends the single return expression.

The bitwise OR operator `|` does not choose one flag. It combines the distinct
set bits from all five constants into one value. `clone()` inspects those bits.

`SIGCHLD` is not a namespace. `container_run()` adds it to the same clone flags
so the parent receives normal child-exit behavior and can use `waitpid()`.

## `container_write_idmaps()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Give the child a usable identity inside its new user namespace.
Container UID/GID 0 maps to the ordinary UID/GID of the user running the
runtime on the host.

**Used by:** The parent calls it in `container_run()` after `clone()` and before
releasing the child through the pipe.

```c
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;

    char path[64];
    char mapping[64];

    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
    if (write_file(path, mapping) < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
    if (write_file(path, "deny") < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
    if (write_file(path, mapping) < 0)
        return -1;

    return 0;
}
```

### Parameters and local variables

| Name | Type | Meaning |
|---|---|---|
| `c` | `struct container *` | Container configuration. This function does not need any field, but the required interface includes it. |
| `child` | `pid_t` | Child’s PID as seen by the host parent; used to locate `/proc/<child>`. |
| `path` | `char[64]` | Reusable buffer holding one procfs filename. |
| `mapping` | `char[64]` | Reusable buffer holding one textual ID-map rule. |

`pid_t` is the system type for process IDs. It is cast to `int` because `%d`
expects an `int` in these `snprintf()` calls.

### Line-by-line explanation

- `(void)c;` explicitly marks the parameter unused and prevents a compiler
  warning. The function signature is fixed by `container.h`.
- The two `char` arrays reserve stack memory for formatted strings.
- The first `snprintf()` produces a path such as `/proc/4312/uid_map`.
- `getuid()` returns the real UID of the runtime’s host user.
- `"0 %d 1"` is an ID-map triple: start at ID 0 inside, map it to this host
  UID, and map one consecutive ID.
- The provided `write_file()` writes that exact text to the kernel control file.
  A negative return aborts immediately.
- The next path is `/proc/<child>/setgroups`. Writing `deny` is a kernel safety
  requirement before an unprivileged process may write `gid_map`.
- The final pair of `snprintf()` calls creates `/proc/<child>/gid_map` and a
  corresponding mapping using `getgid()`.
- `return 0` means all three writes succeeded.

### Why the parent performs these writes

The new user namespace initially has no ID mappings, so the child cannot perform
the privileged-looking setup allowed to namespace root. The parent can identify
the child using its host PID and populate `/proc/<child>/...`. The sync pipe
keeps the child blocked until this is finished.

This does **not** make the process host root. UID 0 inside maps to the caller’s
ordinary host UID. The child can perform namespace-scoped administration while
remaining unprivileged with respect to the host.
