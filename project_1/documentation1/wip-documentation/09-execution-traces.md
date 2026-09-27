# 9. End-to-End Execution Traces

This chapter follows control flow in time. An arrow means “causes the next
operation,” not necessarily a C function call. `main()` below is the host entry
point in `emulator/main.c`; guest `main()` is a different function compiled into
the bare-metal image.

## A. Normal bare-metal boot

Host `main()` parses arguments -> `vmm_create()` creates CPU/RAM/MMIO ->
`vmm_load_binary()` copies flat code and sets RIP -> optional boot info is copied
to reserved RAM -> `vmm_run()` starts Unicorn at RAM base -> `guest/start.S`
establishes C execution using prepared RSP/RDI -> guest `main()` runs.

## B. Guest prints a character

Guest `putc_('A')` -> `mmio_w32(SERIAL_BASE+SERIAL_TX,'A')` -> Unicorn sees
serial MMIO mapping -> calls `serial_write()` with offset zero -> masks low byte
-> host `putchar('A')` -> guest resumes.

## C. Guest powers off

Guest `vm_exit(5)` -> store 5 at serial base+8 -> callback stores exit code 5,
sets `powered_off`, stops Unicorn -> run loop sees no fault and returns 5 -> host
emulator process exits 5.

## D. Guest accesses invalid memory

Guest instruction touches unmapped address -> Unicorn invokes `mem_invalid()` ->
callback stores fault state/address, prints stderr, stops, returns false ->
`uc_emu_start()` returns -> `vmm_run()` checks fault first -> returns 77.

## E. MMIO LOG

Guest `vlog_write()` converts its guest virtual pointer to the numerically equal
GPA because paging is off, writes
MSG_LO, MSG_HI, LEN, LEVEL, then CMD=1 -> write callback truncates values to 32
bits and dispatches CMD -> `run_command()` clears old error -> `cmd_log()` checks
4096 cap and translates -> `logstore_append()` writes `[seq] LEVEL bytes\n` ->
counters advance -> guest reads STATUS, sees READY without ERROR, and the
guest-side `vlog_write()` wrapper returns zero for success.

## F. MMIO STAT

Guest allocates `struct vlog_stats`, writes its GPA and LEN, then CMD=3 -> error
cleared -> `cmd_stat()` verifies LEN>=8 -> translator verifies eight mapped bytes
-> local stats constructed -> `memcpy()` writes possibly unaligned guest buffer
-> callback returns synchronously -> guest reads updated fields.

## G. Real virtio log

The tiny **init** program is the first user-space program Linux runs after boot.
It writes `"1 function runner started\n"` to `/dev/hvc0` -> stock
virtio console driver places bytes in guest RAM and builds descriptors -> driver
places chain head in avail ring, advances idx, kicks -> QEMU/vhost-user backend
calls `vlog_virtq_handle()` -> handler snapshots idx and acquires ->
`walk_chain()` validates indices and translates data -> local record assembled ->
`vlog_sink_emit()` (provided host helper) strips newline, parses the leading
level digit, and calls logstore -> used
entry `{head,0}` written, release barrier, idx advanced -> backend interrupts ->
Linux reclaims/reuses descriptors.

## Whiteboard rule

For every trace, draw ownership and address conversion. Never jump directly
from “guest pointer” to “host reads it”; the translator is the security boundary.
