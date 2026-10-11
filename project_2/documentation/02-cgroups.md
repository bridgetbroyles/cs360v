# Cgroups and Resource Limits

## What a cgroup is and why namespaces are not enough

A **cgroup**, short for **control group**, is a Linux kernel mechanism for grouping processes and controlling how many resources those processes can use.

Namespaces and cgroups solve different problems:

* **Namespaces** control isolation: what processes can see and what resources they appear to have.
* **Cgroups** control resource usage: how much CPU, memory, how many processes, and other resources a group may consume.

For example, a PID namespace can make a container process see a separate process-numbering environment. However, the PID namespace by itself does not prevent that process from:

* consuming too much memory
* creating thousands of child processes
* exhausting resources on the host

Cgroups provide those resource limits.

With **cgroup v2**, cgroups are normally exposed through a virtual filesystem under:

```text
/sys/fs/cgroup
```

The directories and files here look like ordinary filesystem objects, but they are actually interfaces to the Linux kernel.

For example:

```text
/sys/fs/cgroup/my_container/memory.max
```

Writing a value to `memory.max` does not store ordinary data in a file. Instead, the kernel interprets the write as a request to change the memory limit for that cgroup.

Similarly, writing a PID to:

```text
cgroup.procs
```

tells the kernel to place that process into the cgroup.

Once a process belongs to a cgroup, processes it creates normally inherit that cgroup membership. This means placing the container's init process into the cgroup also causes the command that it later starts to belong to the same resource-controlled group.

The basic distinction to remember is:

```text
Namespaces -> What the process can see
Cgroups    -> How much the process can use
```

---

# `write_cgroup_limit()`

**Assignment status:** Added private helper. The specification required writing the limits but did not require this separate function.

**Purpose:** Take one limit, convert it into the text format expected by cgroup v2, and write it to the appropriate cgroup control file.

**Used by:** `container_cgroup_init()` calls it for:

* `pids.max`
* `memory.max`
* `memory.swap.max`

```c
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
{
    char path[PATH_MAX + 32];
    char value[32];

    snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);
    if (limit < 0)
        snprintf(value, sizeof value, "max");
    else
        snprintf(value, sizeof value, "%ld", limit);
    return write_file(path, value);
}
```

## Function declaration

```c
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
```

There are several pieces here.

### `static`

```c
static
```

means this function is only accessible from this source file.

It is therefore a private helper rather than part of the public interface exposed through something like `container.h`.

### `int`

```c
int
```

is the function's return type.

The function ultimately returns the result of `write_file()`, which uses the project's usual convention of:

```text
0  -> success
-1 -> failure
```

### `struct container *c`

```c
struct container *c
```

is a pointer to the container's configuration/state structure.

The function uses:

```c
c->cg_path
```

to determine which cgroup directory belongs to this container.

### `const char *file_name`

```c
const char *file_name
```

is the name of the particular cgroup control file that should be modified.

Examples:

```text
pids.max
memory.max
memory.swap.max
```

`const` means this function promises not to modify the string being pointed to.

### `long limit`

```c
long limit
```

is the numerical resource limit that should be written.

For example:

```text
100
512000000
0
```

A negative value has a special meaning in this helper: it represents an unlimited resource and will be converted into the cgroup v2 string:

```text
max
```

---

## Local variables

```c
char path[PATH_MAX + 32];
char value[32];
```

Two character arrays are created.

### `path`

```c
char path[PATH_MAX + 32];
```

holds the complete path to the cgroup control file.

For example, the pieces:

```text
c->cg_path = /sys/fs/cgroup/my_container
file_name  = memory.max
```

become:

```text
/sys/fs/cgroup/my_container/memory.max
```

The extra space beyond `PATH_MAX` provides room for the slash and the control-file name.

### `value`

```c
char value[32];
```

holds the text that will actually be written to the control file.

It might contain:

```text
512000000
```

or:

```text
max
```

The cgroup interface expects text, so the numeric `long` cannot simply be passed directly to `write_file()`.

---

## Constructing the control-file path

```c
snprintf(path, sizeof path, "%s/%s", c->cg_path, file_name);
```

This constructs the path in memory.

The format string:

```c
"%s/%s"
```

means:

```text
first string + "/" + second string
```

So if:

```text
c->cg_path = /sys/fs/cgroup/my_container
file_name  = memory.max
```

then `path` becomes:

```text
/sys/fs/cgroup/my_container/memory.max
```

Importantly, `snprintf()` has **not changed the cgroup**.

It has only created a string in memory.

---

## Handling unlimited limits

```c
if (limit < 0)
```

checks whether the supplied limit is negative.

This helper interprets a negative limit as:

```text
unlimited
```

Cgroup v2 represents an unlimited value using the text:

```text
max
```

Therefore:

```c
if (limit < 0)
    snprintf(value, sizeof value, "max");
```

stores the string:

```text
max
```

inside `value`.

Again, this `snprintf()` only constructs the string. The cgroup is not changed yet.

---

## Handling ordinary numeric limits

```c
else
    snprintf(value, sizeof value, "%ld", limit);
```

If the limit is not negative, the number is converted into decimal text.

The `%ld` format specifier means:

```text
format a long integer as a decimal number
```

For example:

```c
limit = 500
```

causes:

```text
value = "500"
```

At this point we have prepared both pieces:

```text
path  -> where to write
value -> what to write
```

---

## Actually changing the cgroup

```c
return write_file(path, value);
```

This is the line that actually performs the write.

The previous `snprintf()` calls only prepared strings.

`write_file()` takes:

```text
path
value
```

and writes `value` to the kernel's cgroup control file at `path`.

For example:

```text
path  = /sys/fs/cgroup/my_container/memory.max
value = 500000000
```

causes the memory limit for that cgroup to be changed.

Returning the result directly also means the helper passes the success/failure result back to its caller.

---

# `container_cgroup_init()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Set up the container's cgroup:

1. enable the needed controllers for child cgroups
2. construct the container cgroup's path
3. create the container cgroup
4. apply the process limit
5. apply the memory limit
6. disable swap for the cgroup

**Used by:** `container_run()` calls it before creating/releasing the child.

```c
int container_cgroup_init(struct container *c)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
    if (write_file(path, "+pids +memory") < 0)
        return -1;

    snprintf(c->cg_path, sizeof c->cg_path,
             "%s/%s", c->cgroup_base, c->name);
    if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "container: create cgroup %s: %s\n",
                c->cg_path, strerror(errno));
        return -1;
    }

    if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
        return -1;

    if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
        return -1;

    if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
        return -1;

    return 0;
}
```

## Function declaration

```c
int container_cgroup_init(struct container *c)
```

This function receives the container structure so it can access information such as:

```c
c->cgroup_base
c->name
c->pids_max
c->mem_max
c->cg_path
```

It returns:

```text
0  -> initialization succeeded
-1 -> initialization failed
```

---

## Temporary path variable

```c
char path[PATH_MAX];
```

This creates a buffer for temporarily storing the path to the parent cgroup's control file.

This variable is separate from:

```c
c->cg_path
```

because `path` is only needed temporarily, while `c->cg_path` needs to remain available to other container functions.

---

# Enabling controllers for child cgroups

```c
snprintf(path, sizeof path, "%s/cgroup.subtree_control", c->cgroup_base);
```

This constructs the path to the parent's:

```text
cgroup.subtree_control
```

For example, if:

```text
c->cgroup_base = /sys/fs/cgroup
```

then:

```text
path =
/sys/fs/cgroup/cgroup.subtree_control
```

The important distinction is that `cgroup.subtree_control` is a **controller configuration interface**.

It specifies which resource controllers are enabled for child cgroups.

It is not where the actual `memory.max` or `pids.max` limits are stored.

---

```c
if (write_file(path, "+pids +memory") < 0)
    return -1;
```

This writes:

```text
+pids +memory
```

to `cgroup.subtree_control`.

The `+` means **enable/add this controller**.

Therefore:

```text
+pids
```

enables the pids controller for child cgroups.

```text
+memory
```

enables the memory controller for child cgroups.

The purpose is to make those controllers available so the container's child cgroup can later use:

```text
pids.max
memory.max
memory.swap.max
```

This line is what actually asks the kernel to change the controller configuration. `snprintf()` above only constructed the path.

If `write_file()` returns a value less than zero, the write failed:

```c
return -1;
```

The function stops immediately because there is no point continuing if the required controllers could not be enabled.

---

# Constructing the container cgroup path

```c
snprintf(c->cg_path, sizeof c->cg_path,
         "%s/%s", c->cgroup_base, c->name);
```

This creates the path for this specific container's cgroup.

For example:

```text
c->cgroup_base = /sys/fs/cgroup
c->name        = container1
```

produces:

```text
/sys/fs/cgroup/container1
```

Notice that this time the destination is:

```c
c->cg_path
```

rather than the temporary `path` variable.

That is because later functions need to know where this container's cgroup is.

For example, `container_cgroup_enter()` will eventually use:

```text
c->cg_path/cgroup.procs
```

---

# Creating the cgroup directory

```c
if (mkdir(c->cg_path, 0755) < 0 && errno != EEXIST) {
```

`mkdir()` asks the operating system to create the directory represented by:

```c
c->cg_path
```

For example:

```text
/sys/fs/cgroup/container1
```

The `0755` argument specifies the directory permissions.

The important part of the condition is:

```c
mkdir(...) < 0
```

A negative return value means the creation failed.

However, the code also checks:

```c
errno != EEXIST
```

`EEXIST` means the directory already exists.

The code deliberately allows that case instead of treating it as fatal.

This can happen, for example, if an earlier run left the cgroup directory behind.

The limits are written again immediately afterward, so the existing directory can still be reused.

The overall condition therefore means:

```text
If mkdir failed
AND
the reason was NOT "already exists"
then treat it as an error.
```

---

# Reporting a creation error

```c
fprintf(stderr, "container: create cgroup %s: %s\n",
        c->cg_path, strerror(errno));
```

This does **not** change the cgroup.

It only prints a diagnostic message.

`stderr` is the standard error stream, which is conventionally used for error messages.

`strerror(errno)` converts the error code stored in `errno` into a human-readable description.

For example, the resulting message might look like:

```text
container: create cgroup /sys/fs/cgroup/container1: Permission denied
```

So there are three different concepts here:

```text
mkdir()       -> attempts the actual filesystem operation
fprintf()     -> tells the human what went wrong
return -1     -> tells the rest of the program that setup failed
```

---

```c
return -1;
```

The function stops because the cgroup could not be created.

The caller can then handle the failure instead of continuing as if resource isolation had been successfully configured.

---

# Applying the process limit

```c
if (write_cgroup_limit(c, "pids.max", c->pids_max) < 0)
    return -1;
```

This calls the helper explained earlier.

The arguments mean:

```text
c                  -> which container
"pids.max"         -> which cgroup control file
c->pids_max       -> what limit to write
```

The helper constructs:

```text
<cgroup path>/pids.max
```

and writes the requested value.

`pids.max` controls how many processes/tasks may exist in the cgroup.

If writing the limit fails:

```c
return -1;
```

stops initialization.

---

# Applying the memory limit

```c
if (write_cgroup_limit(c, "memory.max", c->mem_max) < 0)
    return -1;
```

This follows exactly the same process.

The helper receives:

```text
"memory.max"
```

and:

```text
c->mem_max
```

It therefore writes the configured memory limit to:

```text
<cgroup path>/memory.max
```

If the write fails, initialization fails.

---

# Disabling swap

```c
if (write_cgroup_limit(c, "memory.swap.max", 0) < 0)
    return -1;
```

This writes:

```text
0
```

to:

```text
memory.swap.max
```

The effect is to prevent this cgroup from using swap.

This is separate from the `memory.max` limit. `memory.max` controls the cgroup's memory limit, while `memory.swap.max` controls how much swap the cgroup can use.

Again, if the write fails:

```c
return -1;
```

---

# Successful completion

```c
return 0;
```

Reaching this line means all required setup steps succeeded:

```text
controllers enabled
        ↓
cgroup path constructed
        ↓
cgroup created/found
        ↓
pids.max configured
        ↓
memory.max configured
        ↓
memory.swap.max configured
```

The cgroup is now configured, but the child process has not yet been placed into it. That is the job of `container_cgroup_enter()`.

---

# `container_cgroup_enter()`

**Assignment status:** One of the 11 required functions.

**Purpose:** Place the cloned container-init process into the cgroup that was just configured.

**Used by:** The parent calls it from `container_run()` after the relevant setup and before releasing the child.

```c
int container_cgroup_enter(struct container *c, pid_t child)
{
    char path[PATH_MAX + 32];
    char pid_text[16];

    snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
    snprintf(pid_text, sizeof pid_text, "%d", (int)child);
    return write_file(path, pid_text);
}
```

## Function declaration

```c
int container_cgroup_enter(struct container *c, pid_t child)
```

The function receives:

```c
c
```

to determine which cgroup should receive the process.

It receives:

```c
child
```

which is the PID of the cloned container-init process.

---

## Local variables

```c
char path[PATH_MAX + 32];
char pid_text[16];
```

Two strings are needed.

`path` will contain:

```text
<cgroup path>/cgroup.procs
```

`pid_text` will contain the child's PID as text.

For example:

```text
child = 12345
pid_text = "12345"
```

The cgroup interface expects the PID to be written as text.

---

## Constructing the `cgroup.procs` path

```c
snprintf(path, sizeof path, "%s/cgroup.procs", c->cg_path);
```

If:

```text
c->cg_path = /sys/fs/cgroup/container1
```

then:

```text
path =
/sys/fs/cgroup/container1/cgroup.procs
```

Again, `snprintf()` only creates the string in memory. It has not moved the process yet.

---

## Converting the PID to text

```c
snprintf(pid_text, sizeof pid_text, "%d", (int)child);
```

`child` has type:

```c
pid_t
```

The code casts it to `int`:

```c
(int)child
```

and formats it using:

```c
%d
```

The result is a string.

For example:

```text
child = 12345
```

becomes:

```text
pid_text = "12345"
```

This conversion is necessary because `write_file()` writes text to the cgroup interface.

---

## Actually entering the cgroup

```c
return write_file(path, pid_text);
```

This is the operation that actually changes the process's cgroup membership.

Conceptually:

```text
path:
    /sys/fs/cgroup/container1/cgroup.procs

value:
    12345
```

Writing the PID to `cgroup.procs` tells the kernel to move that process into the cgroup.

The function returns the result of that operation directly.

Therefore:

```text
0  -> process successfully entered the cgroup
-1 -> entering the cgroup failed
```

---

# Why the child is synchronized

The child is intentionally prevented from immediately continuing while the parent finishes setting up its environment.

The important ordering is:

```text
Parent creates/configures cgroup
        ↓
Parent places child into cgroup
        ↓
Parent completes required setup
        ↓
Parent releases child
        ↓
Child continues running
```

This prevents the child from briefly running the requested command before its resource limits have been applied.

Once the container-init process is inside the cgroup, processes it creates inherit the cgroup membership.

So the complete resource-control story is:

```text
1. Enable controllers
        ↓
2. Create container cgroup
        ↓
3. Configure pids.max
        ↓
4. Configure memory.max
        ↓
5. Configure memory.swap.max
        ↓
6. Put container init into cgroup
        ↓
7. Release child to continue
        ↓
8. Command runs inside the resource-controlled group
```

# Final mental model

There are several operations in this code that look similar but do completely different things.

### `snprintf()`

Builds a string in memory.

```c
snprintf(path, ...);
```

It does **not** change the cgroup.

### `mkdir()`

Asks the operating system to create the cgroup directory.

```c
mkdir(c->cg_path, 0755);
```

### `write_file()`

Performs the actual write to a cgroup control interface.

For example:

```c
write_file(path, "+pids +memory");
```

changes which controllers are enabled for child cgroups.

Or:

```c
write_file(path, pid_text);
```

moves a process into a cgroup.

### `fprintf()`

Only prints an error message for the human.

```c
fprintf(stderr, ...);
```

It does **not** modify the cgroup.

### `return -1`

Reports failure to the caller.

It does not itself change the kernel state.

The key sequence is therefore:

```text
snprintf()
    ↓
prepare the path/value as strings

write_file()
    ↓
ask the kernel to perform the cgroup operation

return 0 / -1
    ↓
tell the caller whether the operation succeeded
```

That distinction is essential for understanding the entire cgroup implementation.
