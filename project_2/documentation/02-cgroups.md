# Cgroups and Resource Limits

A cgroup is a kernel-managed group of processes. Controller files inside a
cgroup directory define limits, and processes join by having their PID written
to `cgroup.procs`. Descendants inherit membership, so moving container init also
moves the future command into the resource-limited group.

## `write_cgroup_limit()`

**Assignment status:** Added private helper. The specification required writing
the limits but did not require this separate function.

**Purpose:** Convert one numeric limit to the text expected by cgroup v2 and
write it to one control file.

**Used by:** `container_cgroup_init()` calls it for `pids.max`, `memory.max`, and
`memory.swap.max`.

```c
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
{
    char path[PATH_MAX + 32];
    char value[32];

    snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);
    if (limit < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", limit);
    return write_file(path, value);
}
```

### Variables and lines

- `static` limits the function to this source file. It is not part of the public
  interface in `container.h`.
- `c` supplies the already-created `cg_path`.
- `file_name` selects a control file such as `memory.max`.
- `limit` is the numeric value to write. Its `long` type matches the fields in
  `struct container`.
- `path` has `PATH_MAX` bytes plus room for `/` and a control filename.
- `value` holds either decimal digits or the word `max`.
- The first `snprintf()` joins the directory and filename.
- A negative limit means unlimited, represented by the literal kernel value
  `max`. Otherwise `%ld` formats the `long` as decimal text.
- Returning `write_file(...)` forwards either success (`0`) or failure (`-1`).

## `container_cgroup_init()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Enable the needed controllers, create this container’s cgroup,
store its path, and apply process, memory, and swap limits.

**Used by:** `container_run()` calls it before creating the child.

```c
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    snprintf(c->cg_path, sizeof c->cg_path,
             "%s/%s", c->cgroup_base, c->name);
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;
    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
        return -1;

    return 0;
}
```

### Line/block explanation

- `path` temporarily holds the parent control-file path.
- The first `snprintf()` constructs
  `<cgroup_base>/cgroup.subtree_control`.
- Writing `+pids +memory` delegates the process-count and memory controllers to
  child cgroups created below this base.
- The second `snprintf()` writes the full new directory path directly into
  `c->cg_path`. Saving it in the shared configuration object lets later
  functions enter and remove the same cgroup.
- `mkdir(..., 0755)` creates the directory. `0755` gives the owner read, write,
  and traversal permission and others read/traversal permission.
- `mkdir()` returning negative normally means failure. The extra
  `errno != EEXIST` condition deliberately accepts a leftover directory from a
  crashed earlier run. Its limits are rewritten below.
- `fprintf(stderr, ...)` reports other failures. `stderr` is the diagnostic
  stream, kept separate from ordinary command output.
- The three helper calls write the process maximum, memory maximum, and zero
  swap. Zero swap prevents a process from escaping the intended memory behavior
  by moving memory to swap.
- Every failed write returns `-1`; reaching the end returns `0`.

## `container_cgroup_enter()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Put the cloned init process into the newly configured cgroup.

**Used by:** The parent calls it from `container_run()` after writing ID maps and
before releasing the child.

```c
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX + 32];
    char pid_text[16];

    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);
    return write_file(path, pid_text);
}
```

### Variables and lines

- `path` holds `<cg_path>/cgroup.procs`.
- `pid_text` holds a printable decimal PID because cgroup control files accept
  text rather than a binary `pid_t` value.
- The first `snprintf()` builds the control-file path.
- The second converts the host-visible child PID to decimal text.
- Writing that PID to `cgroup.procs` moves the process into the cgroup.
- Returning `write_file()` directly propagates its success or failure.

The child is still blocked on the synchronization pipe at this moment. That
guarantees the requested command never briefly runs outside the limits. When
container init later forks the command, cgroup membership is inherited.
