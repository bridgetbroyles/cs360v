# Project 1 — Quiz Quick Reference

Use this as a **fast lookup**, not a full explanation. The longer guide explains the why.

---

## The one-sentence project

We built a tiny **hypervisor**: a host C program that runs a tiny x86-64 guest program, gives it RAM and devices, and safely handles the guest's requests.

## Vocabulary in 20 seconds

| Term | Meaning |
|---|---|
| **Host** | Our normal machine/process running the VMM. |
| **Guest** | The tiny program running *inside* the virtual machine. |
| **VMM / hypervisor** | Host code that creates and controls the VM. |
| **Bare metal guest** | Guest has no Linux/OS underneath it; it talks directly to hardware-like addresses. |
| **GPA** | Guest physical address: address the guest believes it is using. |
| **HVA** | Host virtual address: actual pointer in our VMM process. |
| **MMIO** | Memory-mapped I/O: special addresses act like device registers, not RAM. |
| **Unicorn** | Library that emulates the guest x86-64 CPU instructions. |
| **QEMU** | A larger VM emulator; only used for optional Linux VM testing, not Project 1's core emulator. |

---

## Big picture: one guest instruction

```text
guest executes instruction
  ├─ address is RAM GPA  → Unicorn reads/writes v->ram
  ├─ address is MMIO GPA → Unicorn calls our memory hook → device code runs
  └─ address is invalid  → our hook stops emulation → VMM returns fault (77)
```

## Memory map

```text
Guest physical addresses (GPAs)

RAM_BASE = 0x00100000  ──┐
                           │  16 MiB guest RAM, backed by host buffer v->ram
RAM_BASE + RAM_SIZE     ──┘

DEV_BASE = 0x10000000     logging-device registers (MMIO)
serial TX = 0x10000000    serial output byte register (same project address area)
poweroff  = 0x10000008    guest writes exit status here
```

**Key address conversion:** `host_pointer = v->ram + (gpa - RAM_BASE)`.

The subtraction is why `v->ram[0]` represents **guest address `RAM_BASE`**, not guest address zero.

---

## Part I — VMM essentials

### Startup flow

```text
allocate host buffer v->ram
map it in Unicorn at GPA RAM_BASE
copy guest .bin bytes to v->ram[0]
set RIP = RAM_BASE              ← first binary byte/instruction
set RSP near top of guest RAM   ← C code needs a stack
uc_emu_start()
```

### Important functions

| Function | Job |
|---|---|
| `vmm_init` | Creates Unicorn, allocates/maps guest RAM, installs hooks. |
| `vmm_load_binary` | Copies flat binary to `v->ram[0]`; sets `RIP = RAM_BASE`. |
| `hook_mem_invalid` | Handles unmapped access; records a fault and calls `uc_emu_stop`. |
| `hook_mmio` | Recognizes device/serial/poweroff addresses and dispatches them. |
| `vmm_run` | Starts Unicorn, then returns the final exit/fault result. |

**`uc_emu_stop()`** stops only this `uc_emu_start()` emulation run. It does not stop macOS/Linux or “the whole CPU.” The VMM then resumes in normal host C code.

### Return values

`false` / `0` commonly means “this operation did not succeed/handle the request”; `true` / nonzero means success. Always check the particular function’s documented convention.

---

## Part II — logging device

### Why it exists

The logging device gives the guest a realistic device interface: the guest programs registers, writes a command, and the host validates guest-provided memory before recording a log line.

### Registers

| Register | Meaning |
|---|---|
| `ID` | Read-only magic value proving this is the expected device. |
| `VERSION` | Read-only interface version. |
| `MSG_LO`, `MSG_HI` | Low/high 32-bit halves of the **64-bit GPA** of message bytes. |
| `LEN` | Number of message bytes to copy. Not a C-string length. |
| `LEVEL` | Severity: 0 DEBUG, 1 INFO, 2 WARN, 3 ERROR; others print `LVL?`. |
| `CMD` | Writing a command asks the device to act: LOG, NOP, FLUSH, or STAT. |
| `STATUS` | Result flags/error code. |
| `SEQ` | Count of successful logs so far; read-only. |

### Why two address registers?

An address is 64 bits but each register is 32 bits:

```c
uint64_t gpa = ((uint64_t)MSG_HI << 32) | MSG_LO;
```

Example: GPA `0x00000001_23456789` means `MSG_HI = 1`, `MSG_LO = 0x23456789`.

### Safe LOG rule

Before copying, require:

```text
LEN <= VLOG_MAX_MSG
RAM_BASE <= GPA
GPA + LEN <= RAM_BASE + RAM_SIZE
no unsigned overflow in GPA + LEN
```

Then copy **exactly `LEN` bytes** from guest RAM. Do not use `strlen`: messages may contain NUL (`0x00`).

### Commands

| Command | Result |
|---|---|
| `LOG` | Validates address/length, records `[sequence] LEVEL bytes`, increments `SEQ`. |
| `NOP` | Does nothing except leave valid/no-error status. |
| `FLUSH` | Makes buffered host log output durable. |
| `STAT` | Copies record/byte statistics to a guest-provided output buffer after validating it. |
| invalid | Sets `ERROR` / `BADCMD`; does not log or increment `SEQ`. |

**Part 2 depends on Part 1:** Part II uses Part I’s mapped guest RAM, invalid-memory hook, and MMIO dispatch. The device receives a GPA and translates it into the host’s `v->ram` buffer.

---

## Part III — virtqueue essentials

Part III is a different logging input path. Instead of fixed MMIO registers, a guest driver publishes **descriptors** in a virtqueue. The host walks them, gathers a log request, writes the log, and records completion in the used ring.

```text
avail ring → descriptor chain → guest buffers → host log → used ring
```

| Concept | Meaning |
|---|---|
| descriptor | Says where a guest buffer is, its length, flags, and next descriptor. |
| chain | Several descriptors linked with `NEXT`; readable pieces form one request. |
| indirect | A descriptor points to another descriptor table in guest memory. |
| writable descriptor | Device may write there; it is **not** input bytes to log. Skip it. |
| used ring | Host completion record; contains the original head descriptor ID. |

### Part III safety rules

- Translate a range only if it lies fully in **one** allowed guest-memory region.
- Reject addresses in gaps, region straddles, overflow, and zero-length translation.
- Check every descriptor `head` and `next` index is `< queue_size`.
- Bound chain walking: a malicious chain can be cyclic.
- Bound collected record size: never overflow a host buffer.
- Never read a `WRITE` descriptor as guest input; that could leak bytes.

---

## Test-case map

| Group | Main point |
|---|---|
| `m_boot`, `m_stack`, `m_ram`, `m_exit`, `m_fault` | Part I: boot, stack, RAM, clean exit, safe invalid access. |
| `t_discovery`, `t_log_*`, `t_repeated`, `t_badcmd`, `t_stat` | Part II fixed device tests. |
| `ag_guest.bin` + oracle | Part II randomized messages, boundaries, binary bytes, bad commands/addresses, recovery, registers. |
| `translate`, `single`, `chained`, `indirect` | Part III normal virtqueue behavior. |
| `cyclic`, `*_oob`, `overlong`, `straddle`, `gap_indirect`, `writable_leak` | Part III hostile-guest safety. |

---

## Fast interview answers

**Why load at `v->ram[0]`?** The buffer offset is relative to its own start. That start is mapped at guest `RAM_BASE`, so offset zero corresponds to GPA `RAM_BASE`.

**Can we load more than one binary?** This loader intentionally supports one flat program at RAM base. Multiple images would need chosen load addresses, non-overlap checks, and an entry-point decision.

**Why validate GPAs?** A GPA is guest-controlled input. Without validation, the guest could make the VMM read/write outside its RAM buffer.

**Why use a temporary copy before logging?** It limits the length and gives the host a stable, owned byte buffer rather than trusting a guest pointer/string.

**Why does a fault stop emulation?** Continuing after an unmapped access would give undefined machine state. The VMM reports the controlled guest fault instead of crashing.

**Difference between Part II and III logging?** Both ultimately produce host log records. Part II uses fixed MMIO registers plus `CMD`; Part III uses queue descriptors/rings and must defend against chains, indices, indirect tables, and multiple memory regions.

**Why are host and guest addresses different?** They belong to different address spaces. The guest sees a simulated machine; the host uses real process pointers.
