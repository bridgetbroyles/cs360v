# Project 2 Learning Guide

## Table of contents

1. [Container Fundamentals](00-container-fundamentals.md)
2. [Repository Map and Code Ownership](01-repository-map.md)
3. [The Complete Lifecycle](02-complete-lifecycle.md)
4. [Namespaces, Identity, and Networking](03-namespaces-identity-networking.md)
5. [Building the Container Filesystem](04-filesystem-and-pivot-root.md)
6. [Capabilities and Seccomp](05-capabilities-and-seccomp.md)
7. [PID 1, Cgroups, Cleanup, and Error Paths](06-init-cgroups-cleanup.md)
8. [Function-by-Function Code Guide](07-function-reference.md)
9. [Testing, Debugging, and Interview Review](08-testing-and-review.md)

## How to use this guide

Start with chapters 0–2 if containers are new to you. They establish the story:
what problem a container solves, what the parent and child processes each do,
and why their actions must happen in a particular order.

Chapters 3–6 follow the implementation mechanisms. Chapter 7 is the quick code
reference: use it with `project_2/runtime/container.c` open. Chapter 8 maps each
official test to the relevant code and contains short interview answers.

## The project in one sentence

The runtime creates a child with isolated Linux namespaces, prepares its
identity, resources, network, and filesystem, removes its privileges, launches
a command under a reaping PID 1, and then removes the remaining host-side state.

## Verified result

The implementation was built in the provided Ubuntu 24.04 ARM64 VM and checked
with:

```bash
cd project_2
sudo ./tests/run_tests.sh
```

Result: **32 passed, 0 failed**, including the AddressSanitizer and
UndefinedBehaviorSanitizer check.

