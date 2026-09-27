# 5. Part II — The Custom MMIO Logging Device

Read Chapters 0, 1, and 3 first. This chapter uses “guest pointer,” “GPA,” and
“MMIO callback” in their project-specific meanings.

## First: what the device is for

The bare-metal guest has no filesystem and cannot call host logging functions.
We therefore define a tiny hardware-like protocol. The guest places a message
in its RAM, tells the device where it is, and writes a command. The host device
validates the request and copies the bytes into a host log file.

This is **paravirtualization**: the guest knows it is virtual and deliberately
uses a guest/host protocol rather than pretending to drive a physical device.

## Exact source locations and the dependency on Part I

All paths are relative to `project_1/`:

| File | Role | Ownership for this assignment |
|---|---|---|
| `emulator/device.c` | host implementation of the custom logging device | callbacks were assigned; this branch also added command helpers |
| `emulator/device.h` | register constants, status bits, commands, state structures | provided |
| `guest/vlog.c` and `guest/vlog.h` | guest-side code that writes the registers | provided |
| `emulator/vmm.c` | creates/maps the device and translates its GPAs | Part I, mixed provided/student code |
| `emulator/logstore.c` | formats and writes the host log file | provided |

Part II is not a second VM. It plugs into the VM built in Part I:

```text
Part I creates RAM and Unicorn
  -> Part I allocates `struct vlog_device`
  -> Part I maps DEV_BASE to Part II callbacks
  -> guest writes registers
  -> Unicorn calls Part II
  -> Part II calls Part I's GPA translator
  -> provided logstore writes the file
```

The dependency is visible in both directions:

```c
/* Part I: emulator/vmm.c attaches the Part II functions. */
uc_mmio_map(v->uc, DEV_BASE, DEV_SIZE,
            vlog_device_mmio_read, v->dev,
            vlog_device_mmio_write, v->dev);

/* Part II: emulator/device.c uses Part I's VMM and RAM translator. */
msg = vmm_gpa_to_host(dev->vmm, msg_addr(dev), dev->len);
logstore_append(dev->vmm->store, dev->seq, dev->level, msg, dev->len);
```

## What we implemented versus what was given

The starter provided `device.h`, `struct vlog_device`, the constants, the three
small helpers, and `vlog_device_init()`. It provided callback signatures with
TODO bodies. We implemented `vlog_device_mmio_read()` and
`vlog_device_mmio_write()`. This branch also introduced `cmd_log()`,
`cmd_stat()`, and `run_command()` to keep callback dispatch understandable.
The guest library and logstore were already provided.

## Part II method map: what each function does at a high level

| Function | High-level job |
|---|---|
| `msg_addr()` | joins two 32-bit register halves into one 64-bit guest address |
| `set_error()` | records ERROR plus a numeric reason in STATUS |
| `clear_error()` | removes the previous command's error while keeping READY |
| `vlog_device_init()` | connects the device to Part I and establishes initial register state |
| `cmd_log()` | validates a guest message, appends it to the host log, then updates counters |
| `cmd_stat()` | validates a guest output buffer and copies counters into guest RAM |
| `run_command()` | treats a CMD write as a commit and dispatches NOP/LOG/FLUSH/STAT |
| `vlog_device_mmio_read()` | returns the register selected by a guest MMIO read |
| `vlog_device_mmio_write()` | latches operands or executes the command selected by a guest MMIO write |

Part II does not contain a loop that runs independently. Its functions run
synchronously when the Part I virtual CPU touches the device's MMIO page.

## Where the register names and addresses come from

`emulator/device.h` is the protocol shared by guest and device. It defines each
register's name and byte offset. `emulator/vmm.h` defines `DEV_BASE` as
`0x20000000`, the start of the device's MMIO page. CMD offset `0x0c` therefore
means guest address `0x2000000c`; Unicorn passes only `0x0c` to the callback.

MSG is not message data inside a register. It is a 64-bit guest physical address
pointing to bytes already in guest RAM. Because registers are 32 bits, MSG is
split into low/high halves. `guest/vlog.c` obtains it from the guest's C message
pointer; paging is disabled, so that guest virtual address equals its GPA.

This provided guest-side function is where that GPA comes from:

```c
int vlog_write(uint32_t level, const void *msg, uint32_t len)
{
    uint64_t gpa = (uint64_t)(uintptr_t)msg;
    mmio_w32(DEV_BASE + VLOG_REG_MSG_LO, (uint32_t)gpa);
    mmio_w32(DEV_BASE + VLOG_REG_MSG_HI, (uint32_t)(gpa >> 32));
    mmio_w32(DEV_BASE + VLOG_REG_LEN, len);
    mmio_w32(DEV_BASE + VLOG_REG_LEVEL, level);
    mmio_w32(DEV_BASE + VLOG_REG_CMD, VLOG_CMD_LOG);

    uint32_t st = mmio_r32(DEV_BASE + VLOG_REG_STATUS);
    if (st & VLOG_STATUS_ERROR)
        return (int)((st >> 8) & 0xff);
    return 0;
}
```

`msg` is the guest C pointer, `gpa` is its numeric address, and `st` is the
STATUS register value read after the synchronous LOG command finishes.

Unlike the executable image, a log message does not need to begin at
`RAM_BASE`. MSG may identify bytes anywhere inside guest RAM: a string literal,
stack array, global array, or other buffer. `vmm_gpa_to_host()` subtracts
`RAM_BASE` to find the corresponding offset and proves that all LEN bytes fit.

```text
example MSG GPA = RAM_BASE + 0x3500
host backing     = v->ram + 0x3500
```

The executable has a fixed start because RIP needs a defined entry point. A
message is ordinary data, so its address depends on where the compiler, linker,
stack, or guest code placed that particular buffer.

## Register map

Registers are 32-bit. Guest addresses are `DEV_BASE + offset`; callbacks receive
only the offset.

| Offset | Name | Direction | Meaning |
|---:|---|---|---|
| `0x00` | ID | device -> guest | magic `VLG1` |
| `0x04` | VERSION | device -> guest | protocol version 1 |
| `0x08` | STATUS | device -> guest | READY bit, ERROR bit, numeric error code |
| `0x0c` | CMD | guest -> device | execute command |
| `0x10` | MSG_LO | both | low 32 address bits |
| `0x14` | MSG_HI | both | high 32 address bits |
| `0x18` | LEN | both | message/output-buffer byte count |
| `0x1c` | LEVEL | both | numeric DEBUG/INFO/WARN/ERROR severity |
| `0x20` | SEQ | device -> guest | successful LOG count |

“Latched” means operand values remain stored in `struct vlog_device` across
commands. Writing CMD is the commit point: it synchronously uses the currently
latched operands.

The symbolic command and level names are numbers shared by guest and device:

| Kind | Numeric values |
|---|---|
| Commands | `NOP=0`, `LOG=1`, `FLUSH=2`, `STAT=3` |
| Levels | `DEBUG=0`, `INFO=1`, `WARN=2`, `ERROR=3` |

STATUS is a packed 32-bit result: bit 0 is READY, bit 2 is ERROR, and the error
code occupies bits 8 through 15. Names such as `BADLEN` make those numeric codes
readable in C; the Errors table below explains when each one is produced.

## Device state and helpers

`dev` is the local pointer to `struct vlog_device`. The structure holds the VMM
back-pointer, status, four operands, sequence, and total message bytes.
`msg_addr()` casts before shifting the high half by 32, then ORs the halves.
`set_error()` ORs ERROR/code bits. `clear_error()` clears both, preserving READY.

```c
static inline uint64_t msg_addr(const struct vlog_device *dev)
{
    return (uint64_t)dev->msg_addr_lo |
           ((uint64_t)dev->msg_addr_hi << 32);
}

static inline void set_error(struct vlog_device *dev, uint32_t code)
{
    dev->status |= VLOG_STATUS_ERROR | (code << VLOG_STATUS_ERR_SHIFT);
}

static inline void clear_error(struct vlog_device *dev)
{
    dev->status &= ~(VLOG_STATUS_ERROR |
                     (0xffu << VLOG_STATUS_ERR_SHIFT));
}
```

This implementation factors command logic into `cmd_log()`, `cmd_stat()`, and
`run_command()`. That keeps register dispatch small and makes validation paths
easier to explain/test.

The structure itself was provided in `emulator/device.h`:

```c
struct vlog_device {
    struct vmm *vmm;       /* connection back to Part I */
    uint32_t    status;
    uint32_t    msg_addr_lo;
    uint32_t    msg_addr_hi;
    uint32_t    len;
    uint32_t    level;
    uint32_t    seq;
    uint32_t    bytes;
};
```

The `vmm` field is what makes Part II dependent on Part I: it leads to Part I's
address translator and to the provided log store owned by the VMM.

## `vlog_device_init()`

`vlog_device_init(dev, vmm)` runs once during `vmm_create()`. It connects the
device to its owning VMM, marks STATUS as READY, clears the address, length, and
counters, and chooses INFO as the initial level. That starting level matters if
the guest issues LOG before writing LEVEL; the other operand registers likewise
retain their initialized or most recently written values.

```c
void vlog_device_init(struct vlog_device *dev, struct vmm *vmm)
{
    dev->vmm         = vmm;
    dev->status      = VLOG_STATUS_READY;
    dev->msg_addr_lo = 0;
    dev->msg_addr_hi = 0;
    dev->len         = 0;
    dev->level       = VLOG_LVL_INFO;
    dev->seq         = 0;
    dev->bytes       = 0;
}
```

## `cmd_log()`

`cmd_log(dev)` processes the current fields in `dev`:

1. If LEN exceeds 4096, set BADLEN and return.
2. For LEN zero, leave `msg=NULL`; the address is deliberately ignored.
3. Otherwise translate the entire MSG/LEN range with `vmm_gpa_to_host()`.
4. If translation fails, set BADADDR and return.
5. Append with the current sequence and level.
6. Increment sequence and add LEN to the byte counter.

Validation occurs before the file side effect or counters. Unknown levels are
not errors; `logstore` formats them as `LVL?`. A zero-length message still
creates `[seq] LEVEL \n`; `logstore_append()` does not dereference NULL when LEN
is zero.

```text
guest writes MSG_LO/MSG_HI/LEN/LEVEL
 -> guest writes CMD=LOG
 -> run_command clears old error
 -> cmd_log checks length
 -> translates guest address to host pointer
 -> logstore_append
 -> seq++, bytes += len
```

```c
static void cmd_log(struct vlog_device *dev)
{
    if (dev->len > VLOG_MAX_MSG) {
        set_error(dev, VLOG_ERR_BADLEN);
        return;
    }

    const void *msg = NULL;
    if (dev->len > 0) {
        msg = vmm_gpa_to_host(dev->vmm, msg_addr(dev), dev->len);
        if (msg == NULL) {
            set_error(dev, VLOG_ERR_BADADDR);
            return;
        }
    }

    logstore_append(dev->vmm->store, dev->seq, dev->level, msg, dev->len);
    dev->seq++;
    dev->bytes += dev->len;
}
```

`logstore_append()` is provided rather than implemented by us. This is the
relevant body from `emulator/logstore.c`; it explains why `cmd_log()` passes a
sequence, level, raw byte pointer, and length:

```c
void logstore_append(logstore *ls, uint32_t seq, uint32_t level,
                     const void *bytes, uint32_t len)
{
    pthread_mutex_lock(&ls->lock);
    fprintf(ls->f, "[%u] %s ", seq, level_name(level));
    if (len)
        fwrite(bytes, 1, len, ls->f);
    fputc('\n', ls->f);
    fflush(ls->f);
    pthread_mutex_unlock(&ls->lock);
}
```

The mutex prevents two host threads from interleaving a record. `level_name()`
converts the numeric level to text, and `fwrite()` copies exactly `len` message
bytes without requiring a terminating zero byte.

## `cmd_stat()`

LOG reads guest RAM into the host. STAT flows in the opposite direction: it
writes two host-side device counters into a guest buffer.

1. Require LEN at least `sizeof(struct vlog_stats)` (8 bytes).
2. Translate exactly 8 bytes at MSG.
3. Build a local `{records=seq, bytes=bytes}` value.
4. Copy it into guest RAM with `memcpy()`.

Why `memcpy` rather than `*(struct vlog_stats *)out = stats`? The guest may give
an address that is byte-valid but not aligned for `struct vlog_stats`; typed
access to an unaligned pointer is undefined on some C targets. `memcpy` safely
handles unaligned byte addresses.

The device translates eight bytes, not the full offered LEN, because eight is
all it writes. LEN proves the guest offered enough logical capacity. STAT does
not log or change counters.

```c
static void cmd_stat(struct vlog_device *dev)
{
    if (dev->len < sizeof(struct vlog_stats)) {
        set_error(dev, VLOG_ERR_BADLEN);
        return;
    }

    void *out = vmm_gpa_to_host(dev->vmm, msg_addr(dev),
                                sizeof(struct vlog_stats));
    if (out == NULL) {
        set_error(dev, VLOG_ERR_BADADDR);
        return;
    }

    struct vlog_stats stats = {
        .records = dev->seq,
        .bytes = dev->bytes
    };
    memcpy(out, &stats, sizeof stats);
}
```

## `run_command()`

It clears the previous error first, then dispatches:

- NOP: no effect beyond clearing error.
- LOG: call `cmd_log()`.
- FLUSH: flush the host store.
- STAT: call `cmd_stat()`.
- unknown: set BADCMD.

Clearing first is necessary because `set_error()` uses bitwise OR. It also makes
a later successful command recover from an earlier failure.

```c
static void run_command(struct vlog_device *dev, uint32_t cmd)
{
    clear_error(dev);

    switch (cmd) {
    case VLOG_CMD_NOP:   break;
    case VLOG_CMD_LOG:   cmd_log(dev); break;
    case VLOG_CMD_FLUSH: logstore_flush(dev->vmm->store); break;
    case VLOG_CMD_STAT:  cmd_stat(dev); break;
    default:             set_error(dev, VLOG_ERR_BADCMD); break;
    }
}
```

FLUSH uses this provided helper. It forces buffered host-file output to be
written without adding a record:

```c
void logstore_flush(logstore *ls)
{
    pthread_mutex_lock(&ls->lock);
    fflush(ls->f);
    pthread_mutex_unlock(&ls->lock);
}
```

## MMIO callbacks

`uc` is Unicorn's engine parameter, `offset` selects the register, `size` is the
access width, and `user_data` is the registered device pointer.
`vlog_device_mmio_read()` switches on the offset and returns constants/state.
CMD and unknown offsets read as zero.

`vlog_device_mmio_write()` truncates the callback's 64-bit value to `value32`,
then switches. Operand writes update state; CMD invokes `run_command`; writes to
read-only/unknown offsets do nothing. The assignment only requires aligned
32-bit accesses, so `size` is unused.

```c
uint64_t vlog_device_mmio_read(uc_engine *uc, uint64_t offset,
                               unsigned size, void *user_data)
{
    (void)uc; (void)size;
    struct vlog_device *dev = user_data;

    switch (offset) {
    case VLOG_REG_ID:      return VLOG_MAGIC;
    case VLOG_REG_VERSION: return VLOG_VERSION;
    case VLOG_REG_STATUS:  return dev->status;
    case VLOG_REG_MSG_LO:  return dev->msg_addr_lo;
    case VLOG_REG_MSG_HI:  return dev->msg_addr_hi;
    case VLOG_REG_LEN:     return dev->len;
    case VLOG_REG_LEVEL:   return dev->level;
    case VLOG_REG_SEQ:     return dev->seq;
    default:               return 0;
    }
}

void vlog_device_mmio_write(uc_engine *uc, uint64_t offset,
                            unsigned size, uint64_t value, void *user_data)
{
    (void)uc; (void)size;
    struct vlog_device *dev = user_data;
    uint32_t value32 = (uint32_t)value;

    switch (offset) {
    case VLOG_REG_MSG_LO: dev->msg_addr_lo = value32; break;
    case VLOG_REG_MSG_HI: dev->msg_addr_hi = value32; break;
    case VLOG_REG_LEN:    dev->len = value32; break;
    case VLOG_REG_LEVEL:  dev->level = value32; break;
    case VLOG_REG_CMD:    run_command(dev, value32); break;
    default:              break;
    }
}
```

## Errors

| Input problem | Status result | Side effects |
|---|---|---|
| command outside 0–3 | BADCMD | none |
| LOG LEN >4096 | BADLEN | no record/counter change |
| nonempty LOG range outside RAM | BADADDR | no record/counter change |
| STAT LEN <8 | BADLEN | no guest write |
| STAT output range outside RAM | BADADDR | no guest write |

## Does a bad Part II command stop the VM?

No. This differs from an unmapped CPU access handled by `mem_invalid()`. A bad
LOG/STAT request reaches mapped device registers normally; the device rejects
the command by setting STATUS and performs no unsafe side effect. The MMIO
callback returns, Unicorn resumes at the guest's next instruction, and the
guest can read STATUS or issue a later valid command. `run_command()` clears the
old error first so recovery is possible.

## Interview answer

“The device is a synchronous register protocol for moving log bytes across the
guest/host boundary. Operand registers latch a 64-bit GPA, length, and level;
CMD clears the old result and dispatches. LOG validates before translating and
reading guest RAM, then updates counters only after append. STAT validates a
guest output buffer and uses memcpy so unaligned buffers are safe. Unknown
register writes are ignored and errors are reported through STATUS.”
