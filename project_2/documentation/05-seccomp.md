# Seccomp System-Call Filtering

Capabilities restrict privileged operations, but seccomp restricts which system
calls a process may attempt at all. The kernel evaluates a small BPF program on
every system call. This implementation returns `EPERM` for a dangerous denylist
and allows other calls.

## Architecture constant

```c
#if defined(__x86_64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define OUR_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "container_seccomp() only supports x86_64 and aarch64"
#endif
```

The compiler defines either `__x86_64__` or `__aarch64__` for the supported
machines. System-call numbers differ by architecture, so the filter first
verifies the syscall’s audit architecture. Unsupported builds stop at compile
time rather than silently installing an incorrect security policy.

## Denied system-call array

```c
static const int denied_syscalls[] = {
    __NR_ptrace,
    __NR_mount,
    __NR_umount2,
    __NR_pivot_root,
    __NR_chroot,
    __NR_setns,
    __NR_unshare,
    __NR_reboot,
    __NR_swapon,
    __NR_swapoff,
    __NR_kexec_load,
    __NR_init_module,
    __NR_finit_module,
    __NR_delete_module,
};

#define NUM_DENIED_SYSCALLS \
    (sizeof denied_syscalls / sizeof denied_syscalls[0])
```

- `static` makes the array private to this file.
- `const` prevents accidental modification.
- Each `__NR_*` constant is the current architecture’s numeric syscall ID.
- The list blocks tracing, mount/root manipulation, joining or creating more
  namespaces, machine reboot/swap/kernel replacement, and kernel-module changes.
- Dividing total array bytes by one element’s bytes computes the element count
  without hard-coding 14.

These declarations are implementation support, not functions and not separate
student requirements. The denylist itself was required by the specification.

## BPF structures

`struct sock_filter` represents one classic BPF instruction. The important
instruction families here are:

- `BPF_LD`: load a field from the kernel-provided `seccomp_data` record;
- `BPF_JMP | BPF_JEQ`: compare a loaded value and jump based on equality;
- `BPF_RET`: return a seccomp decision.

`struct sock_fprog` contains the final instruction count and a pointer to the
instruction array. `BPF_STMT` and `BPF_JUMP` are macros that produce instruction
initializers.

## `container_seccomp()`

**Assignment status:** One of the 11 functions explicitly required by the
specification.

**Purpose:** Build and install the architecture-aware system-call denylist.

**Used by:** `container_setup()` calls it last. The requested command inherits
the installed filter across `fork()` and `execvp()`.

### Allocate the instruction array

```c
int container_seccomp(void)
{
    struct sock_filter filter[4 + 2 * NUM_DENIED_SYSCALLS + 1];
    size_t count = 0;
```

- Four initial instructions load/check architecture and load the syscall number.
- Every denied syscall needs two instructions: compare, then return `EPERM`.
- One final instruction allows unmatched calls.
- `count` is both the next free array index and the number of completed
  instructions. `size_t` is the standard unsigned type for array sizes.

### Verify architecture

```c
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, arch));
    filter[count++] = (struct sock_filter)BPF_JUMP(
        BPF_JMP | BPF_JEQ | BPF_K, OUR_AUDIT_ARCH, 1, 0);
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
```

- `count++` uses the current index, then advances to the next slot.
- `BPF_W` loads one 32-bit word. `BPF_ABS` uses an absolute byte offset.
- `offsetof(struct seccomp_data, arch)` safely computes the architecture field’s
  location instead of assuming structure layout.
- The jump compares the loaded value to `OUR_AUDIT_ARCH`. On equality it skips
  one instruction, bypassing the kill. On mismatch it skips zero and executes
  `SECCOMP_RET_KILL_PROCESS`.
- Killing a foreign ABI prevents syscall-number confusion from bypassing rules.

The cast to `struct sock_filter` turns the macro initializer into an assignable
compound value.

### Load the syscall number

```c
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        offsetof(struct seccomp_data, nr));
```

This replaces the currently loaded architecture value with the current syscall
number from `seccomp_data.nr`.

### Generate one comparison pair per denied call

```c
    for (size_t i = 0; i < NUM_DENIED_SYSCALLS; i++) {
        filter[count++] = (struct sock_filter)BPF_JUMP(
            BPF_JMP | BPF_JEQ | BPF_K,
            (unsigned int)denied_syscalls[i], 0, 1);
        filter[count++] = (struct sock_filter)BPF_STMT(
            BPF_RET | BPF_K,
            SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    }
```

- `i` visits every syscall number in the array.
- `BPF_K` means compare against an immediate constant.
- If equal, the jump skips zero instructions, so the next instruction returns
  an error. If unequal, it skips one, moving to the next comparison pair.
- `SECCOMP_RET_ERRNO` tells the kernel to reject the syscall and make it appear
  to return `-1`.
- `EPERM` is placed in the permitted data bits and becomes the caller’s errno,
  meaning “operation not permitted.”

### Allow every unmatched syscall

```c
    filter[count++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
```

Execution reaches this instruction only if no denylist comparison matched.

### Package and install the filter

```c
    struct sock_fprog program = {
        .len = (unsigned short)count,
        .filter = filter,
    };
```

- `.len` is the number of valid instructions, converted to the 16-bit type
  required by the kernel structure.
- `.filter` points to the first instruction.
- This designated-initializer syntax names fields rather than relying on order.

```c
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "container: set no_new_privs: %s\n", strerror(errno));
        return -1;
    }
```

`drop_capabilities()` already set this flag, but setting it again is harmless
and makes this function safe if called independently. An unprivileged process
may install a seccomp filter only after promising that `exec` cannot increase
its privileges.

```c
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) < 0) {
        fprintf(stderr, "container: install seccomp filter: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}
```

- `SYS_seccomp` calls the kernel directly because glibc does not universally
  expose a convenient wrapper.
- `SECCOMP_SET_MODE_FILTER` selects BPF filtering mode.
- Flags are zero; `&program` supplies the packaged filter.
- On success, the policy immediately applies to container init and is inherited
  by the command.

## Why seccomp must be last

This filter blocks `mount`, `umount2`, and `pivot_root`. Installing it before
filesystem setup would prevent the container from completing its own isolation.
The final order is: use the needed powers, drop capabilities, install seccomp,
and only then launch untrusted command code.
