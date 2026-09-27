# 02 — The Complete Lifecycle

## Table of contents

- [Parent preparation](#parent-preparation)
- [Clone and synchronization](#clone-and-synchronization)
- [Child setup](#child-setup)
- [Command execution](#command-execution)
- [Teardown](#teardown)
- [Failure paths](#failure-paths)

## Parent preparation

Provided `main()` parses arguments and calls `container_run(&c)`. Our runtime
initializes owned state before acquiring resources:

```c
c->cg_path[0] = '\0';
c->net_host_if[0] = '\0';
c->sync[0] = c->sync[1] = -1;
```

`-1` means a descriptor is not open. An empty first character means a named
resource has not been created. These sentinels make the shared cleanup path safe.

The cgroup is created first so the child can enter it before doing any work.

## Clone and synchronization

The parent creates a pipe and allocates a stack:

```c
pipe(c->sync);
stack = malloc(CONTAINER_STACK_SIZE);
child = clone(child_entry,
              (char *)stack + CONTAINER_STACK_SIZE,
              container_namespaces() | SIGCHLD,
              c);
```

The stack top is passed because the supported architectures grow stacks toward
lower addresses. `child_entry` adapts the callback type:

```c
static int child_entry(void *argument)
{
    return container_init(argument);
}
```

The child closes its write end and blocks reading one byte. While blocked, the
parent performs external setup:

```c
container_write_idmaps(c, child);
container_cgroup_enter(c, child);
container_net_host_setup(c, child); /* only with --net */
```

The parent then writes one byte, which is a release signal—not application data.

## Child setup

After release, init calls `container_setup(c)` in this order:

```text
hostname
loopback and optional veth
private mount propagation
read-only rootfs bind
writable /tmp
minimal /dev
private /proc
pivot_root
capability removal
seccomp installation
```

Security restrictions are installed only after operations needing privilege or
denied syscalls have completed.

## Command execution

Init stays PID 1 and forks the requested command:

```c
pid_t command = fork();
if (command == 0) {
    execvp(c->argv[0], c->argv);
    _exit(127);
}
```

The command inherits namespaces, mounts, capability state, seccomp, and intended
file descriptors. Status 127 means `execvp` failed.

Init uses `waitpid(-1, ...)`, not `waitpid(command, ...)`, so it can also reap
orphan descendants reparented to PID 1. It returns when the original command is
reaped.

## Teardown

After init exits, Linux destroys the child namespaces and their mounts. The
parent removes remaining host-side state:

```c
container_net_host_teardown(c); /* if --net was attempted */
container_cleanup(c);           /* remove empty cgroup */
free(stack);
```

The returned result remains the command status, such as 42 or `128 + signal`.

## Failure paths

Every `container_run()` failure reaches one cleanup block:

```c
if (pipe_created) { close open pipe ends; }
if (child > 0) { kill(child, SIGKILL); wait_for_pid(child, &status); }
if (network_attempted) { container_net_host_teardown(c); }
if (c->cg_path[0]) { container_cleanup(c); }
free(stack);
```

Killing and reaping prevents a setup failure from leaving a blocked init behind.
`SIGKILL` is used because namespace PID 1 has special handling for ordinary
signals but cannot ignore `SIGKILL`.

