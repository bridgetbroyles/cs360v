# 06 — PID 1, Cgroups, Cleanup, and Error Paths

## Table of contents

- [Why a container needs init](#why-a-container-needs-init)
- [Command status propagation](#command-status-propagation)
- [Cgroup creation](#cgroup-creation)
- [Limit enforcement](#limit-enforcement)
- [Normal cleanup](#normal-cleanup)
- [Robust error cleanup](#robust-error-cleanup)

## Why a container needs init

The cloned child becomes PID 1 in the new PID namespace. It does not directly
`exec` the requested command. Instead it forks the command and remains alive:

```c
pid_t command = fork();
if (command == 0)
    execvp(c->argv[0], c->argv);
```

When a process exits before its children, those orphans are reparented to PID 1.
If PID 1 never calls `waitpid`, exited orphans remain as zombies. The loop uses
`waitpid(-1, ...)` so it accepts any child:

```c
for (;;) {
    pid_t reaped = waitpid(-1, &status, 0);
    if (reaped == command)
        return status_code(status);
}
```

The official observer creates an orphaned grandchild and then counts zombies in
the container's `/proc`, directly exercising this behavior.

## Command status propagation

`waitpid` returns an encoded status, not the desired numeric result:

```c
if (WIFEXITED(status))
    return WEXITSTATUS(status);
if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
```

Examples:

| Command result | Runtime result |
|---|---:|
| `return 42` | 42 |
| Killed by SIGTERM (15) | 143 |
| `execvp` failure | 127 |

Init returns this value; the runtime parent waits for init and decodes the same
way, preserving it through both process layers.

## Cgroup creation

On cgroup v2, a cgroup is a directory containing kernel-backed control files.
`container_cgroup_init()` performs:

```text
<base>/cgroup.subtree_control ← +pids +memory
mkdir <base>/<container name> (EEXIST is okay)
<group>/pids.max              ← requested count or max
<group>/memory.max            ← requested bytes or max
<group>/memory.swap.max       ← 0
```

The full directory is stored in `c->cg_path` so later functions do not rebuild
or guess it.

`EEXIST` is accepted because a killed prior run may leave an empty directory.
The limits are rewritten, so stale settings cannot silently carry over.

## Limit enforcement

After `clone`, but before release:

```c
write_file("<cg_path>/cgroup.procs", "<child pid>");
```

Moving init is enough: children inherit their parent's cgroup. Therefore, both
the command and all its descendants count toward `pids.max` and `memory.max`.

- At `pids.max`, another `fork` fails with `EAGAIN`.
- At `memory.max`, with swap max zero, the kernel OOM-kills the offending process.

## Normal cleanup

The parent waits for init before removing the cgroup. By then all processes in
the PID namespace are gone, so the directory should be empty:

```c
if (rmdir(c->cg_path) != 0 && errno != ENOENT)
    return report_errno("remove container cgroup");
```

`ENOENT` is harmless because a prior cleanup path may already have removed it.
Mounts do not need host-side unmount calls: their private namespace disappears
with the child.

## Robust error cleanup

The parent tracks acquisition state rather than assuming everything exists:

- `pipe_created` says pipe descriptors need closing.
- `child > 0` says a clone child must be killed and reaped.
- `network_attempted` says host veth teardown should run.
- `c->cg_path[0]` says a cgroup path was established.
- `stack != NULL` is safe to pass to `free` either way.

This supports failures at every stage: cgroup writes, pipe creation, allocation,
clone, ID maps, cgroup entry, release, or wait. Cleanup is idempotent where
practical, meaning repeating it or cleaning partially built state is harmless.

