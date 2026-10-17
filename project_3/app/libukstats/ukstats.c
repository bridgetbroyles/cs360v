/* ukstats.c: statistics library compiled into the runner unikernel.
 *
 * It implements three things from SPEC.md Part III:
 *
 *   1. a boot hook, which runs during boot and records when boot finished;
 *   2. runner_on_invoke(), which counts invocations;
 *   3. runner_platform_info(), which answers the runner's INFO command.
 *
 * runner.c supplies weak defaults that these definitions replace.
 */
#include <stdio.h>

/* TODO (implemented): include the required Unikraft APIs. */
#include <uk/init.h>
#include <uk/alloc.h>
#include <uk/plat/time.h>

/* TODO (implemented): store boot time and invocation count. */
static __nsec boot_ns;
static unsigned long invocations;

/* TODO 1 (implemented): record and print boot completion time. */
/*
 * Called by Unikraft during late boot, before runner.c's main(). This function
 * records when boot reached this point and writes the required boot record to
 * the console log captured by ukdeploy.py.
 */
static int ukstats_boot_finished(void)
{
	boot_ns = ukplat_monotonic_clock();

	printf("1 unikernel booted in %lu ms\n",
	       (unsigned long)(boot_ns / 1000000));

	return 0;
}

/* Register the function above as a priority-zero late initialization hook. */
uk_late_initcall(ukstats_boot_finished, 0);

/* TODO 2 (implemented): count successful RUN commands. */
/*
 * Called by runner.c after each successful RUN command. The function name is
 * not needed because the project asks for one total invocation count.
 */
void runner_on_invoke(const char *fn)
{
	/* The project tracks only the total count, not counts by function name. */
	(void)fn;
	invocations++;
}

/* TODO 3 (implemented): build the INFO response from live statistics. */
/*
 * Called by runner.c when it handles INFO. This function reads live kernel
 * statistics, formats the required response into the caller's buffer, and
 * returns success.
 */
int runner_platform_info(char *out, size_t n)
{
	struct uk_alloc *allocator = uk_alloc_get_default();

	/* Report -1 if no allocator exists; otherwise ask it for available bytes. */
	long free_memory = allocator == NULL
		? -1L
		: (long)uk_alloc_availmem(allocator);

	unsigned long uptime_ms =
		(unsigned long)(ukplat_monotonic_clock() / 1000000);

	unsigned long boot_ms = (unsigned long)(boot_ns / 1000000);

	snprintf(out, n,
		 "platform=unikraft freemem=%ld uptime_ms=%lu "
		 "boot_ms=%lu invocations=%lu",
		 free_memory, uptime_ms, boot_ms, invocations);

	return 0;
}
