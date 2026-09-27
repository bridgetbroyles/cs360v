# 15. Final Cheat Sheet

This is compressed review, not introductory teaching. Read the earlier chapters
first. Expanded names: VMM=virtual machine monitor, MMIO=memory-mapped I/O,
GPA=guest physical address, HVA=host virtual address.

## Architecture

Parts I/II: bare-metal guest -> Unicorn executes its stores -> our MMIO callback
-> checked guest RAM -> provided logstore. Part III: Linux writes `/dev/hvc0` ->
its virtio driver publishes descriptors in the available ring -> vhost-user
backend -> sink/logstore -> used-ring completion returns buffers to Linux.

## Ownership and source locations

- Part I: our TODO implementations are in `project_1/emulator/vmm.c`; its
  constants, structures, and declarations in `vmm.h` were provided.
- Part II: our callback/command implementation is in
  `project_1/emulator/device.c`; `device.h`, `guest/vlog.c`, and logstore were
  provided. Part II uses Part I's VM, MMIO mapping, and GPA translator.
- Part III: our queue implementation is in `project_1/vhost/virtio.c`;
  `virtq.h`, backend, sink, Linux guest, and QEMU launcher were provided.

## Functions

- `serial_write`: low-byte console or poweroff/exit.
- `mem_invalid`: record fault, stderr, stop, return false.
- `vmm_create`: store, CPU, RAM map, MMIO, registers, hooks.
- `vmm_load_binary`: flat bytes into RAM; RIP=base.
- `vmm_run`: execute; fault=77; unclean error=1; else exit code.
- `vmm_gpa_to_host`: one-region remaining-space translation.
- `cmd_log`: validate/read/append/update counters.
- `cmd_stat`: validate/build stats/unaligned-safe copy to guest.
- `run_command`: clear old error and dispatch.
- MMIO read/write: expose/latch registers.
- `virtq_gpa_to_hva`: nonempty range inside one QEMU region.
- `append_data`: translate and cap copy.
- `walk_chain`: bounded direct/indirect traversal.
- `vlog_virtq_handle`: avail -> record -> sink -> used.

## Memorize

- RAM `0x00100000`, 16 MiB; serial `0x10000000`; device `0x20000000`.
- RIP=RAM base; RSP=boot-info base-16; RDI=boot-info base.
- Device register offsets from DEV_BASE: ID 00, VERSION 04, STATUS 08, CMD 0c,
  MSG 10/14, LEN 18, LEVEL 1c, SEQ 20.
- Commands: NOP 0, LOG 1, FLUSH 2, STAT 3.
- Flags: NEXT 1, WRITE 2, INDIRECT 4.
- Limits: messages/records 4096; fault exit 77.

## Critical invariants

- Guest address is never directly dereferenced.
- Entire range must fit one mapping.
- Validate before side effect.
- Only successful LOG changes `seq/bytes`.
- Device-writable descriptor is not input.
- Check index before table access; cap hops to table size.
- Copy at most remaining record space.
- Used entry becomes visible before used idx.
- Complete malformed work so driver resources return.

## Implementation2-specific facts

- Device commands are factored into helpers.
- STAT uses local struct + `memcpy` for unaligned GPA.
- Part I rejects start exactly at RAM end, even LEN zero.
- Virtio rejects LEN zero explicitly and checks addition overflow.
- One `walk_chain` handles main and indirect tables recursively.
- Indirect remainder bytes are ignored; host alignment is checked.
- INDIRECT branch precedes WRITE branch.
- Empty sink emissions are ignored; bad heads still complete.
- Queue host pointers are assumed valid; guest metadata is checked.
- `vmm_create` has partial-failure cleanup limitations.

## Commands

```bash
cd project_1/tests
P1_SEEDS=20 ./run_tests.sh
./run_virtq_tests.sh
cd ../vhost && ./run-qemu.sh   # optional Linux integration
cd ../..
git diff --check
git status --short
```

## 20 interview facts

1. A bare-metal guest has no guest OS.
2. Unicorn executes its x86-64 instructions.
3. Our VMM supplies machine layout/policy/devices.
4. QEMU runs the separate real-Linux Part III VM.
5. Virtio is why Linux needs no custom driver.
6. vhost-user puts the backend in another process.
7. MMIO turns special loads/stores into callbacks.
8. Paging off means GVA=GPA, never GPA=HVA.
9. Host-backed RAM is shared between Unicorn and device code.
10. Range subtraction avoids unsigned overflow.
11. Poweroff state distinguishes intentional stop.
12. Fault hook converts invalid access to exit 77.
13. CMD is the synchronous MMIO commit point.
14. Error is cleared before every command.
15. Empty LOG is valid and does not examine MSG.
16. STAT is host-to-guest and alignment-safe.
17. Descriptors point to buffers; avail publishes heads.
18. Bounded walkers prevent guest-created loops.
19. Used ring returns ownership to Linux.
20. Barriers order shared-memory publication.
