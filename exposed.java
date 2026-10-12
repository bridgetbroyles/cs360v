Function declaration
static int write_cgroup_limit(struct container *c,
                              const char *file_name, long limit)
There are several pieces here.

static
static
means this function is only accessible from this source file.

It is therefore a private helper rather than part of the public interface exposed through something like container.h.

int
int
is the function's return type.