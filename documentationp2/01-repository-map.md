# 01 — Repository Map and Code Ownership

## Table of contents

- [What we implemented](#what-we-implemented)
- [What was provided](#what-was-provided)
- [The container structure](#the-container-structure)
- [Call relationships](#call-relationships)

## What we implemented

All student work is in `project_2/runtime/container.c`. It implements eleven
public functions plus small private helpers.

| Implemented function | One-sentence purpose |
|---|---|
| `container_namespaces` | Returns namespace flags used by `clone`. |
| `container_write_idmaps` | Makes UID/GID 0 meaningful inside the user namespace. |
| `container_cgroup_init` | Creates a cgroup and writes limits. |
| `container_cgroup_enter` | Moves init into that cgroup. |
| `container_setup` | Constructs the child environment in dependency order. |
| `container_network` | Brings isolated loopback up. |
| `container_net_config` | Configures the optional container veth and route. |
| `container_seccomp` | Installs the syscall denylist. |
| `container_init` | Runs as PID 1, launches the command, and reaps children. |
| `container_run` | Coordinates the parent-side lifecycle. |
| `container_cleanup` | Removes the empty cgroup. |

## What was provided

| File | Provided responsibility |
|---|---|
| `runtime/main.c` | Parses options, fills `struct container`, calls `container_run`. |
| `runtime/container.h` | Defines the fixed structure, constants, and signatures. |
| `runtime/util.c` | Provides `write_file(path, value)` for pseudo-files. |
| `runtime/net.c` | Creates the host bridge/veth and moves one end to the child. |
| `runtime/Makefile` | Builds provided files and the implementation. |
| `make-rootfs.sh` | Creates the BusyBox root filesystem. |
| `tests/*` | Builds workloads and checks behavior inside and outside. |

The host half of `--net` was provided. Our code only calls it:

```c
container_net_host_setup(c, child);
container_net_host_teardown(c);
```

Our `container_net_config()` implements the child half.

## The container structure

```c
struct container {
    /* main.c fills these configuration fields */
    char *const *argv;
    const char *name;
    const char *hostname;
    const char *rootfs;
    long pids_max;
    long mem_max;
    const char *cgroup_base;
    int net_enabled;
    const char *net_ifname;
    const char *net_ip;
    int net_prefix;
    const char *net_gw;

    /* our runtime fills these state fields */
    int sync[2];
    char cg_path[PATH_MAX];
    char net_host_if[16];
};
```

Ownership rules:

- Configuration strings and `argv` are borrowed; our code does not free them.
- Our runtime creates and closes `sync` descriptors.
- Cgroup initialization fills `cg_path`; cleanup consumes it.
- Provided networking fills `net_host_if`; teardown consumes it.
- `container_run()` owns and frees the clone stack.

## Call relationships

```text
main
└── container_run
    ├── container_cgroup_init
    ├── container_namespaces
    ├── clone → child_entry → container_init
    │                          ├── container_setup
    │                          │   ├── container_network
    │                          │   ├── container_net_config (with --net)
    │                          │   ├── drop_capabilities
    │                          │   └── container_seccomp
    │                          ├── fork
    │                          ├── execvp(command)
    │                          └── waitpid(-1)
    ├── container_write_idmaps
    ├── container_cgroup_enter
    ├── provided host network setup/teardown
    └── container_cleanup
```

