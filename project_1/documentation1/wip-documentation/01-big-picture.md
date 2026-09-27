# 1. Big Picture and Why This Project Exists

Read [Chapter 0](00-virtualization-fundamentals.md) first. This chapter applies
those fundamentals to the assignment without assuming you already know its C
names or register layout.

## The problem we are solving

We want code running inside an isolated virtual machine to send diagnostic
messages to a file owned by the host. For example, the guest might report:

```text
INFO function runner started
WARN cache miss
ERROR request failed
```

The guest cannot simply call host `fprintf()`: it is a different machine/address
space. We need a controlled communication path—a virtual logging device.

## Why logging?

Logging is a small, understandable workload that demonstrates real device ideas:
the guest submits data, the device validates guest-provided addresses, the host
performs an external side effect, and the device reports completion. The same
ideas appear in virtual disks (guest buffers contain blocks) and virtual network
cards (guest buffers contain packets).

## Are all three parts “just logging”?

Yes, all three parts contribute to getting guest log messages into a host file,
but they teach different layers. Part I does not implement the logging command;
it builds the small virtual computer that Part II's logger needs. Part II uses a
custom device protocol that only our bare-metal guest understands. Part III
performs the same end result—guest bytes reach a host log—but through Linux's
standard virtio console protocol in a completely separate QEMU VM.

```text
Part I:   create a machine capable of running our tiny guest
Part II:  add our private logging interface to that machine
Part III: implement a standard Linux-compatible logging path under QEMU
```

The final log format is shared, but the submission path is different:

```text
Parts I/II: guest writes custom device registers → `device.c`
Part III:   Linux publishes virtqueue descriptors → `virtio.c`
```

## The three parts, one at a time

## Where the code for each part lives

All paths below are relative to `project_1/`:

| Part | Code we implemented | Important provided code it connects to |
|---|---|---|
| Part I | `emulator/vmm.c` | `emulator/vmm.h`, `emulator/main.c`, Unicorn, `guest/start.S` |
| Part II | `emulator/device.c` | `emulator/device.h`, `guest/vlog.c`, `guest/vlog.h`, `emulator/logstore.c` |
| Part III | `vhost/virtio.c` | `vhost/virtq.h`, `vhost/backend.c`, `vhost/sink.c`, `vhost/run-qemu.sh` |

Some files are deliberately mixed: `vmm.c` contained provided scaffolding and
student TODOs. Chapters 4, 5, and 7 label the exact boundary and show the code.

### Part I: build a tiny computer

`emulator/vmm.c` creates one x86-64 virtual CPU through Unicorn, allocates 16 MiB
of guest RAM, defines virtual serial and logging-device address ranges, loads a
bare-metal guest binary, and runs it. Think of Part I as building the computer,
not yet defining all of the logging device's behavior.

Part II depends directly on Part I. Part I creates `v->dev`, maps the device's
MMIO page, and passes `v->dev` to the callbacks implemented in Part II:

```c
v->dev = calloc(1, sizeof *v->dev);
vlog_device_init(v->dev, v);

err = uc_mmio_map(v->uc, DEV_BASE, DEV_SIZE,
                  vlog_device_mmio_read, v->dev,
                  vlog_device_mmio_write, v->dev);
```

Without those Part I lines, the guest's Part II register accesses would have no
device attached to receive them. Conversely, Part I can create the machine, but
the logger does nothing useful until Part II supplies the callbacks.

### Part II: attach our own simple logging device

`emulator/device.c` implements our private logging protocol. A **device register**
is a small named value exposed through MMIO. The guest writes four kinds of
information:

- `MSG`: the guest address where the message bytes already live;
- `LEN`: how many bytes belong to the message;
- `LEVEL`: its severity, such as INFO or WARN;
- `CMD`: which operation to perform, such as LOG.

These names and numeric offsets come from `emulator/device.h`, the shared
guest/device contract. `MSG` is split into `MSG_LO` and `MSG_HI` because each
register is 32 bits while an address is 64 bits.

The device's MMIO region begins at `DEV_BASE`, a constant from `emulator/vmm.h`
whose value is `0x20000000`. For example, the command register is at offset
`0x0c`, so the guest accesses address `DEV_BASE + 0x0c = 0x2000000c`. Unicorn
passes the relative offset `0x0c` to the device callback.

The GPA—guest physical address—comes from the guest's ordinary message pointer.
Paging is disabled, so that guest pointer has the same numeric value as its GPA.
The guest library `guest/vlog.c` converts the pointer to 64 bits and writes its
low/high halves to the MSG registers. The host then validates and translates
that GPA before reading the bytes.

### Part III: replace our private protocol with a standard one

`vhost/virtio.c` does not run under our Unicorn VMM and does not use MSG/LEN/CMD
registers. QEMU runs a real Linux guest. A Linux program writes to `/dev/hvc0`,
Linux's existing virtio console driver describes the message buffers in a
virtqueue, and our external backend reads those descriptions.

A **descriptor** is the virtio equivalent of saying “the bytes start at this GPA
and have this length.” A **ring** is shared metadata used to submit and complete
those descriptors. Our handler validates the same kind of untrusted guest
address, collects the bytes, sends them to a provided log **sink** (a helper that
formats/appends a record), and marks the request completed.

Part III therefore reuses the goal and memory-safety principle, not the Part II
register protocol or Unicorn machine.

## Architecture diagram

```text
PARTS I + II

 bare-metal x86-64 guest (no guest OS)
   |  prints                         |  logs
   |  store to serial MMIO           |  write address/length/level/command
   v                                 v
 Unicorn CPU ----------------> callbacks in host emulator
                                      |
                                      | validate GPA and translate it
                                      v
                              host-backed guest RAM
                                      |
                                      v
                                  logstore -> file

PART III (a separate execution path)

 Linux application: write("/dev/hvc0", bytes)
   -> Linux virtio_console driver
   -> driver describes guest buffers and publishes work
   -> kick
   -> QEMU/vhost-user plumbing
   -> vlog_virtq_handle()
        -> validate/translate guest buffer addresses
        -> combine message pieces
        -> logging helper -> host file
        -> report completion to Linux
   -> Linux driver reuses the returned buffers
```

## What connects the parts

Parts I and II run in the same `emulator` process. `struct vmm` is the main host
state object: `v->ram` points to the host allocation backing guest RAM and
`v->store` points to the host log manager. The logging device stores a back
pointer `dev->vmm` so its callback can reach both.

Part III is separate from that VMM. It shares the provided log-record code and
the conceptual pattern, but QEMU supplies mappings for Linux guest RAM and Linux
submits requests with virtqueues rather than our private registers.

| Question | Parts I/II answer | Part III answer |
|---|---|---|
| Who runs the guest CPU? | Unicorn | QEMU, optionally accelerated by KVM |
| What guest runs? | tiny bare-metal binary | real Linux kernel + tiny init |
| How is work submitted? | write the CMD device register | publish a chain head and notify the device |
| Where are data locations described? | MSG address and LEN registers | virtio descriptors |
| How is address translated? | `vmm_gpa_to_host()` | `virtq_gpa_to_hva()` |
| How is completion reported? | STATUS says success/error; SEQ counts logs | used ring returns buffers; interrupt alerts Linux |
| Who writes the final record? | provided `logstore_append()` | provided sink parses console bytes, then uses logstore |

## The same message through both logging paths

Suppose the message is `cache miss` at WARN level.

In Parts I/II, our guest library finds the string's guest pointer, writes that
address into MSG_LO/HI, writes LEN=10 and LEVEL=WARN, then writes CMD=LOG. Our
MMIO callback translates the pointer and calls the log store.

In Part III, a Linux program writes a wire-format line such as
`2 cache miss\n` to `/dev/hvc0`. Linux decides where those bytes live in its RAM
and puts the address/length into one or more virtio descriptors. Our queue
handler joins the descriptor bytes and the sink interprets leading `2 ` as WARN.

Both ultimately produce a record such as `[0] WARN cache miss`, but the guest,
submission protocol, VM software, and completion mechanism differ.

## One sentence to remember

This project first teaches us to build a tiny VM and a custom host/guest device
protocol, then makes us implement the equivalent data path using the real virtio
queue protocol that Linux already understands.
