# 05 — Capabilities and Seccomp

## Table of contents

- [Why both mechanisms exist](#why-both-mechanisms-exist)
- [Bounding-set removal](#bounding-set-removal)
- [Clearing active capability sets](#clearing-active-capability-sets)
- [No new privileges](#no-new-privileges)
- [Seccomp program structure](#seccomp-program-structure)
- [Reading one deny rule](#reading-one-deny-rule)
- [Architecture portability](#architecture-portability)

## Why both mechanisms exist

Capabilities split traditional root power into named privileges such as network
administration. Seccomp filters the syscall interface itself. They answer
different questions:

```text
capabilities: Is this process privileged enough to perform this operation?
seccomp:      Is this syscall allowed to be attempted at all?
```

The runtime installs both only after hostname, network, mounts, and pivoting are
finished.

## Bounding-set removal

The bounding set limits capabilities a later `exec` may acquire:

```c
for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
    prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);
}
```

Iterating through `CAP_LAST_CAP` avoids assuming a hardcoded number of kernel
capabilities.

## Clearing active capability sets

Linux capability version 3 uses two 32-bit data elements:

```c
struct __user_cap_header_struct header = {
    .version = _LINUX_CAPABILITY_VERSION_3,
    .pid = 0, /* current process */
};
struct __user_cap_data_struct data[2];
memset(data, 0, sizeof data);
syscall(SYS_capset, &header, data);
```

Zeroing `effective`, `permitted`, and `inheritable` fields in both elements
removes active capabilities. A raw syscall is used because glibc does not expose
the required `capset` wrapper.

## No new privileges

```c
prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
```

This is permanent for the process and descendants. Future `exec` calls cannot
gain privilege through setuid/setgid bits or file capabilities. It is also a
requirement for an unprivileged process to install a seccomp filter.

## Seccomp program structure

Seccomp-BPF is a tiny kernel-evaluated program. It receives `struct seccomp_data`
for each syscall:

```text
load architecture
if architecture is unexpected → kill process
load syscall number
if number is ptrace       → return EPERM
if number is mount        → return EPERM
...
otherwise                 → allow
```

The denylist covers `ptrace`, mount/root manipulation, namespace creation or
entry, reboot/swap/kexec operations, and kernel-module operations.

The program is installed with:

```c
struct sock_fprog program = {
    .len = sizeof filter / sizeof filter[0],
    .filter = filter,
};
syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program);
```

It is inherited across both `fork()` and `execvp()`, so the requested command
cannot escape it.

## Reading one deny rule

The macro expands to two BPF instructions:

```c
#define DENY_SYSCALL(number) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (number), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, \
             SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
```

- Compare the loaded syscall number with `number`.
- If equal, do not jump; the next instruction returns `EPERM`.
- If unequal, jump over that return and check the next rule.
- `SECCOMP_RET_ERRNO` blocks without killing the command, making the syscall
  behave as if the kernel rejected it normally.

## Architecture portability

Syscall numbers differ between x86-64 and AArch64. The filter first checks:

```c
#if defined(__x86_64__)
#define CONTAINER_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CONTAINER_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif
```

Each optional syscall is guarded with `#ifdef __NR_name`. This lets the same
source compile when an older architecture does not define a particular syscall.
Rejecting an unexpected ABI before comparing numbers prevents an attacker from
using another syscall-number interpretation to bypass the denylist.

