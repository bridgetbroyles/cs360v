# 4. Part I — The Unicorn VMM (`emulator/vmm.c`)

Prerequisites: Chapter 0 explains VMM, bare metal, Unicorn, and MMIO; Chapter 3
explains GVA/GPA/HVA. This chapter maps those ideas to exact C state.

## Purpose

This file creates the tiny computer, loads a flat guest image (raw machine-code
bytes rather than a normal OS-loaded executable), runs it,
and mediates its interactions with host memory and devices.

## Exact source locations

All paths are relative to `project_1/`:

| File | Role | Ownership for this assignment |
|---|---|---|
| `emulator/vmm.c` | Part I VMM implementation | mixed provided scaffolding and our implementations |
| `emulator/vmm.h` | memory map, constants, `struct vmm`, function declarations | provided |
| `emulator/main.c` | parses arguments and calls the VMM lifecycle | provided |
| `emulator/logstore.c` | opens, formats, flushes, and closes host logs | provided |
| `guest/start.S` | first bare-metal guest instructions | provided |
| `guest/vlog.c` | guest helpers for serial, exit, and Part II logging | provided |

“Provided” means it existed in the starter branch. “Our implementation” means
the starter had a TODO or stub that this branch filled in.

## What we implemented versus what was given

| Area in `vmm.c` | Ownership |
|---|---|
| `serial_read()` | provided; serial reads return zero |
| `serial_write()` | our implementation |
| `mem_invalid()` | our implementation |
| `trace_code()` | provided |
| opening logstore and Unicorn in `vmm_create()` | provided scaffolding |
| RAM allocation/mapping, both MMIO mappings, RSP, fault hook | our implementation |
| device allocation/init, RDI, optional trace hook | provided scaffolding |
| `vmm_load_binary()` | our implementation |
| `vmm_load_bootinfo()` | provided |
| `vmm_run()` | our implementation |
| `vmm_destroy()` | provided |
| `vmm_gpa_to_host()` | our implementation |

## Part I method map: what each function does at a high level

| Function | High-level job | Does it run guest code? |
|---|---|---|
| `serial_read()` | models a serial device with no readable registers | no; called by Unicorn during a guest read |
| `serial_write()` | turns guest stores into console output or clean shutdown | no; called by Unicorn during a guest write |
| `mem_invalid()` | records an unmapped guest access and ends this VM run as a fault | no; called by Unicorn after it detects an invalid access |
| `trace_code()` | prints the address and size of each instruction for debugging | no; observes guest execution |
| `vmm_create()` | constructs the virtual machine and connects RAM/devices/callbacks | no |
| `vmm_load_binary()` | copies one flat guest image into RAM and chooses its first instruction | no |
| `vmm_load_bootinfo()` | copies optional startup data into reserved guest RAM | no |
| `vmm_run()` | enters Unicorn and lets the virtual CPU execute until stop/fault/error | yes |
| `vmm_gpa_to_host()` | validates a guest RAM range and returns its host backing pointer | no |
| `vmm_destroy()` | releases all host resources owned by the VMM | no |

The normal lifecycle is:

```text
host main
 -> vmm_create
 -> vmm_load_binary
 -> optional vmm_load_bootinfo
 -> vmm_run
      -> guest executes and triggers callbacks
      -> poweroff or fault stops Unicorn
 -> vmm_destroy
 -> host process exits with vmm_run's result
```

## Boot info is not a boot device

`BOOTINFO_BASE` is a constant, not a function or device. It is the address of a
reserved 64-KiB slice at the top of ordinary guest RAM:

```c
#define BOOTINFO_SIZE 0x10000ULL
#define BOOTINFO_BASE (RAM_BASE + RAM_SIZE - BOOTINFO_SIZE)
```

If the emulator is run with `--bootinfo file`, the provided loader copies that
file's bytes into this RAM slice. RDI points there when the guest starts. The
VMM does not emulate boot hardware and does not interpret the bytes. In this
project, “boot info” just means optional startup data handed to the guest.

`struct vmm` is the central host-side state object. `v` is the conventional local
name for a pointer to it. Its important fields are `uc` (Unicorn engine), `ram`
(host backing for guest RAM), `dev` (logging-device state), `store` (host log
manager), and termination fields.

This provided definition in `emulator/vmm.h` is the object shared by Part I and
Part II:

```c
struct vmm {
    uc_engine          *uc;
    uint8_t            *ram;
    struct vlog_device *dev;
    logstore           *store;
    int                 trace;
    int                 powered_off;
    int                 exit_code;
    int                 faulted;
    uint64_t            fault_addr;
};
```

Part I fills most of these fields. Part II reaches `ram` and `store` through the
back-pointer stored in `dev`.

The address and register names used below are constants from `emulator/vmm.h`:

| Name | Meaning |
|---|---|
| `RAM_BASE` | first guest RAM address and guest entry address |
| `BOOTINFO_BASE` | guest address of the small boot-information structure |
| `SERIAL_TX` | offset of the serial output register within the serial MMIO page |
| `SERIAL_POWEROFF` | offset of the serial shutdown register |
| `VMM_EXIT_FAULT` | host exit code 77 for an invalid guest-memory access |

The reserved boot-information region can hold up to 64 KiB of startup data
supplied to the guest. This guest barely uses it, but placing its address in RDI
follows the machine's defined startup convention.

## `serial_read()` and `trace_code()`

The serial device has nothing useful to read, so `serial_read()` always returns
zero. It exists because Unicorn's MMIO mapping accepts both a read callback and
a write callback.

When the user passes `--trace`, Unicorn calls `trace_code()` before each guest
instruction. It prints the guest instruction address and size to host stderr.
This is diagnostic output only; it does not change guest state.

```c
static inline uint64_t serial_read(uc_engine *uc, uint64_t offset,
                                   unsigned size, void *user_data)
{
    (void)uc; (void)offset; (void)size; (void)user_data;
    return 0;
}

static void trace_code(uc_engine *uc, uint64_t address,
                       uint32_t size, void *user_data)
{
    (void)uc; (void)user_data;
    fprintf(stderr, "[trace] rip=0x%08llx (%u bytes)\n",
            (unsigned long long)address, size);
}
```

## `serial_write()`

**Purpose:** implement output and shutdown writes. `uc` is the Unicorn engine;
`offset` selects a register inside the serial page; `size` is the access width;
`value` is what the guest wrote; and `user_data` is the context pointer registered
by `vmm_create()`. It casts that context to `struct vmm *`.

Control flow:

- `SERIAL_TX`: mask `value & 0xff`, cast to int, and `putchar()` it.
- `SERIAL_POWEROFF`: store `(int)value`, set `powered_off=1`, stop Unicorn.
- anything else: ignore.

Masking explicitly selects the low byte. State is written before stopping so
`vmm_run()` knows the stop was intentional. Without `uc_emu_stop()`, guest
`vm_exit()` loops forever. An alternative is buffered output, but direct output
matches the synchronous teaching device.

The offset chooses the register; the value supplies that register's data. The
low-byte mask does not choose between TX and POWEROFF:

```text
guest writes address SERIAL_BASE + 0, value 0x00000141
 -> callback offset is 0, so this is SERIAL_TX
 -> 0x141 & 0xff = 0x41
 -> host prints 'A'

guest writes address SERIAL_BASE + 8, value 5
 -> callback offset is 8, so this is SERIAL_POWEROFF
 -> host records exit code 5 and stops guest execution
```

```c
static inline void serial_write(uc_engine *uc, uint64_t offset,
                                unsigned size, uint64_t value, void *user_data)
{
    (void)size;
    struct vmm *v = user_data;

    if (offset == SERIAL_TX) {
        putchar((int)(value & 0xff));
    } else if (offset == SERIAL_POWEROFF) {
        v->exit_code = (int)value;
        v->powered_off = 1;
        uc_emu_stop(uc);
    }
}
```

**Interview answer:** “Unicorn routes stores in the serial page here. Offset zero
prints the low byte; offset eight records a clean guest exit and stops emulation.
The callback receives a relative offset, so it must not subtract SERIAL_BASE.”

## `mem_invalid()`

**Purpose:** contain guest reads, writes, or instruction fetches outside mapped
RAM/MMIO. It records `faulted` and `fault_addr`, prints a diagnostic to stderr,
calls `uc_emu_stop()`, and returns false so Unicorn does not retry.

The access type, size, and value are unused because every unmapped access has the
same policy. stderr matters because stdout is the guest's tested console stream.
An alternative VMM might allocate a page and return true, but this machine has a
fixed physical map.

```c
static inline bool mem_invalid(uc_engine *uc, uc_mem_type type,
                               uint64_t address, int size, int64_t value,
                               void *user_data)
{
    (void)type; (void)size; (void)value;
    struct vmm *v = user_data;

    v->faulted = 1;
    v->fault_addr = address;
    fprintf(stderr, "guest fault: access to unmapped address 0x%llx\n",
            (unsigned long long)address);
    uc_emu_stop(uc);
    return false;
}
```

### Does execution resume after `mem_invalid()`?

No. Unicorn has already determined that the address is outside every mapped RAM
or MMIO region; `mem_invalid()` is a notification and policy callback, not the
code that performs the original bounds check. `uc_emu_stop()` ends the current
`uc_emu_start()` call. Returning `false` tells Unicorn that we did not repair the
mapping and that it must not retry the failed instruction.

This does not stop a physical CPU or terminate a host thread. In this project,
the host thread was synchronously executing inside `uc_emu_start()`; that call
returns to `vmm_run()`. `vmm_run()` sees `v->faulted` and returns 77. Provided
`main()` then calls `vmm_destroy()` and exits. There is no “move on to the next
guest instruction” after a fault.

## `vmm_create()`

`vmm_create(v, trace, log_path)` receives empty VMM storage, a flag controlling
instruction tracing, and a host log filename. **Purpose:** construct a usable VM.
**Returns:** 0 on success and -1 on a checked failure. **Important state:** `uc`,
`ram`, `dev`, `store`, and `trace`.

Actual sequence:

1. Zero all VMM fields and save the trace flag.
2. Open the log store.
3. Create a Unicorn x86-64 engine.
4. Allocate 16 MiB with `calloc`, so initial RAM bytes are zero.
5. Map that buffer at `RAM_BASE` with read/write/execute permission.
6. Map serial MMIO with `v` as callback context.
7. Allocate and initialize `v->dev`.
8. Map logging-device MMIO with `v->dev` as context.
9. Write RSP (stack pointer) just below boot info and RDI (first argument) to
   boot-info base.
10. Register the unmapped-memory hook for all addresses.
11. Optionally register the provided instruction trace hook over RAM.

Why host-backed RAM? Unicorn instruction execution and device host code see the
same allocation; device access becomes checked pointer arithmetic rather than a
copying API.

This implementation checks allocation/mapping/hook errors but does not check the
return values of the RSP/RDI writes or trace-hook registration. It also returns
directly from most setup failures without unwinding already acquired resources;
the process soon exits through `main`, so tests pass, but a reusable library
would use one cleanup path.

**30-second answer:** “`vmm_create` opens the sink and Unicorn CPU, allocates and
maps host-backed RAM, maps two MMIO devices with the appropriate contexts,
initializes stack and boot-argument registers, installs the fault hook, and
optionally installs tracing.”

The central wiring in the real function looks like this. The comments mark the
handoff from Part I to Part II:

```c
uc_err err;

v->ram = calloc(1, RAM_SIZE);
err = uc_mem_map_ptr(v->uc, RAM_BASE, RAM_SIZE, UC_PROT_ALL, v->ram);

err = uc_mmio_map(v->uc, SERIAL_BASE, SERIAL_SIZE,
                  serial_read, v, serial_write, v);

/* Part I creates the Part II device object. */
v->dev = calloc(1, sizeof *v->dev);
vlog_device_init(v->dev, v);

/* Part I connects guest addresses to Part II's callback functions. */
err = uc_mmio_map(v->uc, DEV_BASE, DEV_SIZE,
                  vlog_device_mmio_read, v->dev,
                  vlog_device_mmio_write, v->dev);

uint64_t rsp = BOOTINFO_BASE - 16;
uc_reg_write(v->uc, UC_X86_REG_RSP, &rsp);

uint64_t rdi = BOOTINFO_BASE;       /* provided startup convention */
uc_reg_write(v->uc, UC_X86_REG_RDI, &rdi);

uc_hook fault_hook;
err = uc_hook_add(v->uc, &fault_hook, UC_HOOK_MEM_UNMAPPED,
                  mem_invalid, v, 1, 0);
```

This excerpt omits repeated error-reporting branches so the connection is
visible; the actual `emulator/vmm.c` checks allocation and mapping failures.

## `vmm_load_binary()`

`vmm_load_binary(v, path)` receives the VMM and a host filesystem path. It opens
the flat file, seeks to measure it, rejects negative size or anything
over RAM size, rewinds, reads exactly that many bytes into `v->ram`, then sets
RIP to RAM base. There is no ELF parsing: the linker/objcopy already produced a
flat image whose byte zero is the entry point.

The function checks `ftell` and short reads, but this implementation does not
check `fseek` or `uc_reg_write` return codes. A more defensive version would.

```c
int vmm_load_binary(struct vmm *v, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0 || (uint64_t)file_size > RAM_SIZE) {
        fclose(f);
        return -1;
    }

    size_t n = fread(v->ram, 1, (size_t)file_size, f);
    fclose(f);
    if (n != (size_t)file_size)
        return -1;

    uint64_t rip = RAM_BASE;
    uc_reg_write(v->uc, UC_X86_REG_RIP, &rip);
    return 0;
}
```

### Offset zero, not offset one

Arrays and C buffers begin at offset zero. Passing `v->ram` to `fread()` is the
same destination as `v->ram + 0`. Part I established this mapping earlier:

```text
host v->ram + 0       <-> guest RAM_BASE + 0
host v->ram + 1       <-> guest RAM_BASE + 1
host v->ram + 0x2000  <-> guest RAM_BASE + 0x2000
```

The `fseek(..., SEEK_END)` call does not write at the end of RAM. It temporarily
moves the input file's cursor to its end so `ftell()` can measure the file, then
`fseek(..., SEEK_SET)` rewinds the cursor before `fread()`.

### Can this VMM load more than one binary?

The current command-line path loads exactly one flat guest image at a time. That
single image can contain many functions plus read-only data, globals, and BSS;
the linker combines them into one binary. Calling `vmm_load_binary()` again
would overwrite bytes beginning at `v->ram + 0`, not add a second independent
program.

A different VMM could load several images at different guest addresses, but it
would need a format or policy describing each load address and entry point. Our
flat-image ABI deliberately fixes all three pieces to agree:

```text
link.ld places _start at RAM_BASE
fread places binary byte 0 at v->ram + 0, mapped as RAM_BASE
RIP begins at RAM_BASE
```

## `vmm_load_bootinfo()`

`vmm_load_bootinfo(v, path)` is optional setup used when the command line has
`--bootinfo`. It reads at most 64 KiB from a host file and copies those opaque
bytes into the reserved guest range beginning at `BOOTINFO_BASE`. “Opaque” means
the VMM does not interpret the bytes; the test harness and guest decide their
format. RDI already points to this region, so guest startup code can find it.

```c
int vmm_load_bootinfo(struct vmm *v, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (uint64_t)sz > BOOTINFO_SIZE) {
        fclose(f);
        return -1;
    }

    size_t n = fread(v->ram + (BOOTINFO_BASE - RAM_BASE),
                     1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz)
        return -1;
    return 0;
}
```

Here `BOOTINFO_BASE - RAM_BASE` converts the guest address into an offset inside
the host allocation `v->ram`. The full checked function is in `emulator/vmm.c`.

## `vmm_run()`

`vmm_run(v)` starts the created and loaded machine. It calls
`uc_emu_start(v->uc, RAM_BASE, 0, 0, 0)`, meaning start at RAM base with no
ending address, timeout, or instruction cap. If `faulted`, it returns
`VMM_EXIT_FAULT` (77) before considering Unicorn's error. If Unicorn errors and
the VM did not power off, it prints and returns 1. Otherwise it returns
`v->exit_code`.

Fault-first ordering matters because rejecting an unmapped access can also make
Unicorn report an error; 77 is the specified semantic result.

```c
int vmm_run(struct vmm *v)
{
    uc_err err = uc_emu_start(v->uc, RAM_BASE, 0, 0, 0);

    if (v->faulted)
        return VMM_EXIT_FAULT;
    if (err && !v->powered_off) {
        fprintf(stderr, "uc_emu_start: %s\n", uc_strerror(err));
        return 1;
    }
    return v->exit_code;
}
```

## `vmm_gpa_to_host()`

`vmm_gpa_to_host(v, gpa, len)` receives a VMM, guest physical start address, and
unsigned byte length. It rejects GPAs below RAM, computes an offset, rejects an
offset at or beyond RAM's end, and rejects a length larger than the remaining
bytes. It then returns `v->ram + offset`. The order prevents subtraction
underflow and range overflow.

```c
void *vmm_gpa_to_host(struct vmm *v, uint64_t gpa, uint64_t len)
{
    if (gpa < RAM_BASE)
        return NULL;
    uint64_t offset = gpa - RAM_BASE;
    if (offset >= RAM_SIZE)
        return NULL;
    if (len > RAM_SIZE - offset)
        return NULL;
    return v->ram + offset;
}
```

## How Part II uses Part I

Part II does not allocate or map guest RAM. Its LOG and STAT commands call the
Part I function above through `dev->vmm`:

```c
msg = vmm_gpa_to_host(dev->vmm, msg_addr(dev), dev->len);
```

That is the dependency: Part I owns the machine, RAM, address translator, log
store, and MMIO registration; Part II owns the logging protocol executed when
Unicorn invokes the registered callbacks.

## `vmm_destroy()`

`vmm_destroy(v)` releases host resources after execution: it closes the Unicorn
engine and log store, frees RAM and device allocations, and clears their
pointers. It checks each optional resource before closing it, so it is safe when
some fields are still NULL.

```c
void vmm_destroy(struct vmm *v)
{
    if (v->uc) uc_close(v->uc);
    if (v->store) logstore_close(v->store);
    free(v->ram);
    free(v->dev);
    v->uc = NULL;
    v->store = NULL;
    v->ram = NULL;
    v->dev = NULL;
}
```

**2-minute interview answer:** creation establishes a CPU and shared memory map;
the loaders place executable and optional startup bytes; MMIO callbacks provide
guest output and control; the fault callback safely terminates invalid
execution; the run loop classifies termination; the GPA translator protects
host memory; and destruction releases host resources.
