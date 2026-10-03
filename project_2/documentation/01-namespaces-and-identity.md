# Namespaces and Identity

## What problem these functions solve

A container process is still an ordinary Linux process running on the **host kernel**. Creating the process alone does not automatically give it an isolated view of the system.

Without namespaces, the child would normally share the host's:

* process ID view
* mount table
* hostname
* network stack
* user/group ID interpretation

Namespaces allow the child to receive separate views of these resources.

The important distinction is:

```text
Namespaces -> isolate what the process sees
Cgroups    -> limit how much resources the process can use
```

The namespace function does not create five unrelated children. It creates **one child process** with several independent namespace options enabled.

The user namespace also introduces an identity problem.

Inside a new user namespace, the child needs a mapping between:

```text
ID inside the namespace
        ↕
ID on the host
```

The runtime establishes a mapping where namespace UID 0 corresponds to the ordinary UID of the user running the container runtime. This allows the child to act as UID 0 **inside its user namespace** without becoming host UID 0.

The same idea is applied to GIDs.

Finally, `clone()` is the Linux process-creation system call used by this runtime. It is similar to `fork()` because it creates a child, but it also accepts flags controlling namespaces and takes a function where the child begins execution.

---

# `container_namespaces()`

**Assignment status:** One of the 11 functions explicitly required by the specification.

**Purpose:** Return the collection of namespace flags that should be passed to `clone()`.

**Used by:** `container_run()` passes the returned value to `clone()` and separately ORs in `SIGCHLD`.

```c
int container_namespaces(void)
{
    return CLONE_NEWUSER
         | CLONE_NEWPID
         | CLONE_NEWNS
         | CLONE_NEWUTS
         | CLONE_NEWNET;
}
```

## Function declaration

```c
int container_namespaces(void)
```

This function:

* takes no arguments
* returns an `int`

The returned integer is not being used as an ordinary numerical value such as `5` or `20`.

Instead, it is being used as a **bitmask**.

A bitmask is an integer where individual bits represent independent options.

The namespace constants each represent a different option that can be passed to `clone()`.

---

## Starting the return expression

```c
return CLONE_NEWUSER
```

The first flag is:

```text
CLONE_NEWUSER
```

This requests a new **user namespace**.

A user namespace gives the child its own interpretation of user and group IDs.

This is important because the container can later have a process that is UID 0 inside the namespace without that process being UID 0 on the host.

However, the namespace initially needs its UID/GID mappings configured. That is the job of `container_write_idmaps()` later.

---

## Adding the PID namespace

```c
| CLONE_NEWPID
```

The `|` is the **bitwise OR operator**.

Here it means:

> Keep the bits already present in the current flag value and turn on the bits represented by this new flag.

`CLONE_NEWPID` requests a new **PID namespace**.

A PID namespace gives the child a separate view of process IDs.

The first process created in the new PID namespace becomes PID 1 from the namespace's perspective.

The important consequence is that processes inside the container do not get the same process view as ordinary host processes.

The host still knows about the process. The isolation is about the process's **namespace view**, not about making the process disappear from the kernel.

---

## Adding the mount namespace

```c
| CLONE_NEWNS
```

`CLONE_NEWNS` requests a new **mount namespace**.

Despite the name `NEWNS`, this means a separate mount namespace.

A mount namespace gives the child its own view of the system's mounted filesystems.

This matters because the container can configure its filesystem view without simply sharing the host's mount table.

---

## Adding the UTS namespace

```c
| CLONE_NEWUTS
```

This requests a new **UTS namespace**.

The UTS namespace primarily isolates the hostname.

Therefore, a hostname configured inside the container can differ from the host's hostname.

---

## Adding the network namespace

```c
| CLONE_NEWNET
```

This requests a new **network namespace**.

The network namespace gives the child a separate network environment, including its own network interfaces and routing information.

So the child does not simply operate directly inside the host's network namespace.

---

# Why the `|` operators do not overwrite each other

This is one of the most important things to understand about this function.

You might initially think:

> If each flag is an integer, won't the next `|` overwrite the previous flag?

No.

The flags are designed as **bit flags**. Each option uses particular bits in the integer.

For a simplified example, imagine:

```text
CLONE_NEWUSER = 00001
CLONE_NEWPID  = 00010
CLONE_NEWNS   = 00100
CLONE_NEWUTS  = 01000
CLONE_NEWNET  = 10000
```

These are not the actual Linux values. They are just a simplified representation.

Now OR them together:

```text
  00001
| 00010
---------
  00011
```

The first bit remains set and the second bit becomes set.

Add another:

```text
  00011
| 00100
---------
  00111
```

Nothing was overwritten. The existing bits remain set while another bit is added.

Eventually:

```text
  00001
| 00010
| 00100
| 01000
| 10000
---------
  11111
```

The resulting integer represents **all five independent options**.

Therefore this:

```c
return CLONE_NEWUSER
     | CLONE_NEWPID
     | CLONE_NEWNS
     | CLONE_NEWUTS
     | CLONE_NEWNET;
```

means:

> Return one integer whose bits indicate that all five namespace options are requested.

It does **not** mean:

> Choose one of the five namespaces.

And it does not mean that later flags replace earlier ones.

---

## The semicolon

```c
;
```

terminates the `return` statement.

The entire expression before it is evaluated as one bitmask.

---

# What `container_namespaces()` actually does

It is useful to separate this function from what happens later.

This function:

```c
container_namespaces()
```

does **not** create any namespaces itself.

It simply constructs and returns the bitmask.

The actual namespace creation happens when the caller passes those flags to `clone()`.

Conceptually:

```text
container_namespaces()
        |
        | returns bitmask
        v
container_run()
        |
        | passes bitmask to clone()
        v
clone()
        |
        | creates child with requested namespaces
        v
child
```

---

# `SIGCHLD` is different from the namespace flags

`SIGCHLD` may appear alongside these flags when `container_run()` calls `clone()`.

It is important not to confuse it with the namespace flags.

The namespace constants mean:

```text
CLONE_NEWUSER -> new user namespace
CLONE_NEWPID  -> new PID namespace
CLONE_NEWNS   -> new mount namespace
CLONE_NEWUTS  -> new UTS namespace
CLONE_NEWNET  -> new network namespace
```

`SIGCHLD` is **not a namespace**.

It is a signal used for normal child-process termination behavior.

The parent can then use mechanisms such as `waitpid()` to wait for the child.

So conceptually:

```text
namespace flags -> what environment the child gets
SIGCHLD         -> child-process termination behavior
```

---

# `container_write_idmaps()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Establish the UID and GID mappings for the child's new user namespace.

The mapping used here connects:

```text
UID 0 inside the namespace
        ↓
the runtime user's UID on the host
```

and similarly:

```text
GID 0 inside the namespace
        ↓
the runtime user's GID on the host
```

**Used by:** The parent calls this after `clone()` and before releasing the child through the synchronization pipe.

```c
int container_write_idmaps(struct container *c, pid_t child)
{
    (void)c;
    char path[64];
    char mapping[64];

    snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());

    if (write_file(path, mapping) < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);

    if (write_file(path, "deny") < 0)
        return -1;

    snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
    snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());

    if (write_file(path, mapping) < 0)
        return -1;

    return 0;
}
```

---

# Parameters and local variables

Before going line by line, it helps to identify what each piece of data represents.

| Name      | Type                 | Purpose                                                                                                        |
| --------- | -------------------- | -------------------------------------------------------------------------------------------------------------- |
| `c`       | `struct container *` | Container configuration. This function does not use any field from it, but the required interface includes it. |
| `child`   | `pid_t`              | Host-visible PID of the child process.                                                                         |
| `path`    | `char[64]`           | Temporary buffer for a `/proc/...` control-file path.                                                          |
| `mapping` | `char[64]`           | Temporary buffer for an ID-mapping rule.                                                                       |

`pid_t` is the system type used for process IDs.

The code casts the PID to `int` in the `snprintf()` calls because `%d` expects an `int`.

---

# `(void)c`

```c
(void)c;
```

The function receives a parameter named `c`, but this particular function does not actually need anything from the container structure.

However, the function signature is fixed by the assignment/interface.

Writing:

```c
(void)c;
```

explicitly tells the compiler:

> I intentionally received this parameter and intentionally do not use it.

This prevents an unused-parameter warning.

It does not change the container and does not perform any namespace operation.

---

# The path buffer

```c
char path[64];
```

This creates a character array on the stack.

The function reuses this buffer several times.

It will first contain something like:

```text
/proc/4312/uid_map
```

then:

```text
/proc/4312/setgroups
```

and finally:

```text
/proc/4312/gid_map
```

The same array can be reused because each `snprintf()` replaces its previous contents.

---

# The mapping buffer

```c
char mapping[64];
```

This creates another character array.

It holds the textual ID mapping that will be written to the kernel.

For example:

```text
0 1000 1
```

The first number is the namespace ID.

The second number is the corresponding host ID.

The final number says how many consecutive IDs are mapped.

---

# Constructing the UID map path

```c
snprintf(path, sizeof path, "/proc/%d/uid_map", (int)child);
```

Suppose:

```text
child = 4312
```

The result becomes:

```text
/proc/4312/uid_map
```

Let's break down the format string:

```c
"/proc/%d/uid_map"
```

`%d` is replaced by the integer child PID.

The cast:

```c
(int)child
```

converts the `pid_t` value to `int` for `%d`.

Again, `snprintf()` is only constructing a string.

It has **not** written the UID mapping yet.

---

# Constructing the UID mapping

```c
snprintf(mapping, sizeof mapping, "0 %d 1", (int)getuid());
```

This is the first important identity operation.

First:

```c
getuid()
```

returns the real UID of the user running the container runtime on the host.

Suppose the runtime is being run by host UID:

```text
1000
```

Then the format:

```text
"0 %d 1"
```

produces:

```text
0 1000 1
```

This means:

```text
namespace UID 0
        ↓
host UID 1000
```

The final `1` means one consecutive UID is being mapped.

So the mapping establishes a relationship between namespace UID 0 and the ordinary host UID.

---

# Writing `uid_map`

```c
if (write_file(path, mapping) < 0)
    return -1;
```

At this point:

```text
path =
/proc/4312/uid_map

mapping =
0 1000 1
```

`write_file()` performs the actual write.

Unlike `snprintf()`, this is an operation that reaches the kernel interface.

The kernel interprets the contents as the UID mapping for the child's user namespace.

If the write fails:

```c
return -1;
```

stops the function immediately.

There is no point continuing with the rest of the identity setup if the UID mapping could not be established.

---

# Constructing the `setgroups` path

```c
snprintf(path, sizeof path, "/proc/%d/setgroups", (int)child);
```

The same `path` buffer is reused.

For child PID `4312`, it becomes:

```text
/proc/4312/setgroups
```

This is another kernel-provided control interface associated with the child's user namespace.

---

# Writing `deny` to `setgroups`

```c
if (write_file(path, "deny") < 0)
    return -1;
```

This writes:

```text
deny
```

to the child's `setgroups` control file.

This step is required before writing the GID mapping in the setup used here.

The important sequence is:

```text
uid_map
   ↓
setgroups = deny
   ↓
gid_map
```

If the `setgroups` write fails, the function immediately reports failure.

---

# Constructing the GID map path

```c
snprintf(path, sizeof path, "/proc/%d/gid_map", (int)child);
```

The `path` buffer is reused again.

For child PID `4312`:

```text
/proc/4312/gid_map
```

This is the GID equivalent of `uid_map`.

---

# Constructing the GID mapping

```c
snprintf(mapping, sizeof mapping, "0 %d 1", (int)getgid());
```

This time the code calls:

```c
getgid()
```

instead of:

```c
getuid()
```

`getgid()` returns the real group ID of the runtime user on the host.

Suppose it returns:

```text
1000
```

Then:

```text
0 1000 1
```

is placed into `mapping`.

Conceptually:

```text
namespace GID 0
        ↓
host GID 1000
```

So the function establishes both sides of the identity relationship:

```text
UID 0 inside -> runtime user's host UID
GID 0 inside -> runtime user's host GID
```

---

# Writing `gid_map`

```c
if (write_file(path, mapping) < 0)
    return -1;
```

This finally writes the GID mapping to:

```text
/proc/<child>/gid_map
```

If it fails, the function reports failure.

If it succeeds, all three required writes have completed.

---

# Successful completion

```c
return 0;
```

At this point:

```text
uid_map    -> configured
setgroups  -> denied
gid_map    -> configured
```

The function tells its caller that the identity setup succeeded.

---

# Why the parent performs the ID-map writes

The important process ordering is:

```text
Parent
  |
  | clone()
  v
Child enters new namespaces
  |
  | child waits on synchronization pipe
  |
  |-----------------------------|
  |                             |
  v                             |
Parent writes UID/GID maps      |
  |                             |
  v                             |
Parent finishes setup           |
  |                             |
  | releases child -------------|
                                v
                         Child continues
```

The child does not immediately continue into the requested command.

Instead, the synchronization mechanism gives the parent time to configure the child's user namespace.

This matters because a newly created user namespace does not automatically contain the desired UID/GID mappings.

The parent can address the child through its host-visible PID:

```text
/proc/<child>/uid_map
/proc/<child>/setgroups
/proc/<child>/gid_map
```

and write the required configuration.

---

# Why `/proc/<child>/...` works

`/proc` is a kernel-provided virtual filesystem.

It is not an ordinary directory containing normal files stored on disk.

Linux exposes process and kernel interfaces through it.

Therefore:

```text
/proc/4312/uid_map
```

can act as a kernel interface for configuring the user namespace associated with process `4312`.

The parent knows the child's host-visible PID because `clone()` returns that PID to the parent.

So the parent can construct the appropriate `/proc/<child>/...` paths.

---

# What the UID mapping does and does not mean

Suppose the host user is UID `1000`.

The mapping:

```text
0 1000 1
```

means:

```text
inside namespace       host
----------------       ----
UID 0                  UID 1000
```

Therefore a process can be:

```text
UID 0
```

from the perspective of its user namespace while still corresponding to:

```text
UID 1000
```

when the host kernel evaluates resources owned by the ordinary host user.

This is why **namespace root is not automatically host root**.

The user namespace changes how IDs are interpreted within that namespace. It does not magically grant the process the host user's or root's unrestricted privileges over host resources.

---

# The complete identity story

The two functions work together, but they perform different jobs.

First:

```c
container_namespaces()
```

constructs the namespace bitmask:

```text
USER
PID
MOUNT
UTS
NETWORK
```

That bitmask is eventually given to `clone()`.

`clone()` then creates the child with those requested namespace environments.

After the child exists, the parent uses:

```c
container_write_idmaps()
```

to configure the user namespace's identity mapping.

The overall sequence is:

```text
container_namespaces()
        |
        | construct namespace flags
        v
clone()
        |
        | create child with isolated namespaces
        v
child exists
        |
        | child waits
        v
parent writes:
    uid_map
    setgroups
    gid_map
        |
        v
parent finishes setup
        |
        v
parent releases child
        |
        v
child continues with configured namespaces and identity
```

The key mental model is:

```text
container_namespaces()
    = "Which isolated views should the child have?"

container_write_idmaps()
    = "How should UID/GID numbers inside the user namespace
       correspond to IDs on the host?"
```

Neither function by itself creates a complete container. They are two pieces of the larger `container_run()` setup process.
