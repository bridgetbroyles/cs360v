# Project 2 Vocabulary and Identifier Reference

Use Control-F with the exact word, function, flag, field, file, or error shown
in the code. Definitions are intentionally short and project-specific.

## One-minute project vocabulary

### Container

An ordinary Linux process environment with separate views of selected kernel
resources. This project creates the separation with namespaces, applies limits
with a cgroup, and restricts operations with capabilities and seccomp.

### Host

The real Linux system running the container runtime. The host and container use
the same kernel, but processes inside receive different namespace views.

### Container init

The process created by `clone()`. It is an ordinary child PID on the host but
appears as PID 1 inside the new PID namespace. `container_init()` runs there.

### Requested command

The program named after `--` on the command line. Container init forks a new
child and `execvp()` replaces that child with this program.

### Namespace

A kernel feature that gives a process a separate view of one type of resource.
This project creates user, PID, mount, UTS, and network namespaces.

### Cgroup

A kernel-managed process group used here to limit process count, memory, and
swap. Descendants inherit their parent's cgroup membership.

### Capability

One individually controlled piece of traditional root authority. The runtime
uses capabilities during setup and then removes them all.

### Seccomp

A kernel system-call filter. This project returns `EPERM` for a required list
of dangerous calls and allows unmatched calls.

### Rootfs

The directory tree that becomes `/` inside the container. It contains the
command and the files that command can see.

### Synchronization pipe

The parent-to-child startup gate stored in `c->sync`. Init waits for one byte
while the host parent writes identity maps and cgroup membership.

## Project files and ownership

### `container.c`

The student implementation file. It contains the eleven required functions and
the private helpers added to organize the implementation.

### `container.h`

Provided interface. It defines `struct container`, constants, and function
declarations. The assignment says not to change it.

### `main.c`

Provided entry point. It parses command-line options, fills `struct container`,
and calls `container_run()`.

### `net.c`

Provided host-side optional networking. It creates the host bridge and veth
pair, moves one end into the child's network namespace, and tears down the host
veth state later.

### `util.c`

Provided helper file containing `write_file()`.

### `SPEC.md`

The authoritative assignment contract describing the required functions,
behavior, ordering, tests, and submission expectations.

### Student-added private helpers

`write_cgroup_limit()`, `bind_device()`, `setup_filesystem()`,
`drop_capabilities()`, `make_ipv4_address()`, `status_to_exit_code()`, and
`child_entry()` were added to organize required work.

## `struct container` and every important field

### `struct container`

The shared configuration and runtime-state record passed through the entire
runtime. `main.c` fills configuration fields; runtime functions fill `sync`,
`cg_path`, and `net_host_if`.

### `c`

The usual pointer name for the current `struct container`. Functions use
`c->field` to read configuration or save runtime state.

### `c->argv`

A null-terminated array containing the requested command followed by its
arguments. `execvp(c->argv[0], c->argv)` runs it inside the new rootfs.

### `c->name`

The container identifier. This implementation also uses it as the cgroup
directory name.

### `c->hostname`

The hostname written into the container's UTS namespace by `sethostname()`.

### `c->rootfs`

The host path to the directory that `setup_filesystem()` turns into `/` inside
the container.

### `c->pids_max`

The requested value for `pids.max`. A negative value is converted to `max`.

### `c->mem_max`

The requested value for `memory.max`, measured in bytes. A negative value is
converted to `max`.

### `c->cgroup_base`

The parent cgroup directory, normally `/sys/fs/cgroup`.

### `c->net_enabled`

Zero normally and nonzero after `--net`. It controls whether host veth setup,
container-side `ceth0` configuration, and host teardown are attempted.

### `c->net_ifname`

The interface name inside the container, normally `ceth0`.

### `c->net_ip`

The container interface's IPv4 address written as text, normally `10.44.0.2`.

### `c->net_prefix`

The subnet prefix length, normally 24. The implementation converts it into a
32-bit netmask.

### `c->net_gw`

The default gateway address, normally the host bridge address `10.44.0.1`.

### `c->sync`

A two-element array of pipe file descriptors. Index zero is the read end and
index one is the write end.

### `c->sync[0]`

The pipe's read end. Container init blocks reading one release byte from it.

### `c->sync[1]`

The pipe's write end. The host parent writes one byte after required setup.

### `c->cg_path`

The complete path to this container's cgroup directory. Cgroup initialization
fills it; enter and cleanup use it later.

### `c->net_host_if`

The generated host-side veth interface name. Provided `net.c` fills it and uses
it during teardown.

## Important local variables

### `child`

In `container_run()`, the host-visible PID returned by `clone()`. The parent
uses it in proc mapping paths, cgroup membership, networking, and `waitpid()`.

### `path`

A reusable text buffer. Depending on the function, it holds a proc control-file
path, cgroup control-file path, or a path under rootfs.

### `mapping`

The text written to `uid_map` or `gid_map`, such as `0 1000 1`.

### `file_name`

The cgroup control-file name passed to `write_cgroup_limit()`, such as
`pids.max` or `memory.max`.

### `limit`

The numeric limit passed to `write_cgroup_limit()`. Negative means unlimited
and is written as `max`.

### `value`

The text form of a cgroup limit: either decimal digits or `max`.

### `host_path`

The host device path built by `bind_device()`, such as `/dev/null`.

### `target_path`

The future container device path under rootfs, such as
`<rootfs>/dev/null`.

### `fd`

A file descriptor returned by `open()`. In `bind_device()` it refers to the
placeholder device file. In `write_file()` it refers to a kernel control file.

### `cap`

The current capability number in the loop from zero through `CAP_LAST_CAP`.

### `header`

The capability API header passed to `SYS_capset`. It selects version three and
PID zero, meaning the calling process.

### `data`

Two zeroed capability records passed to `SYS_capset`. Zero bits request no
effective, permitted, or inheritable capabilities.

### `sock`

The IPv4 datagram socket used as a control handle for networking ioctls. The
configuration functions do not send application traffic through it.

### `request`

A `struct ifreq` containing an interface name plus the address, netmask, or
flags used by one ioctl request.

### `addr`

Generic socket-address storage passed to `make_ipv4_address()`.

### `ip_text`

The human-readable IPv4 address string passed to `make_ipv4_address()`.

### `ipv4`

A `struct sockaddr_in` view of the generic address storage.

### `netmask`

A `struct sockaddr_in` view of `request.ifr_netmask` used to fill the binary
IPv4 mask.

### `mask_bits`

The 32-bit subnet mask calculated from `c->net_prefix` before conversion to
network byte order.

### `route`

A `struct rtentry` filled with default destination, mask, gateway, and flags.

### `denied_syscalls`

The array of syscall numbers that the seccomp filter returns `EPERM` for.

### `filter`

The array of classic-BPF instructions constructed by `container_seccomp()`.

### `count`

The number of completed seccomp instructions and the index of the next free
instruction slot.

### `program`

The `struct sock_fprog` that packages the filter array and its final length for
the seccomp system call.

### `status`

The encoded process-completion value filled by `waitpid()`. It is not directly
the child's exit code.

### `release_byte`

Storage for the one-byte parent-to-init startup signal. Its actual character
value is unimportant.

### `command_pid`

The PID returned when container init forks the requested command. Init uses it
to recognize when the main workload has finished.

### `reaped`

The PID returned by `waitpid(-1, ...)`, identifying which child was collected.

### `arg`

The generic pointer passed by `clone()` to `child_entry()`. The adapter converts
it back to `struct container *`.

### `stack`

The allocated memory used as container init's clone stack. The parent frees it
only after init exits.

### `setup_ok`

The parent-side success flag tracking ID-map creation, cgroup entry, and the
release-byte write. False means the command was not successfully released.

### `cidr`, `pid`, and `tmp`

Variables in provided `net.c`. `cidr` holds the bridge address plus prefix,
`pid` holds the child PID as text, and `tmp` holds the temporary container-side
veth name before it becomes `ceth0`.

## Namespace flags

### `CLONE_NEWUSER`

Creates a new user namespace. UID and GID values inside are translated through
`uid_map` and `gid_map` to identities outside.

### `CLONE_NEWPID`

Creates a new PID namespace for the child. The cloned child becomes PID 1 in
that namespace.

### `CLONE_NEWNS`

Creates a new mount namespace with a separate mount-table view. The name is
historical: `NEWNS` means new mount namespace.

### `CLONE_NEWUTS`

Creates a new UTS namespace containing a separate hostname and domain name.

### `CLONE_NEWNET`

Creates a new network namespace with separate interfaces, addresses, routes,
and network state.

### `SIGCHLD`

The low-byte clone exit signal used here. The kernel notifies the parent when
the cloned child exits, and the child behaves as a normal waitable child.

### `CLONE_VM`

Not used. If present, parent and child would share memory like threads. Its
absence means the cloned process does not share the parent's address space.

## Identity vocabulary and control files

### UID

User identifier. UID zero is the root user identity inside a user namespace.

### GID

Group identifier. GID zero is the root group identity inside the user
namespace. It is separate from UID zero and participates in group ownership
and permission checks.

### Inside ID

The numeric UID or GID a process sees inside its user namespace.

### Outside ID

The corresponding UID or GID the parent namespace and host use for that
process.

### `0 <host-id> 1`

User-namespace map format: start at inside ID zero, map it to one host ID, and
map exactly one ID. It does not grant host UID or GID zero.

### `/proc/<child>/uid_map`

The proc control file where the parent maps container UID zero to the caller's
host UID.

### `/proc/<child>/setgroups`

The control file written with `deny` before the unprivileged GID map is written.

### `/proc/<child>/gid_map`

The proc control file where the parent maps container GID zero to the caller's
host GID.

### Supplementary groups

Additional groups beyond a process's primary GID. Writing `deny` to setgroups
disables changing this list in the new user namespace.

### `getuid()`

Returns the host UID of the runtime process. It becomes the outside UID in the
map.

### `getgid()`

Returns the host GID of the runtime process. It becomes the outside GID in the
map.

## Cgroup vocabulary and control files

### Cgroup v2

The unified cgroup interface used by this project. Directories represent
cgroups and files inside them configure limits and membership.

### `cgroup.subtree_control`

The parent control file. Writing `+pids +memory` enables those controllers for
child cgroups.

### `cgroup.procs`

The membership file. Writing init's host PID moves init into the cgroup; later
descendants inherit that membership.

### `pids.max`

Maximum total processes allowed in the cgroup and its descendant process tree.

### `memory.max`

Maximum memory usage allowed for the cgroup.

### `memory.swap.max`

Maximum swap usage. This implementation writes zero.

### `max`

The cgroup v2 text value meaning no configured limit. The runtime converts a
negative `long` to this word.

### Controller delegation

Enabling controllers in the parent cgroup so child cgroups can use their
control files.

### `mkdir()`

Creates the cgroup directory. In cgroup v2, creating the directory creates the
cgroup.

### `rmdir()`

Removes the empty cgroup directory during cleanup.

## Filesystem and mount vocabulary

### Mount

A kernel attachment that makes a filesystem or existing path appear at a
selected path.

### Mount namespace

The namespace selected by `CLONE_NEWNS`. It gives the process its own mount
table.

### Mount propagation

Rules controlling whether mount changes are copied between related mount
trees. This implementation changes the tree to private propagation.

### Bind mount

A mount that makes an existing file or directory appear at another path. It
does not copy the underlying contents.

### Remount

Changing flags on an existing mount. This project remounts the rootfs bind as
read-only.

### Recursive mount operation

An operation applied to a mount and the mounts below it. This is represented by
`MS_REC`.

### `MS_PRIVATE`

Changes a mount so future mount and unmount events do not propagate to peer
mounts.

### `MS_REC`

Applies the mount operation recursively through the subtree.

### `MS_BIND`

Creates or operates on a bind mount.

### `MS_REMOUNT`

Changes flags on an existing mount instead of creating another mount.

### `MS_RDONLY`

Marks a mount read-only.

### `MNT_DETACH`

Requests a lazy unmount. The mount disappears from the visible tree now and is
fully released after remaining references disappear.

### `mount()`

Creates a mount or changes mount properties. This project uses it for private
propagation, rootfs binding, read-only remount, tmpfs, procfs, and devices.

### `umount2()`

Unmounts with optional flags. The project uses `MNT_DETACH` to detach the old
host root after pivoting.

### `pivot_root`

The kernel operation that changes the process's mount-tree root and relocates
the old root.

### `SYS_pivot_root`

The syscall identifier passed to `syscall()` because the code does not use a
normal glibc wrapper.

### `pivot_root(".", ".")`

After changing into rootfs, makes that current directory the new root and
stacks the old root at the current location until it is detached.

### `chdir()`

Changes the process's current working directory. The setup first enters rootfs
and later enters the new `/`.

### Current working directory, or CWD

The directory used as the starting point for relative paths. After detaching
the old root, the code changes to the new slash.

### `tmpfs`

A temporary memory-backed filesystem. The project mounts it at `/tmp` and
`/dev` inside the container.

### `/tmp`

The writable temporary directory placed over the read-only rootfs.

### `/dev`

The device-file directory. This runtime covers it with an empty tmpfs and adds
only `/dev/null` and `/dev/zero`.

### `/dev/null`

A device that accepts and discards bytes. Programs commonly use it as an output
sink.

### `/dev/zero`

A device that supplies zero bytes when read.

### Device node

A special filesystem object connecting file operations to a kernel device.

### Placeholder file

The ordinary empty file created under the new `/dev` before a real host device
is bind-mounted on top of it.

### `mknod()`

The operation normally used to create device nodes. This runtime does not use
it; it bind-mounts two existing host device nodes.

### `procfs`

The virtual proc filesystem. A fresh mount inside the PID namespace shows that
namespace's process view.

### `/proc`

The usual mount point for procfs. Tools such as `ps` obtain process information
from it.

### Read-only rootfs

The rootfs bind after the `MS_REMOUNT | MS_RDONLY` operation. `/tmp` and `/dev`
remain writable because they are separate mounts placed on top.

## File-descriptor and process vocabulary

### File descriptor

A small integer identifying an open kernel object such as a file, pipe, or
socket within a process.

### `pipe()`

Creates two file descriptors: read end at index zero and write end at index
one.

### EOF

End-of-file. A pipe read returns zero when no writers remain and all existing
bytes have been consumed. Init treats this as the parent not releasing it.

### `read()`

Reads the parent's one-byte release signal from the synchronization pipe.

### `write()`

Writes the release byte or writes text through the provided file helper.

### `open()`

Opens a path and returns a file descriptor. It is used for device placeholders
and kernel control files.

### `close()`

Releases one process's reference to a file descriptor.

### `O_CREAT`

Makes `open()` create the destination file if it does not already exist.

### `O_WRONLY`

Opens a file for writing only.

### `O_CLOEXEC`

Marks a descriptor to close automatically when a successful exec replaces the
process program.

### `0666`

Requested placeholder-file permissions: read and write for owner, group, and
others, still modified by the process umask.

### `0755`

Cgroup-directory permission bits used by `mkdir()`: owner may write; everyone
may read and traverse, subject to filesystem rules.

### Parent process

The original runtime process executing `container_run()` on the host side.

### Child process

Context-dependent. The clone child is container init; the fork child inside
init is the requested command process.

### `clone()`

Creates container init, starts it at `child_entry()`, applies the namespace
flags, and uses the supplied stack.

### Clone stack

The separately allocated stack passed to `clone()`. The pointer passed is
`stack + CONTAINER_STACK_SIZE`, the high end used by a downward-growing stack.

### `CONTAINER_STACK_SIZE`

Provided stack-size constant equal to one megabyte.

### `fork()`

Creates a new process initially continuing the same program. Container init
uses it to create the command child.

### `execvp()`

Replaces the calling process with the requested executable and argument array.
It does not create another process and returns only on failure.

### `_exit()`

Terminates immediately without normal stdio flushing. The command child uses
`_exit(127)` after a failed exec.

### PID

Process identifier. A process can have different PID values when viewed from
different PID namespaces.

### PID 1

The first process in a PID namespace. Here it is container init, which launches
and collects container children.

### PID 2

The requested command is normally PID 2 inside the container because init is
PID 1. Exact later PIDs depend on process creation.

### Orphan process

A process whose parent exited. PID 1 in the namespace adopts it.

### Zombie process

An exited process whose completion status has not yet been collected by its
parent with a wait operation.

### Reap

Collect an exited child's status with `waitpid()`, removing its zombie record.

### `waitpid()`

Waits for and collects a child. Init uses PID minus one to collect any child;
the host parent waits for the specific cloned init PID.

### `waitpid(-1, ...)`

Waits for any child of the caller. Container init uses it to collect the main
command and adopted descendants.

### `WIFEXITED(status)`

True when the child exited normally.

### `WEXITSTATUS(status)`

Extracts the normal exit code from an encoded wait status.

### `WIFSIGNALED(status)`

True when a signal ended the child.

### `WTERMSIG(status)`

Extracts the terminating signal number from an encoded wait status.

### Exit code 127

The conventional code used here when `execvp()` could not run the command.

### 128 plus signal number

The shell-style result produced when a signal ended the child.

## Capability vocabulary and constants

### Capability bounding set

The ceiling on capabilities this process and later executed programs may gain.
The runtime removes every capability from it.

### Effective capabilities

Capability bits the process can currently exercise.

### Permitted capabilities

Capability bits the process is allowed to place in its effective set.

### Inheritable capabilities

Capability bits that may participate in capability inheritance across exec.

### `CAP_LAST_CAP`

The highest capability number known by the build headers. The drop loop visits
zero through this value.

### `CAP_SETPCAP`

The capability associated with changing capability sets, including bounding-
set operations. It is relevant to why bounding-set removal occurs first.

### `CAP_NET_ADMIN`

The capability used for network-administration operations such as changing
interface addresses, flags, and routes.

### `CAP_SYS_ADMIN`

A broad capability covering many administrative operations. Setting no-new-
privileges permits seccomp filter installation without retaining it.

### `_LINUX_CAPABILITY_VERSION_3`

The capability API version used by `SYS_capset`. It uses two 32-bit data
records.

### `struct __user_cap_header_struct`

Capability request header containing the API version and target PID.

### `struct __user_cap_data_struct`

Capability masks for effective, permitted, and inheritable sets. Two records
cover capability numbers zero through 63.

### `PR_CAPBSET_DROP`

The `prctl()` operation that removes one capability number from the bounding
set.

### `PR_SET_NO_NEW_PRIVS`

The `prctl()` operation that permanently enables the no-new-privileges flag.

### No new privileges

A process setting that stops later exec from increasing privilege through
set-user-ID bits or file capabilities. It is also a prerequisite for this
unprivileged seccomp installation.

### `prctl()`

Linux process-control system call wrapper. This project uses it for bounding-
set removal and no-new-privileges.

### `SYS_capset`

The syscall identifier used to replace the current process's capability sets
with the two zeroed data records.

## Networking vocabulary, structures, and flags

### Network namespace

The `CLONE_NEWNET` view containing separate interfaces, addresses, routes, and
loopback state.

### Loopback, `lo`, and localhost

The private interface used for communication within one network stack.
`container_network()` adds active flags to `lo`.

### `ceth0`

The default name of the veth endpoint inside the container.

### veth pair

Two linked virtual Ethernet interfaces. Packets entering one endpoint emerge
from the other.

### `cvbr0`

The host bridge name stored in `CONTAINER_BRIDGE`. Provided `net.c` creates or
reuses it.

### Bridge

A virtual network switch. The host-side veth endpoint attaches to `cvbr0`.

### Gateway

The next-hop address used for destinations outside the directly connected
subnet. Here it is normally `10.44.0.1`.

### Default route

A route with destination zero and mask zero. It matches destinations not
handled by a more specific route.

### Netmask

A 32-bit value separating the network prefix from host-address bits.

### Prefix length

The number of leading one bits in a netmask. Prefix 24 corresponds to
255.255.255.0.

### Network byte order

The byte order used in network structures. `htonl()` converts a 32-bit host
integer into this order.

### `AF_INET`

The IPv4 address family.

### `SOCK_DGRAM`

Datagram socket type. Here it creates a socket suitable as a handle for network
interface ioctls.

### `socket()`

Creates the control socket used for interface and routing operations.

### `ioctl()`

Performs device- or interface-specific control operations on a file descriptor.

### `struct ifreq`

Interface request structure. `ifr_name` selects the interface; union fields
carry flags, an address, or a netmask.

### `ifr_name`

The interface-name field in `struct ifreq`, set to `lo` or `c->net_ifname`.

### `ifr_flags`

The interface flag field read and written by the flag ioctls.

### `ifr_addr`

The interface address field used with `SIOCSIFADDR`.

### `ifr_netmask`

The interface netmask field used with `SIOCSIFNETMASK`.

### `IFNAMSIZ`

The fixed size of a Linux interface-name array, including its terminating null
byte.

### `struct sockaddr`

Generic socket-address storage accepted by interfaces supporting multiple
address families.

### `struct sockaddr_in`

IPv4-specific socket-address structure containing `sin_family` and `sin_addr`.

### `sin_family`

Field identifying the address family. This project sets it to `AF_INET`.

### `sin_addr`

Field holding the binary IPv4 address.

### `inet_pton()`

Converts a presentation string such as `10.44.0.2` into binary network form.
Exactly one means success.

### `htonl()`

Converts a 32-bit value from host byte order to network byte order.

### `SIOCGIFFLAGS`

Reads an interface's current flags into `ifr_flags`.

### `SIOCSIFFLAGS`

Writes the updated `ifr_flags` to an interface.

### `IFF_UP`

Interface flag marking the interface administratively enabled.

### `IFF_RUNNING`

Interface flag marking the interface operational/running.

### `SIOCSIFADDR`

Assigns the IPv4 address stored in `ifr_addr` to the selected interface.

### `SIOCSIFNETMASK`

Assigns the netmask stored in `ifr_netmask` to the selected interface.

### `struct rtentry`

Routing-table entry structure containing destination, mask, gateway, and flags.

### `rt_dst`

The route destination address. Zero is used for the default route.

### `rt_genmask`

The route mask. Zero combines with destination zero to match every address.

### `rt_gateway`

The next-hop gateway address.

### `rt_flags`

Route behavior flags. This project sets `RTF_UP | RTF_GATEWAY`.

### `RTF_UP`

Marks a route active.

### `RTF_GATEWAY`

Marks a route as using a gateway rather than direct delivery.

### `SIOCADDRT`

Adds the prepared `struct rtentry` to the network namespace's routing table.

### `CONTAINER_BRIDGE`

Provided constant containing the host bridge name `cvbr0`.

### `CONTAINER_BRIDGE_GW`

Provided constant containing the bridge and default-gateway address
`10.44.0.1`.

### CIDR

Address-plus-prefix notation such as `10.44.0.1/24`.

### iproute2 and `ip`

Linux networking command suite used by provided `net.c` to create the bridge
and veth pair.

## Seccomp and BPF vocabulary

### System call, or syscall

A request from a user-space program to the kernel, such as opening a file,
mounting a filesystem, or creating a process.

### `struct seccomp_data`

The kernel-provided record inspected by a seccomp filter. This project reads
its `arch` and `nr` fields.

### `arch`

The audit architecture value in `struct seccomp_data`.

### `nr`

The current syscall number in `struct seccomp_data`.

### Audit architecture

The architecture identifier supplied with each filtered syscall. The filter
checks it before interpreting syscall numbers.

### `AUDIT_ARCH_X86_64`

Audit architecture identifier selected for an x86-64 build.

### `AUDIT_ARCH_AARCH64`

Audit architecture identifier selected for an ARM64 build.

### `OUR_AUDIT_ARCH`

Project macro selecting one of the two supported audit architecture constants.

### ABI

Application binary interface. It includes rules such as syscall numbering and
calling conventions. Different ABIs can assign different meanings to numbers.

### BPF

Berkeley Packet Filter instruction format. Seccomp uses classic BPF as a small
decision program even though this filter is not filtering packets.

### `struct sock_filter`

One classic-BPF instruction.

### `struct sock_fprog`

The filter program descriptor containing instruction count in `len` and array
pointer in `filter`.

### `BPF_STMT`

Macro producing one non-jump BPF instruction initializer.

### `BPF_JUMP`

Macro producing one BPF jump instruction initializer.

### `BPF_LD`

Load-operation class used to load `arch` or `nr` from seccomp data.

### `BPF_W`

Specifies a 32-bit word-sized load.

### `BPF_ABS`

Specifies loading from an absolute byte offset in the input record.

### `BPF_JMP`

Jump-operation class.

### `BPF_JEQ`

Equality comparison used for architecture and syscall-number checks.

### `BPF_K`

Specifies that the comparison or return uses an immediate constant.

### `BPF_RET`

Return-operation class producing the seccomp decision.

### `offsetof()`

Computes a structure field's byte offset. The filter uses it for `arch` and
`nr` in `struct seccomp_data`.

### `SECCOMP_RET_KILL_PROCESS`

Seccomp result that terminates the process. Used for architecture mismatch.

### `SECCOMP_RET_ERRNO`

Seccomp result that rejects the syscall and returns an errno value.

### `SECCOMP_RET_DATA`

Mask identifying the data bits where the filter places `EPERM`.

### `SECCOMP_RET_ALLOW`

Seccomp result that permits the syscall to execute.

### `SECCOMP_SET_MODE_FILTER`

Operation passed to the seccomp syscall to install a BPF filter program.

### `SYS_seccomp`

The syscall identifier used to install the packaged seccomp program.

### Denylist

A policy that blocks listed operations and allows operations not listed. This
project's seccomp policy is a denylist.

### `NUM_DENIED_SYSCALLS`

Compile-time array-length calculation used to size the filter and loop over the
complete denylist.

## Every denied syscall

### `__NR_ptrace`

Syscall number for tracing or controlling another process.

### `__NR_mount`

Syscall number for creating or changing a filesystem mount.

### `__NR_umount2`

Syscall number for unmounting with flags.

### `__NR_pivot_root`

Syscall number for changing the process's mount-tree root.

### `__NR_chroot`

Syscall number for changing the process's root directory view.

### `__NR_setns`

Syscall number for joining an existing namespace.

### `__NR_unshare`

Syscall number for creating separate execution-context resources, including
namespaces.

### `__NR_reboot`

Syscall number for reboot and related system-wide machine actions.

### `__NR_swapon`

Syscall number for enabling a swap area.

### `__NR_swapoff`

Syscall number for disabling a swap area.

### `__NR_kexec_load`

Syscall number for loading a replacement kernel image.

### `__NR_init_module`

Syscall number for loading a kernel module from memory.

### `__NR_finit_module`

Syscall number for loading a kernel module from a file descriptor.

### `__NR_delete_module`

Syscall number for unloading a kernel module.

## Error values and return conventions

### `errno`

Thread-local integer set by a failing system or library call to identify the
error.

### `strerror(errno)`

Converts the current errno value to a readable error message.

### `EPERM`

Operation not permitted. The seccomp filter returns this for denied syscalls.

### `EINVAL`

Invalid argument. During bounding-set removal, it is ignored for capability
numbers the running kernel does not recognize.

### `EEXIST`

Already exists. Cgroup creation accepts this and reuses the directory.

### `ENOENT`

No such file or directory. Cgroup cleanup accepts it because the requested
directory is already absent.

### `EINTR`

Interrupted system call. Both wait loops retry when a signal interrupts
`waitpid()`.

### Return zero

Usually success for setup/helper functions. For an executed command, zero is
also the conventional successful program result.

### Return minus one

Failure convention for most setup/helper functions.

### Return one

Generic runtime failure used when setup or waiting fails.

### Short read

A read returning fewer bytes than requested. Init expects exactly one release
byte, so any other result is failure.

### Short write

A write returning fewer bytes than requested. The release operation and
provided `write_file()` treat this as failure.

### Best-effort

An attempted feature whose failure is reported but does not abort the core
container. Loopback and optional network setup are treated this way.

## Required and helper functions

### `container_namespaces()`

Returns the five namespace flags passed to `clone()`.

### `container_write_idmaps()`

Writes the child's UID map, setgroups denial, and GID map from the host parent.

### `write_cgroup_limit()`

Student-added helper converting one numeric cgroup limit to text and writing
its control file.

### `container_cgroup_init()`

Enables controllers, creates the cgroup, stores its path, and writes limits.

### `container_cgroup_enter()`

Writes init's host PID to the container cgroup's `cgroup.procs` file.

### `bind_device()`

Student-added helper creating a destination and bind-mounting `/dev/null` or
`/dev/zero` there.

### `setup_filesystem()`

Student-added helper creating private mounts and changing the visible root to
rootfs.

### `drop_capabilities()`

Student-added helper clearing the bounding, effective, permitted, and
inheritable capability state and setting no-new-privileges.

### `container_setup()`

Runs child-side setup in order: hostname, networking, filesystem, capabilities,
and seccomp.

### `container_network()`

Reads loopback flags, adds `IFF_UP | IFF_RUNNING`, and writes them back.

### `make_ipv4_address()`

Student-added helper converting IPv4 text to a `sockaddr_in` representation.

### `container_net_config()`

Configures ceth0's address, netmask, active flags, and default route.

### `container_net_host_setup()`

Provided host-side function creating the bridge and veth, then moving the
container endpoint into the child's network namespace.

### `container_net_host_teardown()`

Provided host-side function attempting to remove the host veth after init exits.

### `container_seccomp()`

Builds and installs the architecture-aware syscall denylist.

### `status_to_exit_code()`

Student-added helper converting encoded wait status to a normal or shell-style
exit code.

### `container_init()`

Runs as container PID 1. It waits for release, runs setup, forks and executes
the command, collects children, and returns the command result.

### `child_entry()`

Student-added adapter converting clone's `void *` argument and calling
`container_init()`.

### `container_run()`

The only container function called by `main()`. It coordinates the complete
host-side lifecycle.

### `container_cleanup()`

Removes the container cgroup directory after init is collected.

### `write_file()`

Provided helper opening a kernel control file, writing the complete supplied
text without adding a newline, closing it, and reporting errors.

## Other functions and C/Linux terms visible in the implementation

### `_GNU_SOURCE`

Feature-test macro enabling GNU/Linux declarations such as `clone()` before
system headers are included.

### `CONTAINER_H`

The include-guard macro in `container.h`. It stops the header's declarations
from being processed more than once in one source file.

### `PATH_MAX`

Maximum path-buffer size constant used for rootfs and cgroup paths.

### `pid_t`

Integer type used for process IDs.

### `size_t`

Unsigned type used for object sizes, array indexes, and byte counts.

### `ssize_t`

Signed byte-count type returned by read/write operations, allowing minus one
for failure.

### `uint32_t`

Unsigned integer type containing exactly 32 bits, matching an IPv4 netmask.

### `NULL`

Null pointer constant. Several mount arguments are unused for the selected
operation and are passed as null.

### `snprintf()`

Builds bounded text in a fixed-size buffer. Used for proc paths, cgroup paths,
device paths, PIDs, mappings, and limits.

### `memset()`

Fills memory with one byte value. The code uses zero to initialize capability
records, network requests, addresses, and route entries.

### `strncpy()`

Copies an interface name into the fixed-size `ifr_name` array with room left
for the terminating null byte.

### `strlen()`

Returns a string's length. Used for hostname length and provided file writes.

### `malloc()`

Allocates the clone stack.

### `free()`

Releases the clone stack after init has exited or clone fails.

### `sethostname()`

Writes `c->hostname` into the current UTS namespace.

### `syscall()`

Calls a kernel operation by syscall number. Used for pivot root, capset, and
seccomp.

### `goto fail`

Transfers control to the shared network-error cleanup label, which closes the
socket and returns failure.

### Bitwise OR, `|`

Combines independent flag bits. Used for namespace flags, mount flags,
interface flags, route flags, BPF operations, and seccomp results.

### Bitwise OR assignment, `|=`

Adds flag bits to an existing value while preserving bits already present.

### Left shift, `<<`

Moves bits left. Network setup uses it to create a prefix-length netmask from
32 one bits.

### `0xFFFFFFFFu`

An unsigned 32-bit value containing all one bits, used as the starting netmask.

### Compound literal cast to `struct sock_filter`

Turns the BPF macro initializer into a value that can be assigned into the
filter array one instruction at a time.

### `stderr`

Standard error output stream used for runtime diagnostics.

### `container: ` diagnostic prefix

Prefix used by implementation diagnostics. The test harness recognizes and
skips these lines when examining command output.

### `main()`

Provided program entry point. It parses options, points `c->argv` at the
requested command, and returns `container_run(&c)`.

### `argc`

Number of command-line strings passed to provided `main()`.

### `argv`

Array of command-line strings passed to `main()`. After option parsing, a
pointer into this array becomes `c->argv`.

### `parse_bytes()`

Provided `main.c` helper converting text limits into a `long`. It recognizes
`max` and K, M, or G suffixes.

### `run_ip()`

Private provided helper in `net.c`. It forks, executes the `ip` command with
the supplied arguments, waits for it, and returns its result.

### `IP(...)`

Provided variadic macro that adds the command name `ip` and a final null pointer
before passing an argument list to `run_ip()`.

### `__VA_ARGS__`

The variable argument list received by the provided `IP(...)` macro.

### `defined`

Preprocessor test used to select the audit architecture macro for x86-64 or
ARM64 during compilation.

### `dup2()`

Used by provided `net.c` to replace standard error descriptor two with an open
`/dev/null` descriptor before executing the `ip` command.

### `fprintf()`

Writes formatted diagnostics to `stderr`.

### `strcmp()`

Compares command-line option strings in provided `main.c`.

### `strtol()`

Converts a text limit to a `long` in provided `parse_bytes()`.

## Command-line options

### `--`

Ends runtime option parsing. Everything after it becomes the command and
arguments stored in `c->argv`.

### `--name`

Sets `c->name`, used as the container and cgroup name.

### `--hostname`

Sets `c->hostname`.

### `--rootfs`

Sets the directory that becomes container `/`.

### `--pids-max`

Sets the process-count limit. The word `max` becomes negative one.

### `--mem-max`

Sets the memory limit in bytes. Provided parsing also accepts K, M, and G
suffixes and the word `max`.

### `--cgroup-base`

Sets the parent cgroup directory.

### `--net`

Turns optional bridge/veth networking on.

### `--net-ip`

Overrides the default container IPv4 address.

## Fast comparison entries

### Namespace versus cgroup

Namespace changes what the process sees. Cgroup limits what the process tree
can consume.

### Capability versus seccomp

Capabilities control privileged authority. Seccomp controls whether selected
system calls may run at all.

### Clone versus fork versus exec

Clone creates namespaced init. Fork creates the command child. Exec replaces
that child with the requested program.

### Host PID versus container PID

The host PID identifies init in host proc, cgroup, network, and wait operations.
The same process sees PID 1 inside its PID namespace.

### Loopback versus veth

Loopback carries localhost traffic within the container. The veth pair carries
traffic between the container network namespace and host bridge.

### UID zero versus GID zero

UID zero is the container root user identity. GID zero is the container root
group identity used in group ownership and permission checks. Each maps to the
launcher's corresponding ordinary host ID.

### Inner wait versus outer wait

Container init waits for command-side children. The host parent waits for
container init.

### Provided versus implemented networking

Provided `net.c` builds and removes the host bridge/veth side. Implemented
`container_net_config()` configures the endpoint inside the container.

# Likely questions and answers

Grouped by topic. The "what if" questions at the end of each section are the style professors like, because they test whether you understand *why* the code is ordered the way it is.

---

## Big picture

**Q: Is a container a virtual machine?**
No. It's ordinary Linux processes with a restricted view of the machine, and it shares the host kernel.

**Q: What are the four tools used to restrict the container, and what does each control?**
1. **Namespaces:** what the process can *see* (PIDs, files, hostname, network, users).
2. **Capabilities:** what root can *do*.
3. **Seccomp:** which syscalls it can *call*.
4. **Cgroups:** how much it can *use* (process count, memory).

**Q: Describe the process tree.**
The host's `main()` calls `container_run()` (the parent/runtime). That clones `container_init()`, which is PID 1 inside the container. Init forks the command, which is PID 2.

**Q: What does the parent do vs the child?**
The parent creates the cgroup and pipe, clones, writes ID maps, joins the child to the cgroup, optionally sets up host networking, releases the child, waits, and cleans up. The child waits for release, then sets up hostname, network, filesystem, capabilities, and seccomp, then forks and execs the command and reaps children.

**Q: Why does the container's "root" not need real root on the host?**
Because of the user namespace. Container UID 0 is mapped to your ordinary host user, so it has no real host power.

---

## Namespaces

**Q: Name the five namespaces used and what each isolates.**
- User: user and group IDs.
- PID: process numbering.
- Mount: the mount table.
- UTS: hostname.
- Network: interfaces and routes.

**Q: Why is the user namespace special?**
It lets an unprivileged user create the other namespaces and drop capabilities.

**Q: Which flag is passed to `clone()` alongside the namespace flags, and why?**
`SIGCHLD`, so the parent is notified when the child exits and `waitpid` works.

**Q: Why does the child need a mount namespace specifically?**
So the mounts made during setup don't appear on the host, and because `pivot_root` requires it.

**Q: Why doesn't `clone` get `CLONE_VM`?**
So the child gets a copy of memory, not shared memory.

**Q: Why does `clone` get a stack argument and `fork` doesn't? Why `stack + SIZE`?**
`clone` runs a function instead of continuing from the call site, so it needs its own stack. Stacks grow downward, so you pass the *top*.

---

## ID maps

**Q: What does the mapping "0 1000 1" mean?**
Container UID 0 maps to host UID 1000, for exactly one ID.

**Q: Why does the parent write the ID maps instead of the child?**
A new user namespace has no mapping, and the child can't set its own. The parent writes it using the child's PID as the **host** sees it.

**Q: In what order are the three files written?**
`uid_map`, then `setgroups` (write "deny"), then `gid_map`.

**Q: Why write "deny" to `setgroups`?**
Without it, a process could drop its supplementary groups and sidestep file permissions that block certain groups. The kernel refuses an unprivileged `gid_map` write until `setgroups` is denied.

**Q: What would happen if you wrote `gid_map` before denying `setgroups`?**
The write would fail, and `container_write_idmaps` would return -1, so setup would fail.

**Q: What would happen if the ID maps were never written?**
The child would have no valid user or group ID and could do almost nothing.

---

## Parent/child synchronization (the pipe)

**Q: What is the pipe for?**
The parent writes one byte to tell the child "ID maps and cgroup are done, continue." The child blocks reading it, so it can't run half-configured.

**Q: Why does the child close `sync[1]` first?**
It only reads. If it kept the write end open, `read()` would never see EOF when the parent fails and closes its end.

**Q: What happens if the parent fails setup?**
It doesn't write the byte. It closes the pipe, the child's read returns 0 (EOF), and the child exits instead of waiting forever.

**Q: Why does the parent always close `sync[1]`, even on success?**
On success the child already has its byte. On failure, closing is what delivers EOF to the child.

**Q: Why does the parent track `setup_ok` instead of returning early on failure?**
It still has to close the pipe and reap the child. Returning early would leak the pipe and leave a zombie or hung child.

**Q: Why is the cgroup created before `clone`?**
So the child is never running without limits.

---

## Cgroups

**Q: What is a cgroup, physically?**
A directory under `/sys/fs/cgroup`. Making the directory creates the cgroup, and writing to files inside sets limits.

**Q: What does writing "+pids +memory" to `cgroup.subtree_control` do?**
It enables the process-count and memory controllers for child cgroups. A child can only use a controller its parent has enabled.

**Q: Which three limit files are written, and what does each do?**
- `pids.max`: max process count, which stops fork bombs.
- `memory.max`: max memory, and exceeding it gets the process killed.
- `memory.swap.max` set to 0: stops over-limit memory from being pushed to swap, so the limit actually holds.

**Q: What does writing "max" mean, and when is it used?**
It means unlimited. It's written when a limit is -1.

**Q: Why is `EEXIST` ignored when making the cgroup directory?**
A crashed earlier run may have left it behind, and the next run must still work.

**Q: What does `container_cgroup_enter` write, and where?**
The child's **host** PID as text, into `<cg_path>/cgroup.procs`.

**Q: Why does one write cover the whole container?**
Every process init starts later inherits the cgroup.

**Q: Why is the host PID used and not 1?**
Because this runs in the parent, which sees host PIDs.

**Q: Why does `container_cleanup` use `rmdir` and not `unlink`?**
Cgroups are directories.

**Q: When does `rmdir` on a cgroup fail with `EBUSY`?**
When processes are still inside it. It only works on an empty cgroup.

**Q: Why are the mounts not cleaned up explicitly?**
They vanish on their own when the last process in the mount namespace exits.

---

## Filesystem setup (very likely heavily tested)

**Q: List the steps of `setup_filesystem` in order.**
1. Make mounts private (recursive).
2. Bind rootfs onto itself, then remount it read-only.
3. Mount a tmpfs on `/tmp`.
4. Mount a tmpfs on `/dev` and bind in null and zero.
5. Mount a fresh `/proc`.
6. `chdir` into rootfs, `pivot_root(".", ".")`, detach the old root, `chdir("/")`.

**Q: Why make mount propagation private first?**
Many systems mark `/` as "shared," so mounts in one place get copied to peers. Our mounts could leak back to the host even in a new mount namespace.

**Q: Why bind-mount the rootfs onto itself?**
`pivot_root` requires the new root to be a mount point, and an ordinary directory isn't one.

**Q: Why does making it read-only take a second call?**
The first bind mount ignores the read-only flag, so it has to be applied with a remount afterward.

**Q: Why can `/tmp` be writable if the root is read-only?**
A tmpfs is a new filesystem mounted on top. Read-only only applies to the mount underneath. It lives in memory and starts empty.

**Q: Why mount a fresh tmpfs on `/dev`?**
To hide the host's `/dev`, which contains sensitive devices. Only null and zero are added back.

**Q: Why bind-mount `/dev/null` and `/dev/zero` instead of creating them with `mknod`?**
A user namespace isn't allowed to create device files, so it borrows the host's real ones.

**Q: Why create an empty file before the bind mount?**
A bind mount needs an existing file to mount onto, and the tmpfs on `/dev` hides the ones `make-rootfs.sh` made. Without it, the mount fails with `ENOENT`.

**Q: Why close the fd right after creating that file?**
Only the file's existence was needed, not the open descriptor.

**Q: What does mounting `/proc` accomplish?**
The container sees only its own processes, because init is already inside the new PID namespace.

**Q: Why must `/proc` be mounted before `pivot_root`?**
The kernel only allows it while the host's `/proc` is still visible. Afterward it fails with `EPERM`.

**Q: Why `chdir(rootfs)` before `pivot_root`?**
So "." refers to the rootfs in the next call.

**Q: Explain `pivot_root(".", ".")`. Why use "." for both arguments?**
Normally the second argument is a directory *inside* the new root where the old root is parked. The rootfs is read-only, so that directory can't be created. Using "." for both stacks the old root on top of the new root at the same spot.

**Q: Why call it with `syscall()`?**
There's no glibc wrapper for it.

**Q: What does `umount2(".", MNT_DETACH)` do?**
It removes the host's old root so the container can't reach host files. `MNT_DETACH` is a lazy unmount, so it disappears immediately even though we're still "inside" it as the cwd.

**Q: Why `chdir("/")` at the end?**
The old working directory is stale after the swap.

**Q: What would happen if you skipped the detach step?**
The host's files would stay reachable under the old root, defeating the isolation.

---

## Capabilities

**Q: What is a capability?**
One piece of root's power. Container root holds them (inside its namespace) until they're dropped.

**Q: What are the three steps in `drop_capabilities`, in order?**
1. Drop every capability from the **bounding set**.
2. Clear the currently held capabilities with `capset`.
3. Set `PR_SET_NO_NEW_PRIVS`.

**Q: What is the bounding set?**
It limits what capabilities a process, and any program it starts, can ever regain.

**Q: Why must the bounding set be dropped before `capset`?**
Dropping from it needs root powers, and `capset` removes them.

**Q: Why is `EINVAL` ignored in the loop?**
It means the kernel doesn't know that capability number, so there's nothing to drop.

**Q: Why does `capset` take an array of two data structures?**
Version 3 stores capabilities in two 32-bit chunks, because there are more than 32.

**Q: Why use version 3? What about version 1?**
Version 3 is the current one. Version 1 only covers capabilities 0 to 31.

**Q: What does a zeroed data array mean?**
Effective, permitted, and inheritable sets are all empty, so the process holds no capabilities.

**Q: What does `header.pid = 0` mean?**
Apply the change to the calling process.

**Q: What does `PR_SET_NO_NEW_PRIVS` do? Can it be undone?**
Running a setuid program, or one with file capabilities, can no longer raise privileges. Once set, it can never be turned off.

**Q: Why is there no glibc function for `capset`?**
There isn't a wrapper, so it's called via `syscall(SYS_capset, ...)`.

**Q: True or false: capset has a version 2 that doesn't exist.**
False. Version 2 existed but is deprecated. Version 3 is current. (Your study copy has an incorrect comment about this.)

---

## Seccomp

**Q: What does seccomp do, and how does it differ from capabilities?**
It blocks chosen syscalls entirely, for any user. Capabilities only stop *root* from doing privileged things.

**Q: Is this filter an allowlist or a denylist?**
A denylist. The listed syscalls fail and everything else is allowed.

**Q: Name some of the blocked syscalls.**
ptrace, mount, umount2, pivot_root, chroot, setns, unshare, reboot, swapon, swapoff, kexec_load, init_module, finit_module, delete_module.

**Q: What happens when the command calls a blocked syscall?**
It's never executed. The call returns -1 with `errno = EPERM`, and the process is not killed.

**Q: What happens on the wrong architecture?**
`SECCOMP_RET_KILL_PROCESS`, so the whole process is killed.

**Q: Why check the architecture first?**
Other CPU modes (such as 32-bit) use different syscall numbers and could bypass the list. The number's meaning also depends on the architecture.

**Q: What are the filter's steps?**
Load arch, kill if wrong, load syscall number, compare against each denied one and return `EPERM` on a match, otherwise allow.

**Q: Why is the array size `4 + 2 * N + 1`?**
3 for the architecture check, 1 to load the syscall number, 2 per denied syscall, and 1 for the final allow. (The constant 4 includes the load-nr.)

**Q: What do the jump arguments `(0, 1)` mean in the deny loop?**
If the number matches, fall through to the next instruction (the `EPERM` return). If not, skip one instruction (that return) and test the next syscall.

**Q: Why is `PR_SET_NO_NEW_PRIVS` required before installing the filter?**
An unprivileged process can't install a filter without it. Capabilities are already gone by then.

**Q: Why is it set twice?**
`drop_capabilities` sets it, and seccomp sets it again deliberately so it works on its own.

**Q: Does the command inherit the filter? Can it remove it?**
It's inherited across `fork` and `exec` and can't be removed.

**Q: Why must seccomp be installed last in `container_setup`?**
It blocks `mount`, `umount2`, and `pivot_root`, which `setup_filesystem` uses. Installing it earlier would break the container's own setup.

**Q: Why is `unshare` blocked?**
It could create a nested user namespace to regain a full capability set.

**Q: What's the weakness of a denylist?**
Any dangerous syscall you forget to list is allowed. An allowlist is safer but much more work.

---

## `container_setup` ordering (almost guaranteed)

**Q: Give the order of `container_setup` and the reason for each position.**
1. Hostname.
2. Loopback: needs root powers, so before dropping capabilities.
3. `--net` config (if enabled): same reason.
4. Filesystem: needs mount powers, so before dropping capabilities.
5. Drop capabilities: only after everything that needed them.
6. Seccomp: last, because it blocks syscalls that setup uses.

**Q: Why are the results of `container_network` and `container_net_config` ignored?**
The spec says the container should still run without loopback.

**Q: What would break if you dropped capabilities before setting up the filesystem?**
The mount calls would fail, since they need root powers.

**Q: What would break if you installed seccomp before the filesystem setup?**
`mount`, `umount2`, and `pivot_root` would return `EPERM`, so the container would fail to set up.

---

## Networking

**Q: Why does `container_network` open a socket if it never sends data?**
It's only a handle for talking to the kernel via `ioctl`.

**Q: What are the three steps to bring up loopback?**
Read the flags (`SIOCGIFFLAGS`), add `IFF_UP | IFF_RUNNING`, write them back (`SIOCSIFFLAGS`).

**Q: Why use `|=` on the flags?**
To turn on those two flags without changing the others.

**Q: Why `IFNAMSIZ - 1` in the name copy?**
`IFNAMSIZ` includes the null terminator, so at most that minus one characters are copied.

**Q: What does a new network namespace start with?**
Only a switched-off loopback.

**Q: What are the four steps in `container_net_config`?**
Set the address, set the netmask, switch the interface on, and add a default route.

**Q: How is a prefix like /24 converted to a netmask?**
Start with 32 one-bits and shift left by `32 - prefix`, so the last bits become zeros. Then `htonl` converts to network byte order.

**Q: Why the `prefix > 0` check?**
Shifting by 32 is undefined in C. Prefix 0 means mask 0.0.0.0.

**Q: What does the default route say?**
Destination 0.0.0.0 with mask 0.0.0.0 matches every address, so anything not on the local network goes through the gateway (the host).

**Q: What happens without the default route?**
The container could only reply to 10.44.0.x addresses.

**Q: What do `RTF_UP` and `RTF_GATEWAY` mean?**
The route is active, and traffic goes via a gateway rather than directly.

**Q: Which networking functions are provided, and where do you call them?**
`container_net_host_setup` (after `cgroup_enter`, before releasing the child) and `container_net_host_teardown` (after `waitpid`), both only if `--net`.

**Q: Why use `goto fail` in `container_net_config`?**
So every error path closes the socket in one place.

---

## Init and reaping

**Q: Why does init fork the command instead of exec'ing it directly?**
PID 1 must stay alive. When a process's parent dies, the kernel hands it to PID 1, and PID 1 must collect it. If init became the command, nobody would reap orphans.

**Q: What is a zombie?**
A process that has exited but whose parent hasn't collected its exit status with `waitpid`.

**Q: Why does init use `waitpid(-1, ...)`?**
-1 means any child, so it reaps orphans as well as the main command.

**Q: When does the loop stop?**
When the reaped PID equals the command's PID.

**Q: What does `EINTR` mean and how is it handled?**
A signal interrupted the wait, which isn't a child failure, so it retries.

**Q: Why `_exit(127)` and not `exit` after a failed exec?**
`exit` would flush copied stdio buffers and could print duplicate output. 127 means "command could not be run."

**Q: How is the exit code computed?**
`WEXITSTATUS` if it exited normally, `128 + WTERMSIG` if killed by a signal.

**Q: What does the container's exit status end up being?**
The command's exit status, returned up through `container_run`. If setup failed, it returns 1.

**Q: Why shouldn't init close file descriptors it didn't open?**
The tests give the command an open descriptor to keep the container running while they check it.

**Q: Which `waitpid` in `container_run` reaps what?**
The parent's `waitpid(child)` reaps init only. Init's `waitpid(-1)` reaps everything inside the container.

**Q: Why can the stack be freed only after `waitpid`?**
The child runs on it until it exits.

---

## Trace and "what if" questions

**Q: A process in the container calls `mount()`. Walk through what happens.**
The kernel runs the BPF filter. The arch matches, the number is loaded, it's compared down the denylist, and the `mount` entry matches. The filter returns `EPERM`, the syscall never executes, and the process sees -1 with `errno = EPERM`.

**Q: Inside the container, a process calls `getpid()`. What does it return?**
Its PID in the container's namespace, such as 2 for the command. Init is 1.

**Q: What would `ps` or `/proc` show without the fresh `/proc` mount?**
The host's processes, since `/proc` would still be the host's.

**Q: A process in the container tries to fork a thousand children. What stops it?**
`pids.max` in the cgroup.

**Q: A process allocates more memory than `memory.max`. What happens?**
It's killed. With swap set to 0, it can't be pushed to swap instead.

**Q: The container tries to write to `/etc`. What happens?**
It fails, because the root is a read-only mount. Only `/tmp` (tmpfs) is writable.

**Q: A container process tries to change the hostname. What happens to the host's?**
It's unaffected, because of the UTS namespace. (Note it has no `CAP_SYS_ADMIN` anymore anyway.)

**Q: What happens if the parent crashes after clone but before writing the byte?**
The pipe's write end closes, the child sees EOF, prints "parent did not release the container," and exits.

**Q: Does the container run as real root on the host?**
No. Container UID 0 maps to your normal user.

---

## Quick-fire true/false

- Containers share the host kernel. **True.**
- Namespaces control how much a process can use. **False.** That's cgroups. Namespaces control what it can see.
- `memory.swap.max` is set to 0 so over-limit processes get killed rather than swapped. **True.**
- Seccomp is installed before the filesystem is set up. **False.** It's last.
- Blocked syscalls kill the process. **False.** They return `EPERM`. Only the wrong architecture kills.
- `PR_SET_NO_NEW_PRIVS` can be turned off after it's set. **False.**
- The first bind mount of rootfs applies the read-only flag. **False.** A remount is needed.
- `/proc` can be mounted after `pivot_root`. **False.** It fails with `EPERM`.
- The parent writes the child's ID maps using PID 1. **False.** It uses the host's PID for the child.
- Cgroup cleanup uses `unlink`. **False.** It uses `rmdir`.

