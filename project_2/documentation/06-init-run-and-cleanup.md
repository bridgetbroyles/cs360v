# Init, Full Lifecycle, and Cleanup

## `status_to_exit_code()`

**Assignment status:** Added private helper. The specification required status
propagation but did not require this function.

**Purpose:** Convert the encoded integer produced by `waitpid()` into the exit
code users expect from a command.

**Used by:** `container_init()` for the requested command and `container_run()`
for container init.

```c
static int status_to_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}
```

- `status` is not already an exit code. It contains bit fields describing why a
  process changed state.
- `WIFEXITED(status)` is true for a normal `exit()` or return from `main()`.
- `WEXITSTATUS(status)` extracts that process’s 0–255 code.
- `WIFSIGNALED(status)` is true if a signal killed the process.
- The common shell convention is `128 + signal number`; for example SIGTERM 15
  becomes 143.
- The fallback `1` handles an unexpected status type as general failure.

## `container_init()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Run as PID 1 in the container’s PID namespace, wait for parent-side
setup, create the isolated environment, launch the requested command, reap all
children, and return the command’s exit status.

**Used by:** `child_entry()`, which is passed to `clone()`.

### Wait for the parent

```c
int container_init(struct container *c)
{
    char release_byte;
    close(c->sync[1]);
    if (read(c->sync[0], &release_byte, 1) != 1) {
        fprintf(stderr, "container: parent did not release the container\n");
        return 1;
    }
    close(c->sync[0]);
```

- `release_byte` is a one-byte destination; its value does not matter. Receiving
  exactly one byte is the event that matters.
- The child closes `sync[1]`, its unused copy of the pipe’s write end.
- `read()` on `sync[0]` blocks while the parent writes ID maps, enters the
  cgroup, and optionally sets up host networking.
- A result other than one means no valid release byte arrived. In particular,
  reading zero means all write ends closed, usually because parent setup failed.
- After a valid release, the read end is no longer needed and is closed.

### Build isolation and fork the command

```c
    if (container_setup(c) < 0)
        return 1;

    pid_t command_pid = fork();
    if (command_pid < 0) {
        fprintf(stderr, "container: fork: %s\n", strerror(errno));
        return 1;
    }
```

- `container_setup()` establishes hostname, networking, filesystem, capability,
  and seccomp state. Any mandatory failure prevents command execution.
- `fork()` creates a second process inside the same namespaces.
- The return value is negative on failure, zero in the newly created command
  child, and the new PID in container init.
- `command_pid` lets init distinguish the main command from any adopted orphan.

```c
    if (command_pid == 0) {
        execvp(c->argv[0], c->argv);
        fprintf(stderr, "container: exec %s: %s\n",
                c->argv[0], strerror(errno));
        _exit(127);
    }
```

- Only the fork child enters this block.
- `execvp()` replaces its current program image with the requested executable.
  `argv[0]` names the program and the full NULL-terminated `argv` supplies its
  arguments. The `p` form searches `PATH` when needed.
- A successful `execvp()` never returns.
- If it returns, execution failed; the diagnostic uses the still-valid errno.
- `_exit(127)` terminates immediately without running inherited stdio cleanup or
  exit handlers. Code 127 conventionally means the command could not execute.

### Reap children as PID 1

```c
    for (;;) {
        int status;
        pid_t reaped = waitpid(-1, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            return 1;
        }
        if (reaped == command_pid)
            return status_to_exit_code(status);
    }
}
```

- `for (;;)` is an intentional infinite loop, exited by `return`.
- `waitpid(-1, ...)` waits for any child, not only the main command. PID 1 adopts
  orphaned descendants, so this is what prevents zombies.
- `status` receives the encoded termination information.
- `reaped` identifies which child was collected.
- `EINTR` means a signal interrupted the wait before a child was collected; the
  correct response is retrying.
- Other wait errors end init with failure.
- Reaped helper/orphan processes are simply discarded and the loop continues.
- When the main command is reaped, its converted status becomes init’s result.

## `child_entry()`

**Assignment status:** Added private helper. The specification suggested a
“small trampoline,” but this exact function was not one of the 11 required
public functions.

**Purpose:** Adapt the typed container pointer function to `clone()`’s required
callback signature.

**Used by:** `container_run()` passes it as the first argument to `clone()`.

```c
static int child_entry(void *arg)
{
    return container_init((struct container *)arg);
}
```

- `clone()` requires a callback of type `int (*)(void *)`.
- `arg` is therefore an untyped `void *`.
- The cast restores `struct container *`, the type `container_init()` expects.
- The returned status becomes the clone child’s exit status.

## `container_run()`

**Assignment status:** One of the 11 required functions and the only function
in `container.c` called directly by provided `main.c`.

**Purpose:** Own and coordinate the complete parent-side lifecycle.

### Create cgroup and synchronization pipe

```c
int container_run(struct container *c)
{
    if (container_cgroup_init(c) < 0)
        return 1;

    if (pipe(c->sync) < 0) {
        fprintf(stderr, "container: pipe: %s\n", strerror(errno));
        container_cleanup(c);
        return 1;
    }
```

- `c` already contains parsed configuration from `main()`.
- The cgroup is created first so failure occurs before any child exists.
- `pipe(c->sync)` fills index 0 with a read descriptor and index 1 with a write
  descriptor.
- If pipe creation fails, the already-created cgroup is removed before return.

### Allocate the clone stack

```c
    char *stack = malloc(CONTAINER_STACK_SIZE);
    if (stack == NULL) {
        fprintf(stderr, "container: out of memory for the child stack\n");
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }
```

- Unlike `fork()`, Linux `clone()` expects the caller to supply stack memory for
  the child callback.
- `CONTAINER_STACK_SIZE` is provided as one MiB (`1 << 20`).
- `malloc()` returns a pointer to heap memory or `NULL` on failure.
- The error path releases both pipe descriptors and the cgroup.

### Clone container init

```c
    pid_t child = clone(child_entry,
                        stack + CONTAINER_STACK_SIZE,
                        container_namespaces() | SIGCHLD,
                        c);
    if (child < 0) {
        fprintf(stderr, "container: clone: %s\n", strerror(errno));
        free(stack);
        close(c->sync[0]);
        close(c->sync[1]);
        container_cleanup(c);
        return 1;
    }
```

- `child_entry` is the child’s starting function.
- On supported architectures the stack grows toward lower addresses, so clone
  receives the address just past the allocation: `stack + size`.
- Namespace flags isolate five resource types; `SIGCHLD` gives conventional
  child termination notification.
- `c` becomes `child_entry`’s `void *arg`.
- The parent receives the host-visible child PID. The child begins in
  `child_entry()` and blocks on the pipe in `container_init()`.
- Clone failure frees every resource created so far.

### Perform parent-only setup

```c
    int setup_ok = (container_write_idmaps(c, child) == 0);
    setup_ok = setup_ok && (container_cgroup_enter(c, child) == 0);

    if (c->net_enabled)
        (void)container_net_host_setup(c, child);
```

- `setup_ok` is a Boolean-like integer: 1 for success, 0 for failure.
- ID-map setup must happen from the parent using `/proc/<child>`.
- `&&` short-circuits. If ID mapping failed, cgroup entry is not attempted.
- With networking enabled, the provided host helper creates the bridge/veth and
  moves the child-side interface into the network namespace.
- Its result is deliberately ignored because networking is best-effort in this
  implementation and provided helper.

### Release or cancel the child

```c
    close(c->sync[0]);
    if (setup_ok && write(c->sync[1], "x", 1) != 1) {
        fprintf(stderr, "container: release child: %s\n", strerror(errno));
        setup_ok = 0;
    }
    close(c->sync[1]);
```

- The parent closes its unused read end.
- If setup succeeded, it writes exactly one arbitrary byte. The child’s blocked
  `read()` returns and execution continues.
- If setup failed, the write is skipped. Closing the write end makes the child’s
  read return EOF, so it exits without running the command.
- A failed/short write changes `setup_ok` to false.
- Closing the write end is required in either case so the child cannot wait
  forever.

### Wait, tear down, and return status

```c
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "container: waitpid: %s\n", strerror(errno));
            free(stack);
            return 1;
        }
    }
    free(stack);
```

- The parent waits for the clone child, which is container init.
- `EINTR` causes a retry. Another error frees the stack and returns failure.
- Once the child has exited, it no longer uses the supplied stack, so `free()`
  is safe.

```c
    if (c->net_enabled)
        (void)container_net_host_teardown(c);

    (void)container_cleanup(c);

    if (!setup_ok)
        return 1;
    return status_to_exit_code(status);
}
```

- Provided network teardown removes the host veth in `--net` mode.
- Cgroup cleanup runs after the process is reaped, when the group should be empty.
- Cleanup is best-effort here, so its result does not overwrite the command’s
  meaningful exit status.
- A failed setup always reports runtime failure (`1`). Otherwise the encoded
  init status is converted. Init itself returned the requested command status,
  so the original result propagates through two waits to the user.

## `container_cleanup()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Remove the per-container cgroup directory after all contained
processes have exited.

**Used by:** `container_run()` during normal teardown and selected early-failure
paths.

```c
int container_cleanup(struct container *c)
{
    if (c->cg_path[0] == '\0')
        return 0;

    if (rmdir(c->cg_path) < 0 && errno != ENOENT) {
        fprintf(stderr, "container: remove cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }
    return 0;
}
```

- `cg_path[0] == '\0'` means the character array begins with the string
  terminator and therefore contains an empty path. There is nothing to remove.
- `rmdir()` removes only an empty directory. Cgroup virtual directories can be
  removed only when no processes remain, which is why cleanup follows wait.
- `ENOENT` means it is already absent and is treated as success.
- Other failures are reported and return `-1`.
- Successful removal or harmless absence returns `0`.

The kernel automatically tears down namespaces and their private mounts when
the last process exits. The cgroup directory and optional host veth are the
explicit host-side resources this project must remove.
