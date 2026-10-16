/* ukstats.c: statistics library compiled into the runner unikernel.
 *
 * It records boot time, counts successful RUN commands, and answers INFO.
 * runner.c supplies weak defaults that these definitions replace.
 */
#include <stdio.h>

/* include the required Unikraft APIs. */
/* Provide uk_late_initcall(), which registers a function to run during boot. */
#include <uk/init.h>

/* Provide access to Unikraft's default memory allocator and free-memory count. */
#include <uk/alloc.h>

/* Provide __nsec and the monotonic clock used for boot and uptime measurements. */
#include <uk/plat/time.h>

/* store boot time and invocation count. */
/* Save the boot-hook timestamp in nanoseconds so INFO can report it later. */
static __nsec boot_ns;

/* Count successful RUN commands handled since this unikernel booted. */
static unsigned long invocations;

/* TODO 1 (implemented): record and print boot completion time. */
/*
 * Called by Unikraft during late boot, before runner.c's main(). This function
 * records when boot reached this point and writes the required boot record to
 * the console log captured by ukdeploy.py.
 */
static int ukstats_boot_finished(void)
{
	/* Read the number of nanoseconds elapsed since this unikernel started. */
	boot_ns = ukplat_monotonic_clock();

	/* Convert nanoseconds to milliseconds and print the required log record. */
	printf("1 unikernel booted in %lu ms\n",
	       (unsigned long)(boot_ns / 1000000));

	/* Tell Unikraft that the boot hook completed successfully. */
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
	/* Mark the parameter intentionally unused so strict builds do not warn. */
	(void)fn;

	/* Record one more successful function invocation. */
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
	/* Ask Unikraft for the allocator that manages the image's normal memory. */
	struct uk_alloc *allocator = uk_alloc_get_default();

	/* Report -1 if no allocator exists; otherwise ask it for available bytes. */
	long free_memory = allocator == NULL
		? -1L
		: (long)uk_alloc_availmem(allocator);

	/* Read the current monotonic time and convert nanoseconds to milliseconds. */
	unsigned long uptime_ms =
		(unsigned long)(ukplat_monotonic_clock() / 1000000);

	/* Convert the boot timestamp saved by the hook to milliseconds. */
	unsigned long boot_ms = (unsigned long)(boot_ns / 1000000);

	/* Write all required fields without writing beyond the supplied buffer. */
	snprintf(out, n,
		 "platform=unikraft freemem=%ld uptime_ms=%lu "
		 "boot_ms=%lu invocations=%lu",
		 free_memory, uptime_ms, boot_ms, invocations);

	/* Tell runner.c that the INFO response was produced successfully. */
	return 0;
}
