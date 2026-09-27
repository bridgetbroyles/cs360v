# 16. Complete Project and Interview Summary

This document is the single-file review for Project 1 on
`claude/implementation2`. It begins with the mental model, explains every method
we implemented at a high level, and ends with interview questions and model
answers. The detailed chapters remain useful when you need full code listings.

## 1. The project in one paragraph

The project teaches how data safely crosses from a virtual machine into host
software. Parts I and II build a tiny virtual computer: Unicorn executes a
bare-metal x86-64 guest, our VMM supplies RAM and virtual devices, and our custom
MMIO logging device accepts a guest address and length, validates them, and
writes the message to a host log. Part III reaches the same result through a
real Linux guest under QEMU: Linux's existing virtio-console driver publishes
buffer descriptions in a virtqueue, and our external backend safely translates,
reads, logs, and completes those buffers.

```text
Parts I/II
bare-metal guest -> custom MMIO registers -> our device.c -> host log

Part III
Linux -> virtio console -> virtqueue -> our virtio.c -> host log
```

Logging is the example workload. The deeper lesson is how a host validates
untrusted guest-provided addresses, lengths, indices, and chains before touching
memory or producing side effects.

## 2. Vocabulary you must understand first

- **Host:** the real environment running the virtualization software.
- **Guest:** code or an operating system running inside a virtual machine.
- **VM:** a software-created computer with virtual CPU, RAM, and devices.
- **VMM:** host software that creates and controls a VM.
- **Bare-metal guest:** a guest program with no guest operating system. It
  cannot use Linux system calls, files, or normal drivers.
- **Unicorn:** the library that emulates the x86-64 CPU in Parts I/II. Our code
  still defines the machine around that CPU.
- **QEMU:** the complete VM software that boots Linux in Part III.
- **MMIO:** memory-mapped I/O. Loads and stores to special guest addresses invoke
  device callbacks instead of ordinary RAM.
- **GVA:** guest virtual address, used by guest instructions.
- **GPA:** guest physical address, used in the VM's hardware memory map.
- **HVA:** host virtual address, a real C pointer in a host process.
- **Virtio:** a standard interface for efficient virtual devices.
- **Virtqueue:** shared-memory structures through which a virtio driver submits
  buffers and a device returns them.
- **Backend:** host code that performs a virtual device's actual work.
- **vhost-user:** the protocol connecting QEMU to an external virtio backend.

Paging is disabled in Parts I/II, so GVA equals GPA there. GPA never
automatically equals HVA: the host and guest remain different address spaces.

## 3. What we implemented versus what was provided

| Part | Our implementation | Important provided pieces |
|---|---|---|
| I | TODO portions of `project_1/emulator/vmm.c` | `vmm.h`, `main.c`, Unicorn, tracing, boot-info loader, teardown |
| II | callbacks and command logic in `project_1/emulator/device.c` | `device.h`, guest `vlog.c`, logstore |
| III | translation and queue handling in `project_1/vhost/virtio.c` | `virtq.h`, `backend.c`, `sink.c`, QEMU launcher, Linux guest |

Part II directly depends on Part I. Part I allocates the device, maps its MMIO
page, supplies the guest RAM translator, and owns the log store. Part III is a
separate QEMU execution path; it repeats the safety pattern without calling the
Part I VMM or Part II callbacks.

## 4. Part I: construct and run the tiny VM

### Machine map

```text
0x00100000  RAM_BASE: beginning of 16 MiB guest RAM
0x010F0000  BOOTINFO_BASE: final 64 KiB reserved for optional startup data
0x01100000  end of RAM

0x10000000  serial/control MMIO page
0x20000000  custom logging-device MMIO page
```

Initial registers:

```text
RIP = RAM_BASE           next guest instruction
RSP = BOOTINFO_BASE - 16 initial stack pointer
RDI = BOOTINFO_BASE      first guest argument: boot-info pointer
```

`BOOTINFO_BASE` is not a boot device. It is only the address of reserved guest
RAM where a provided loader may place an opaque startup blob.

### Host-backed RAM

Part I allocates host memory and tells Unicorn which guest addresses it backs:

```c
v->ram = calloc(1, RAM_SIZE);
uc_mem_map_ptr(v->uc, RAM_BASE, RAM_SIZE, UC_PROT_ALL, v->ram);
```

That creates this relationship:

```text
host v->ram + 0       <-> guest RAM_BASE + 0
host v->ram + offset  <-> guest RAM_BASE + offset
```

Unicorn and our device callbacks therefore see the same underlying bytes.

### Why the binary begins at offset zero

`fread(v->ram, ...)` writes at host-buffer offset zero, which the mapping exposes
as guest address `RAM_BASE`. The guest linker script also places `_start` at
`RAM_BASE`, and the VMM initializes RIP to `RAM_BASE`.

```text
linker address RAM_BASE
       = binary byte 0
       = v->ram[0]
       = initial RIP
```

The current emulator accepts one flat guest image at a time. That image can
contain many functions and data sections. Loading again would overwrite the
start of the first image. A multi-image loader would need separate load-address
and entry-point rules.

`fseek(f, 0, SEEK_END)` does not load at the end of RAM. It moves the input
file's cursor to measure its size. The following `SEEK_SET` rewinds before
`fread()`.

### Serial/control MMIO

The serial page contains separate registers:

```text
SERIAL_TX       offset 0: print low byte of value
SERIAL_POWEROFF offset 8: store exit code and stop emulation
```

The offset chooses the operation; the value contains its data. For example:

```text
address SERIAL_BASE+0, value 0x141 -> offset 0 -> 0x141 & 0xff = 0x41 -> 'A'
address SERIAL_BASE+8, value 5     -> offset 8 -> clean VM exit code 5
```

### Part I method map

| Function | High-level explanation |
|---|---|
| `serial_write()` | Implements console output and guest-requested poweroff. It distinguishes operations by MMIO offset. |
| `mem_invalid()` | Records an access that Unicorn already determined was unmapped, stops this emulation run, and says not to retry it. |
| `vmm_create()` | Opens the log, creates Unicorn, allocates/maps RAM, maps both devices, initializes RSP/RDI, and installs hooks. |
| `vmm_load_binary()` | Measures one flat image, copies it to `v->ram[0]`, and sets RIP to `RAM_BASE`. |
| `vmm_run()` | Enters Unicorn and classifies the stop as fault, unexpected engine error, or clean guest poweroff. |
| `vmm_gpa_to_host()` | Proves an entire GPA range lies in guest RAM and returns `v->ram + offset`. |

Provided helpers include `serial_read()`, `trace_code()`,
`vmm_load_bootinfo()`, and `vmm_destroy()`.

### What happens on an invalid CPU access

Unicorn owns the mapping table. If the guest reads, writes, or fetches outside
RAM and both MMIO pages, Unicorn invokes `mem_invalid()`.

```text
unmapped guest access
 -> mem_invalid records fault/address
 -> uc_emu_stop ends the current uc_emu_start call
 -> callback returns false: mapping was not repaired; do not retry
 -> vmm_run sees faulted and returns 77
 -> provided main destroys the VM and exits
```

The guest does not resume at its next instruction. `uc_emu_stop()` stops the
emulated CPU, not a physical CPU or the host operating system.

### GPA translation and the core safety check

```c
if (gpa < RAM_BASE)
    return NULL;
uint64_t offset = gpa - RAM_BASE;
if (offset >= RAM_SIZE)
    return NULL;
if (len > RAM_SIZE - offset)
    return NULL;
return v->ram + offset;
```

Checking remaining capacity avoids overflow and catches a buffer that begins in
RAM but extends past its end. Part II must use this function; it must never cast
a guest number directly to a host pointer.

## 5. Part II: the custom MMIO logging device

### Why it exists

The bare-metal guest cannot open a host file. Instead, it places message bytes
somewhere in guest RAM, writes their GPA and length into device registers, and
writes a command. Unicorn routes those MMIO operations to `device.c`.

Unlike the executable, a message can live anywhere within guest RAM. For
example, GPA `RAM_BASE+0x3500` translates to `v->ram+0x3500` if all LEN bytes
fit.

### Register protocol

| Offset | Register | Meaning |
|---:|---|---|
| `0x00` | ID | magic identifier `VLG1` |
| `0x04` | VERSION | protocol version 1 |
| `0x08` | STATUS | READY bit 0, ERROR bit 2, error code in bits 8–15 |
| `0x0c` | CMD | NOP=0, LOG=1, FLUSH=2, STAT=3 |
| `0x10/0x14` | MSG_LO/MSG_HI | halves of one 64-bit GPA |
| `0x18` | LEN | byte count |
| `0x1c` | LEVEL | DEBUG=0, INFO=1, WARN=2, ERROR=3 |
| `0x20` | SEQ | successful LOG count |

Operand registers are latched: their values remain in `struct vlog_device`.
Writing CMD is the commit point that synchronously uses the current operands.

### Where the GPA comes from

The provided guest library receives an ordinary guest C pointer. With paging
disabled, that pointer's numeric GVA equals its GPA. It splits the 64-bit number
across MSG_LO and MSG_HI, writes LEN/LEVEL, then writes CMD=LOG.

```text
guest pointer -> numeric GPA -> MSG_LO/MSG_HI
                                  + LEN + LEVEL
                                  + CMD=LOG
```

### Part II method map

| Function | High-level explanation |
|---|---|
| `msg_addr()` | Reconstructs the 64-bit GPA from two 32-bit registers. |
| `set_error()` | Adds ERROR and a reason code to STATUS. |
| `clear_error()` | Removes the previous command error while preserving READY. |
| `vlog_device_init()` | Connects the device back to Part I's VMM and initializes its registers/counters. |
| `cmd_log()` | Validates LEN and MSG, appends one host record, then updates SEQ/bytes only after success. |
| `cmd_stat()` | Validates an output buffer and copies `{records, bytes}` into guest RAM without changing counters. |
| `run_command()` | Clears stale error state and dispatches the numeric command. |
| `vlog_device_mmio_read()` | Returns the constant or state selected by the read offset. |
| `vlog_device_mmio_write()` | Latches writable operands or invokes `run_command()` for CMD. |

### LOG versus STAT

```text
LOG:  guest RAM -> host device -> host log
STAT: host device counters -> guest RAM
```

LOG rejects LEN greater than 4096. A zero-length LOG is valid and ignores MSG.
Unknown levels are accepted and formatted as `LVL?`. Only a successful LOG
increments SEQ and total bytes.

STAT requires the guest to offer at least eight bytes, translates exactly the
eight bytes it writes, constructs a local `struct vlog_stats`, and uses
`memcpy()`. The byte copy is safe even when the guest buffer is not naturally
aligned for a C structure.

### Device errors are recoverable

A BADLEN, BADADDR, or BADCMD is not a Part I CPU fault. The guest successfully
accessed mapped device registers; the device rejected the logical request. It
sets STATUS, returns from the callback, and guest execution continues. A later
command clears the old error before running, allowing recovery.

## 6. Part III: QEMU, Linux, and virtqueues

### Why Part III is separate

Linux does not know the private Part II register protocol. QEMU instead presents
standard virtio console device ID 3, which Linux's existing `virtio_console`
driver recognizes as `/dev/hvc0`. Our code is an external `vlog-backend`
process. Provided vhost-user plumbing connects that process to QEMU through a
Unix socket and shared guest-memory mappings.

```text
Linux application
 -> /dev/hvc0
 -> Linux virtio_console driver
 -> descriptor chain in guest RAM
 -> QEMU/vhost-user
 -> our vlog_virtq_handle()
 -> provided sink/logstore
 -> used-ring completion
```

### Split-virtqueue structures

```text
descriptor table        available ring             used ring
where buffers are       guest publishes heads      device returns heads

desc[head] <----------- avail.ring[...]     head -> used.ring[...]
```

A descriptor contains a GPA, length, flags, and possibly the next descriptor
index. It points to data; it does not normally contain the data.

- NEXT continues the chain at `next`.
- WRITE means the device may write into this buffer. On this guest-to-host
  queue, it is output space rather than input data, so our code skips it.
- INDIRECT means the GPA points to another descriptor table. One indirect level
  is allowed; nested indirect tables are not.

The available ring transfers work to the device. The used ring returns
ownership to Linux. Used `len=0` means the device wrote zero bytes into guest
buffers; it does not mean the device consumed zero input bytes.

### Part III method map

| Function | High-level explanation |
|---|---|
| `virtq_gpa_to_hva()` | Searches QEMU's regions and returns an HVA only if one nonempty GPA range fits wholly in one region. |
| `append_data()` | Translates one readable descriptor and copies at most the remaining space in the 4096-byte local record. |
| `walk_chain()` | Bounds-checks indices, snapshots descriptors, handles one indirect level, skips WRITE buffers, and caps hops to prevent cycles. |
| `vlog_virtq_handle()` | Processes the finite batch currently in avail, emits each assembled record, and publishes each head in used. |
| `vlog_sink_emit()` | Provided helper that removes trailing newlines, parses `<level> `, assigns sequence, and calls logstore. |

### Why Part III needs another translator

Part I has one contiguous guest RAM allocation. QEMU may expose several regions
with gaps. Linux may place a descriptor buffer anywhere within those advertised
regions, so `virtq_gpa_to_hva()` searches for the containing region. It rejects:

- length zero;
- `gpa+len` unsigned wraparound;
- an address outside every region or in a gap;
- a range crossing a region boundary.

### Safe chain walking

For each descriptor, the walker first checks `index < table_size`, then copies
the shared descriptor into local `d`. The local snapshot keeps addr/len/flags/
next internally consistent for that iteration. A maximum of `table_size` hops
prevents a guest-created cycle from looping forever.

Readable data is gathered into a local 4096-byte record. The complete declared
descriptor range must translate even if only a prefix will fit. Indirect tables
are translated, checked for C alignment, and walked from entry zero with further
indirection disabled.

### Queue processing and memory ordering

The handler snapshots `avail->idx` once, executes an acquire barrier, and
processes until `last_avail` reaches that snapshot. New work waits for a later
call, which keeps each batch finite.

For completion, it writes `{id=head, len=0}` into a used slot, executes a release
barrier, then increments `used->idx`. This prevents Linux from observing the new
count before the completion entry is visible. A barrier orders memory; it is not
a lock, bounds check, or notification.

Malformed chains are still completed. Unsafe bytes are ignored, but returning
the head prevents Linux from waiting forever for a descriptor resource. These
errors do not stop QEMU or Linux; our function returns to the provided backend,
which may later invoke it on another kick.

## 7. The central comparison

| Question | Parts I/II | Part III |
|---|---|---|
| VM software | our VMM using Unicorn | QEMU |
| Guest | one bare-metal image | Linux |
| Guest-side protocol code | provided `guest/vlog.c` | stock Linux virtio driver |
| Submission | write MSG/LEN/LEVEL then CMD | publish descriptor-chain head in avail |
| Address translator | `vmm_gpa_to_host()` | `virtq_gpa_to_hva()` |
| Completion | STATUS and SEQ | used ring and interrupt |
| Bad request | STATUS error; guest continues | unsafe data skipped; chain completed |
| Invalid CPU address | fatal to current Part I run | handled by QEMU, outside our queue code |

## 8. End-to-end traces to practice aloud

### Parts I/II LOG

```text
guest string lives somewhere in guest RAM
 -> guest vlog_write converts pointer to GPA
 -> writes MSG_LO, MSG_HI, LEN, LEVEL
 -> writes CMD=LOG
 -> Unicorn calls vlog_device_mmio_write at CMD offset
 -> run_command clears stale error and calls cmd_log
 -> cmd_log checks 4096 limit
 -> vmm_gpa_to_host validates entire range
 -> logstore_append writes [seq] LEVEL message
 -> seq and bytes advance
 -> callback returns and guest reads STATUS
```

### Part III record

```text
Linux program writes "2 cache miss\n" to /dev/hvc0
 -> virtio_console driver places bytes in guest RAM
 -> driver fills descriptors and publishes head in avail
 -> driver kicks device
 -> provided backend calls vlog_virtq_handle
 -> walk_chain checks indices/flags and translates GPAs
 -> append_data gathers at most 4096 bytes
 -> sink parses level 2 and logs "cache miss"
 -> handler writes used entry, release barrier, used idx
 -> backend interrupts Linux
 -> Linux reuses returned descriptors
```

## 9. Safety invariants worth memorizing

1. Never cast a guest address directly to a host pointer.
2. Validate the complete range, not only its first byte.
3. Avoid overflow with remaining-space checks or explicit wrap detection.
4. Perform validation before logs, counter updates, or guest-memory writes.
5. Part II changes counters only after successful LOG append.
6. Use `memcpy()` for possibly unaligned guest structures.
7. Check a descriptor index before table access.
8. Bound chain hops to guarantee termination.
9. WRITE descriptors are not input bytes on this queue.
10. Cap the assembled record at 4096 bytes.
11. Publish a used entry before publishing the new used index.
12. Complete malformed queue work so the driver regains its resource.

## 10. Implementation-specific limitations

Be ready to distinguish assignment correctness from production hardening:

- `vmm_create()` does not unwind most resources on partial setup failure.
- Some Unicorn register/hook return values are unchecked.
- Part III assumes backend-created pointers such as `vq`, rings, `mem`, and
  `sink` are valid; it validates guest-controlled metadata.
- Indirect-table remainder bytes are ignored rather than rejected.
- INDIRECT is handled before WRITE if both malformed flags are present.
- Queue data is copied through a 4096-byte stack buffer rather than processed
  zero-copy.
- The teaching interface completes malformed chains without a per-request error
  report; production hardware might reset or expose richer status.

## 11. Interview questions and model answers

### 1. What did you build?

“We built two guest-to-host logging paths. First, a minimal VMM uses Unicorn to
run a bare-metal x86-64 guest and exposes a custom MMIO logging device. Second,
an external vhost-user backend processes a standard virtio-console queue from a
real Linux guest under QEMU. Both paths validate guest-described memory before
host access and report completion through their respective protocols.”

### 2. What does Unicorn provide, and what does your VMM provide?

“Unicorn emulates the x86-64 CPU and supplies memory/MMIO/hook APIs. Our VMM
defines the machine: guest RAM, addresses, virtual devices, initial registers,
binary loading, fault policy, execution lifecycle, and address translation.”

### 3. Why does the binary start at RAM offset zero?

“`uc_mem_map_ptr` maps host `v->ram+0` to guest `RAM_BASE`. The linker places
`_start` at `RAM_BASE`, the flat file begins with `_start`, `fread` copies byte
zero to `v->ram+0`, and RIP starts at `RAM_BASE`. These are matching parts of
the machine ABI, not a property discovered from the flat file.”

### 4. Can this implementation load two binaries?

“Not as independent images. The command line selects one flat guest image and
the loader copies it at offset zero. That image may contain many linked modules.
A multi-image design would need separate load addresses, entry points, overlap
checks, and possibly relocation support.”

### 5. What exactly happens after `mem_invalid()`?

“Unicorn calls it only after detecting an unmapped guest access. We record the
fault and address, stop the current emulation run, and return false so Unicorn
does not retry. `uc_emu_start` returns, `vmm_run` prioritizes the recorded fault
and returns 77, and provided `main` destroys the VM. The guest does not resume.”

### 6. Why mask `value & 0xff` in `serial_write()`?

“The MMIO offset already selected the TX register. That register transmits one
byte, so the mask discards any upper callback bits and gives `putchar` exactly
the low byte. Offset eight selects poweroff instead and uses the value as the
exit code.”

### 7. Why can the host not cast MSG to a pointer?

“MSG is a guest-controlled number in the guest physical map. It has no direct
meaning in the host process. We must verify its complete range belongs to guest
RAM and then add the proven offset to the host backing pointer.”

### 8. Explain Part II LOG versus STAT.

“LOG is guest-to-host: validate the message, append it, then advance record and
byte counters. STAT is host-to-guest: verify the guest offered at least eight
valid bytes and copy current counters there with `memcpy`. STAT neither logs nor
changes counters.”

### 9. Why clear STATUS error before each command?

“The helper sets errors with bitwise OR, and STATUS describes the latest command.
Clearing the old error prevents stale or combined error codes and lets a valid
command recover after an invalid one while preserving READY.”

### 10. What is QEMU doing in Part III?

“QEMU replaces our small VMM and boots Linux with a standard virtio console.
The provided vhost-user layer shares guest-memory mappings and queue
notifications with our external backend. Our `virtio.c` processes the queue; it
does not emulate the CPU or implement QEMU itself.”

### 11. Explain the three split-virtqueue structures.

“Descriptors describe buffers and link them into chains. The available ring is
written by the driver to submit chain heads. The used ring is written by the
device to return completed heads. The rings use free-running counters and
modulo queue capacity to select physical slots.”

### 12. Why skip WRITE descriptors?

“WRITE grants space the device may modify. This transmit queue carries data from
guest to host, so device-writable buffers are not guest input. Reading them
could consume or leak bytes the guest did not submit as data.”

### 13. How do you prevent a malicious descriptor chain from hanging you?

“We check every index before dereference and cap traversal at the number of
entries in the current table. An acyclic valid chain cannot require more hops;
a cyclic or overlong chain is therefore guaranteed to terminate.”

### 14. Why complete malformed chains?

“The driver transferred ownership when it published the chain. If the device
silently abandoned it, Linux could wait forever or exhaust descriptors. We emit
no unsafe data but return the head through used so the resource becomes
available again.”

### 15. Why are memory barriers needed, and why are they not locks?

“The acquire barrier prevents descriptor/ring reads from moving before the
observed avail publication. The release barrier prevents the used entry write
from moving after the used-index publication. They order visibility across
shared memory; they do not provide bounds checking, mutual exclusion, or a
notification by themselves.”

### 16. What are the strongest aspects and limitations of this implementation?

“Its strengths are whole-range translation, overflow-aware bounds checks,
validate-before-side-effect ordering, bounded descriptor traversal, direction
checks, record caps, alignment checks, and reliable completion. Limitations
include incomplete setup cleanup, some unchecked Unicorn calls, trusted backend
pointers, permissive handling of malformed flag combinations/remainders, and a
copying stack buffer rather than a production zero-copy design.”

## 12. Rapid-fire facts

- RAM begins at `0x00100000` and is 16 MiB.
- Serial begins at `0x10000000`; custom logger begins at `0x20000000`.
- RIP is instruction pointer, RSP is stack pointer, RDI is first argument.
- Part I guest paging is off: GVA=GPA, but GPA never automatically equals HVA.
- Fault exit code is 77.
- Maximum Part II message and Part III assembled record are 4096 bytes.
- MSG is an address, not message bytes.
- CMD is the Part II commit point.
- Only successful LOG changes Part II counters.
- Descriptors point to buffers; avail submits; used completes.
- NEXT=1, WRITE=2, INDIRECT=4.
- Part III used length is zero because our device writes no guest buffers.
- A kick notifies the device; an interrupt notifies the driver.
- Part II errors are recoverable STATUS results; Part I unmapped CPU access ends
  the current VM run.

## 13. A reliable way to answer any code question

For an unfamiliar line, answer in this order:

1. Identify who controls each input: host setup or untrusted guest.
2. Name the address space: GVA, GPA, HVA, MMIO offset, or ring index.
3. State the invariant being checked.
4. Walk the state change or side effect in order.
5. Explain failure behavior and whether execution continues.
6. Explain what later layer consumes the result.
7. Offer a realistic alternative and its tradeoff.

The central sentence to return to is:

> The guest may describe work and memory, but host code must validate the full
> description before accessing memory, causing an external side effect, or
> reporting completion.
