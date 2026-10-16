# Project 2 `container.c` Documentation

This folder explains the exact implementation on the `p2-helen` branch. It is
written for a reader who is new to containers and Linux systems programming.
Every function in `runtime/container.c` is covered, including the private helper
functions that were not required by the assignment specification.

The implementation was run in the Ubuntu VM against the official test suite:
**32 tests passed and 0 failed**.

## How to read these files

1. [00-foundations-and-data.md](00-foundations-and-data.md) introduces the
   process model, the `struct container`, return values, system calls, and the
   most common C data structures.
2. [01-namespaces-and-identity.md](01-namespaces-and-identity.md) explains
   `container_namespaces()` and `container_write_idmaps()`.
3. [02-cgroups.md](02-cgroups.md) explains `write_cgroup_limit()`,
   `container_cgroup_init()`, and `container_cgroup_enter()`.
4. [03-filesystem-and-setup.md](03-filesystem-and-setup.md) explains
   `bind_device()`, `setup_filesystem()`, `drop_capabilities()`, and
   `container_setup()`.
5. [04-networking.md](04-networking.md) explains `container_network()`,
   `make_ipv4_address()`, and `container_net_config()`.
6. [05-seccomp.md](05-seccomp.md) explains `container_seccomp()` and the
   seccomp-related constants and data structures.
7. [06-init-run-and-cleanup.md](06-init-run-and-cleanup.md) explains
   `status_to_exit_code()`, `container_init()`, `child_entry()`,
   `container_run()`, and `container_cleanup()`.
8. [07-whole-project-summary.md](07-whole-project-summary.md) connects all the
   functions into one end-to-end story and summarizes the design decisions.
9. [08-quiz-cheatsheet.md](08-quiz-cheatsheet.md) is a compact vocabulary,
   lifecycle, isolation, networking, and seccomp reference for the quiz.
10. [09-interview-question-bank.md](09-interview-question-bank.md) provides
    likely implementation, data-structure, design, and debugging questions with
    answer checkpoints for spoken practice.
11. [vocab.md](vocab.md) is an exhaustive Control-F reference for project
    vocabulary, flags, structures, variables, functions, errors, and options.

## What the assignment asked us to implement

The specification explicitly required these eleven functions:

| Required function | Main responsibility |
|---|---|
| `container_namespaces()` | Select the five new namespaces. |
| `container_write_idmaps()` | Give container root a host UID/GID mapping. |
| `container_cgroup_init()` | Create the cgroup and write resource limits. |
| `container_cgroup_enter()` | Put container init into that cgroup. |
| `container_setup()` | Coordinate child-side isolation and restrictions. |
| `container_network()` | Bring loopback up. |
| `container_net_config()` | Configure the optional veth interface. |
| `container_seccomp()` | Install the syscall filter. |
| `container_init()` | Act as PID 1, launch the command, and reap children. |
| `container_run()` | Coordinate the complete parent-side lifecycle. |
| `container_cleanup()` | Remove the container cgroup. |
