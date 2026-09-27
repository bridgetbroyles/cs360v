# 8. Critical Data Structures

This is a reference chapter, not the best first introduction. Read Chapters 4,
5, and 7 first. A **structure** groups related C fields; `object.field` accesses a
field in an object and `pointer->field` accesses it through a pointer.

The table separates who is allowed to write state from who consumes it. “Guest”
means untrusted VM code; “backend” means trusted host setup code; “handler” means
our host-side device algorithm.

| Structure/field | Represents | Written by | Read by | Invariant |
|---|---|---|---|---|
| `vmm.uc` | handle identifying the emulated CPU | `vmm_create` | all Unicorn calls | valid while VM lives |
| `vmm.ram` | host backing of guest RAM | create/load | Unicorn/device | exactly 16 MiB |
| `vmm.dev` | custom logger state | create/init | MMIO callbacks | points back to same VMM |
| `vmm.store` | host log sink | create | device/netlog | owns record formatting/file |
| `powered_off` | clean shutdown flag | `serial_write` | `vmm_run` | set before stopping |
| `exit_code` | guest result | `serial_write` | `vmm_run` | meaningful after poweroff |
| `faulted/fault_addr` | invalid-access result | `mem_invalid` | run loop/debugger | fault result has priority |
| `vlog_device.status` | packed READY/error result register | init/commands | guest MMIO read | most recent command result |
| `msg_addr_lo/hi` | 64-bit message GPA halves | guest MMIO | commands | recombined with widened shift |
| `len/level` | latched operands | guest MMIO | commands | persist across commands |
| `seq` | successful LOG count | `cmd_log` | read/STAT/store | increment after append |
| `bytes` | successful message-byte sum | `cmd_log` | STAT | excludes log prefix/newline |
| `vlog_stats` | counters returned to guest | `cmd_stat` | guest | two 32-bit fields |
| `virtq_mem_region` | one GPA/HVA mapping | backend | translator | buffer must fit wholly |
| `virtq_mem` | region table | backend | translator/walker | may contain gaps |
| `virtq.desc` | host pointer to main descriptor table | guest fills/backend maps | walker | `num` entries |
| `virtq.avail` | guest-published heads | guest | handler | idx published after content |
| `virtq.used` | device completions | handler | guest | entry published before idx |
| `virtq.num` | queue capacity | backend | handler | zero is handled; normally power of 2 |
| `last_avail` | device-owned consume cursor | handler | handler | next logical avail entry |
| `vring_desc.addr/len` | GPA and size | guest | translator/walker | untrusted until checked |
| `vring_desc.flags/next` | type/direction/link | guest | walker | next bounds-checked before access |

## Constants to memorize

These constants come from `emulator/vmm.h`, `emulator/device.h`, `virtq.h`, and
the standard virtio ring header—not from arbitrary numbers in our `.c` files.

- `RAM_BASE=0x00100000`, `RAM_SIZE=16 MiB`.
- `SERIAL_BASE=0x10000000`, `DEV_BASE=0x20000000`.
- `BOOTINFO_SIZE=64 KiB`; RSP is 16 bytes below its base.
- `VMM_EXIT_FAULT=77`.
- `VLOG_MAX_MSG=VIRTQ_MAX_RECORD=4096`.
- Descriptor flags: NEXT=1, WRITE=2, INDIRECT=4.
- Levels: DEBUG=0, INFO=1, WARN=2, ERROR=3; other=`LVL?`.
- Errors: BADCMD=1, BADADDR=2, BADLEN=3.
