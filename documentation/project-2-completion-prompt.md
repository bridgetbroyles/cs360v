# Project 2 Completion Prompt

Copy the prompt below into a new coding task **after Project 2 has been brought
into the working tree**. It is based on `upstream/main:project_2/SPEC.md`.

---

```text
You are an expert Linux systems programmer. Complete CS360V/CS380 Project 2 in
this repository. Work carefully, implement the assignment fully, and verify it
in the provided Ubuntu VM. Do not modify the assignment specification or tests.

## Scope and source of truth

Read these files completely before changing code:

1. project_2/SPEC.md — authoritative behavioral contract.
2. project_2/README.md — project overview and build/test commands.
3. project_2/SETUP.md — VM, rootfs, and test setup.
4. project_2/runtime/container.h — fixed interface and `struct container`.
5. project_2/runtime/container.c — the student TODO skeleton.
6. project_2/runtime/main.c, util.c, and net.c — provided code; understand
   their contracts but do not change them unless the specification explicitly
   requires a correction.
7. project_2/tests/run_tests.py and observer.c — understand the observable
   behavior the test suite checks. Do not weaken, edit, skip, or replace tests.

Implement **only `project_2/runtime/container.c`** unless a compile issue in a
provided file makes a minimal, clearly justified change unavoidable. Do not
change `container.h` signatures or struct fields. Do not use Docker, a
third-party container library, a privileged helper, or a workaround that
bypasses the intended Linux namespace/cgroup/seccomp mechanisms.

## Required documentation style in the implementation

The code must be easy for a student new to Linux containers to read quickly.

At the very top of every source file you modify, add a compact table of
contents comment. For `container.c`, list every implemented method with one
sentence saying what it does. Example format:

/*
 * Quick map of this file
 * - container_namespaces(): returns the isolation namespaces requested at clone.
 * - container_write_idmaps(): lets container UID/GID 0 map to the caller.
 * ...
 */

Immediately before **every function you implement**, add a short, readable
comment block with these labels:

/*
 * What: <plain-English job of this function>.
 * Called by: <specific function / parent or child lifecycle stage>.
 * Important: <ordering, ownership, privilege, or failure rule that matters>.
 */

Keep these comments concise (normally 3–6 lines), accurate, and specific.
Do not add comments that merely repeat the C syntax. For helper functions you
add, also state who calls them. Use meaningful names and avoid unexplained
magic constants; define a named constant or add a short explanation.

## Assignment requirements

Implement all eleven declared functions in `container.c`:

1. `container_namespaces()`
   - Return the OR of `CLONE_NEWUSER`, `CLONE_NEWPID`, `CLONE_NEWNS`,
     `CLONE_NEWUTS`, and `CLONE_NEWNET`.

2. `container_write_idmaps(c, child)`
   - From the parent, use `write_file()` to write:
     - `/proc/<child>/uid_map`: `0 <getuid()> 1`
     - `/proc/<child>/setgroups`: `deny`
     - `/proc/<child>/gid_map`: `0 <getgid()> 1`
   - Preserve the required ordering: deny `setgroups` before writing `gid_map`.
   - Build paths safely and return `-1` on errors with a useful `container:`
     diagnostic.

3. `container_cgroup_init(c)`
   - Enable `+pids +memory` in `<cgroup_base>/cgroup.subtree_control`.
   - Create `<cgroup_base>/<name>`, tolerating `EEXIST`.
   - Store the full path in `c->cg_path`.
   - Write limits to `pids.max`, `memory.max`, and `memory.swap.max`.
   - Convert a negative pids/memory limit to literal `max`; set swap max to 0.

4. `container_cgroup_enter(c, child)`
   - Write the child PID to `<cg_path>/cgroup.procs`.

5. `container_network()`
   - Best-effort only: bring `lo` up in the new network namespace using an
     `AF_INET`/`SOCK_DGRAM` socket, `SIOCGIFFLAGS`, `IFF_UP | IFF_RUNNING`, and
     `SIOCSIFFLAGS`.
   - Call this before capabilities are dropped. A loopback failure should not
     prevent a container command from running.

6. `container_net_config(c)`
   - In `--net` mode, configure the provided veth named `c->net_ifname`.
   - Apply its IPv4 address and netmask from `c->net_ip` / `c->net_prefix`.
   - Bring it up and add a default route through `c->net_gw`.
   - Use the required socket/ioctl interfaces: `SIOCSIFADDR`, `SIOCSIFNETMASK`,
     `SIOCSIFFLAGS`, and `SIOCADDRT`.

7. `container_setup(c)` — run in the cloned child/init, in this exact order:
   - Set `c->hostname`.
   - Bring up loopback; if enabled, configure the container end of the veth.
   - Make mount propagation private: `MS_REC | MS_PRIVATE`.
   - Bind mount `c->rootfs` onto itself and remount it read-only.
   - Mount writable tmpfs at `<rootfs>/tmp`.
   - Mount tmpfs at `<rootfs>/dev`; create empty targets, then bind host
     `/dev/null` and `/dev/zero` into it.
   - Mount a fresh procfs at `<rootfs>/proc` **before** changing roots.
   - Pivot into the rootfs using `syscall(SYS_pivot_root, ".", ".")` after
     changing directory, detach the old root with `umount2(..., MNT_DETACH)`,
     then `chdir("/")`.
   - Only after setup is complete, drop all capabilities: bounding set,
     permitted/effective/inheritable sets, then `PR_SET_NO_NEW_PRIVS`.
   - Install seccomp last.

8. `container_seccomp()`
   - Build a seccomp-BPF denylist that returns `EPERM` for dangerous syscalls:
     `ptrace`, `mount`, `umount2`, `pivot_root`, `chroot`, `setns`, `unshare`,
     `reboot`, `swapon`, `swapoff`, `kexec_load`, and available `*_module`
     syscalls.
   - Check the active architecture (`AUDIT_ARCH_X86_64` or `AUDIT_ARCH_AARCH64`)
     before loading the syscall number.
   - Allow all other syscalls. Use `PR_SET_NO_NEW_PRIVS` and
     `SYS_seccomp/SECCOMP_SET_MODE_FILTER`; the filter must survive fork/exec.
   - Handle architecture-dependent syscall macros portably with preprocessor
     guards when a syscall is unavailable on the build architecture.

9. `container_init(c)`
   - This is PID 1 in the new PID namespace.
   - Close its write pipe end; block until parent releases it by writing one
     byte; close the read end.
   - Run `container_setup(c)`.
   - `fork()` the requested command; in the command child call
     `execvp(c->argv[0], c->argv)`.
   - Preserve inherited descriptors that the tests may intentionally pass in.
   - As init, repeatedly call `waitpid(-1, ...)` to reap every child/orphan.
   - When the actual command is reaped, return its shell-style status:
     `WEXITSTATUS(status)` or `128 + signal`.

10. `container_run(c)` — parent-side lifecycle, in this exact order:
    - Initialize cgroup.
    - Create `c->sync` pipe.
    - Allocate the `CONTAINER_STACK_SIZE` clone stack and pass its top to
      `clone()` via a small correctly typed trampoline calling `container_init`.
    - Use `container_namespaces() | SIGCHLD` as clone flags.
    - Write child ID maps, enter its cgroup, and if requested call provided
      `container_net_host_setup(c, child)`.
    - Only then release the child through the pipe.
    - Wait for the init child, translate its status, tear down host networking
      if used, remove the cgroup, free/close resources, and return the command
      status.
    - On every failure, close/free what this function owns, avoid deadlocking
      the child, and clean up the cgroup when it was created.

11. `container_cleanup(c)`
    - After the child is reaped, remove `c->cg_path` with `rmdir`.
    - Treat `ENOENT` as harmless; report genuine cleanup failures.

## Engineering constraints

- Check all relevant system-call and library-call return values.
- Print diagnostics to stderr beginning with `container: `, because the test
  harness recognizes that prefix.
- Keep parent-side and child-side file descriptors straight. Avoid deadlocks:
  each process must close the unused end of the synchronization pipe.
- Do not drop capabilities or install seccomp before filesystem/network setup;
  those setup operations require the privileges/syscalls you would remove.
- Do not mount procfs after detaching the old root: it fails in this user
  namespace setup.
- Do not allow rootfs writes except `/tmp`; do not expose host `/dev` broadly.
- Do not allow a failed command exec to masquerade as successful execution.
- Do not leave cgroups, veths, allocated clone stacks, sockets, or file
  descriptors behind after an error or normal completion.

## Required verification

1. Build with `make -C project_2/runtime`.
2. Build the test rootfs with `cd project_2 && ./make-rootfs.sh ./rootfs`.
3. Run the complete provided suite from the Linux VM with
   `cd project_2 && sudo ./tests/run_tests.sh`.
4. Investigate every failing check from its first concrete error; do not hide
   failures with test-specific branches or hardcoded output.
5. Re-run the full suite after the final fix.
6. In the final report, state:
   - files changed;
   - how each assignment part is implemented;
   - the exact test command and outcome;
   - any environmental limitation only if it genuinely prevented verification.

Before finalizing, reread SPEC.md and compare every listed function and required
ordering rule against the implementation. The implementation must be complete,
general, and safe—not merely sufficient for one observed test run.
```
