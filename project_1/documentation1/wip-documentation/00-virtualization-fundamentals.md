# 0. Virtualization Fundamentals

> How can code running inside a virtual computer safely send a message such as
> `Hello` to a log file on the real computer?

Parts I and II build the smallest possible virtual computer and invent a simple
answer. Part III keeps the same goal but replaces our teaching interfaces with
QEMU, Linux, virtio, and an external device backend.

```text
build a tiny virtual computer
→ give it virtual devices
→ let the guest submit a message
→ safely locate that message in guest memory
→ write it to a host log
→ part3: repeat the idea with Linux and a standard device protocol
```

## 1. Begin with a normal computer program

A normal Linux application does not control the CPU, memory, disk, or network
hardware by itself. It runs on top of the Linux operating system:

```text
application → Linux kernel → device driver → physical hardware
```

Linux provides `printf`, files, processes, system calls, virtual memory,
networking, and access to hardware. A **device driver** is software that knows
the protocol for a device. A network driver submits packets to a network card;
a disk driver submits reads and writes to a disk.

This matters because the guest in Parts I and II has no Linux kernel. We cannot
assume any of those services exist.

## 2. A virtual machine is a software-created computer

A computer needs a CPU to execute instructions, RAM to store instructions and
data, and devices for input, output, storage, networking, or control. A
**virtual machine (VM)** provides software-created versions of those resources.

- The **host** is the real computer/environment running the virtualization
  software.
- The **guest** is the code or operating system running inside the VM.
- The **virtual machine monitor (VMM)** is host software that creates and
  manages the virtual computer.

```text
host computer
└── VMM / virtualization software
    ├── virtual CPU
    ├── virtual RAM
    ├── virtual devices
    └── guest code
```

A guest may be a tiny program, as in Parts I and II, or a complete Linux system,
as in Part III.

## 3. Parts I and II: our tiny virtual computer

| Role | Concrete component |
|---|---|
| Host | the real computer running the project |
| VMM | our `emulator/emulator` host program |
| CPU engine used by the VMM | Unicorn |
| Guest | our small x86-64 bare-metal program |
| Virtual devices | serial/control and logging devices implemented by us |

```text
host
└── our VMM (`emulator/emulator`)
    ├── Unicorn executes guest CPU instructions
    ├── `v->ram` backs guest RAM
    ├── serial/control callbacks provide output and poweroff
    ├── logging callbacks receive log requests
    └── bare-metal guest executes inside this machine
```

### What Unicorn does—and does not do

The guest contains x86-64 machine instructions. **Unicorn** is a CPU emulation
engine: it implements an x86-64 CPU in software and executes those instructions.

Unicorn is a component of our VMM, not the whole VMM. Our code still decides how
much RAM exists, which addresses mean RAM or devices, which callbacks implement
devices, where execution and the stack begin, and what poweroff or invalid
memory means.

**CPU emulation** executes a target CPU's instructions in software. It is
flexible but slower than direct execution. **Hardware-assisted virtualization**
lets a compatible physical CPU execute guest instructions with hardware
isolation. Linux **KVM** provides this acceleration. Parts I/II do not use KVM.
QEMU in Part III may use KVM, or its software emulator **TCG**. You only need to
recognize KVM and TCG; neither is code we implement.

## 4. What “bare metal” means

The Parts I/II guest is a **bare-metal program**: it runs without a guest
operating system between it and the virtual hardware. It has no guest Linux
providing `printf`, files, processes, system calls, normal virtual memory, or
built-in drivers.

It is built with `-ffreestanding`, linked for a known address, and converted to
a flat binary. `guest/start.S` supplies its initial entry code. The VMM loads the
binary into guest RAM and starts the virtual CPU at its first instruction.

```text
normal application: application → Linux services/drivers → hardware
our guest:          bare-metal guest → virtual hardware → our VMM
```

“Bare metal” does not mean the guest controls the real laptop. It means there is
no OS inside this VM. Code such as `guest/vlog.c` plays a small driver-like role
by knowing our logging registers, but it is not a Linux kernel driver.

## 5. Why the VM needs virtual devices

A CPU and RAM can calculate, but they cannot communicate with the outside world
alone. **I/O** means input/output communication with devices. A **virtual
device** is software-defined hardware presented to a guest.

Parts I/II provide two custom devices:

- The **serial/control device** sends guest text to the host terminal and lets
  the guest request poweroff with an exit code.
- The **logging device** lets the guest identify a message in guest RAM and ask
  the host to append it to a host log.

The logging feature is intentionally simple. Its educational purpose is the
safe communication path. Virtual disks and network cards face the same basic
problem: the guest describes a buffer, and the host must validate it before use.

## 6. MMIO: how the bare-metal guest talks to devices

**Memory-mapped I/O (MMIO)** assigns some guest addresses to devices instead of
ordinary RAM. The guest CPU still uses load/store instructions; the address
determines the behavior.

```text
store to RAM address            → change guest RAM
store to serial MMIO address    → serial callback handles it
store to logging MMIO address   → logging callback handles it
```

Our memory map places RAM at `0x00100000`, serial at `0x10000000`, and the
logging device at `0x20000000`. `vmm_create()` registers those regions with
Unicorn. For MMIO, Unicorn invokes a host callback:

```text
guest store → Unicorn recognizes MMIO → `vlog_device_mmio_write()`
```

Likewise, `serial_write()` handles serial/control writes. These callbacks are
the boundary between guest instructions and host device behavior.

## 7. The central safety problem: guest addresses are not host pointers

The guest and host have different address spaces. If the guest says its message
is at `0x00102000`, that number identifies a virtual-machine location. The host
cannot safely cast it to a C pointer. Guest RAM is a host allocation stored in
`v->ram`.

```text
guest RAM base            0x00100000
guest message address     0x00102000
                         ------------
offset into guest RAM     0x00002000

host address = v->ram + 0x2000
```

This is guest-physical-address to host-virtual-address translation:

```text
guest address → offset in known guest RAM → host backing pointer
```

It is not the full translation performed by an OS. Paging is disabled in the
Parts I/II guest, so guest virtual address equals guest physical address. The
remaining step maps that guest physical address to the host allocation.

The guest controls both address and length. A buffer can begin inside RAM but
extend beyond it. The host must prove the entire range belongs to guest RAM and
must avoid integer overflow during the check. Otherwise a buggy or malicious
guest could read secrets, corrupt the host, or crash it.

> The guest may request access to a buffer, but only the host decides whether
> that address and length really describe guest RAM.

`vmm_gpa_to_host()` enforces this in Parts I/II.

## 8. Part III: move from a teaching VM to real Linux

Parts I/II ask how to build a tiny VM, communicate with custom hardware, and
safely access guest memory. Part III asks how a real Linux guest communicates
with a standard device whose implementation lives outside the VM process.

| Role | Part III component |
|---|---|
| Host | the real computer |
| VM software / VMM | QEMU |
| Guest | real Linux |
| Guest driver | Linux's `virtio_console` driver |
| Virtual hardware | virtio console presented by QEMU |
| Device implementation | our separate `vlog-backend` process |
| Connection to backend | vhost-user |

Part III is a separate path, not an extension of our Unicorn VMM:

```text
Parts I/II: our VMM → Unicorn → bare-metal guest
Part III:   QEMU → Linux guest
```

## 9. QEMU's role

**QEMU** creates and runs a much more complete VM than our teaching VMM. It
provides the virtual CPU, RAM, interrupts, device infrastructure, and environment
needed to boot Linux.

Our backend does not replace QEMU or run Linux. It implements only one device's
behavior.

```text
host
├── QEMU: runs Linux and presents a virtio console
└── our backend: performs that console device's logging work
```

## 10. Driver versus device

The **`virtio_console` driver** is Linux kernel software inside the guest. It
knows the standard virtio console protocol.

The **virtio console device** is the virtual hardware interface Linux believes
exists. QEMU presents that interface; our backend performs its data-processing
behavior.

```text
Linux application
→ writes `/dev/hvc0`
→ Linux `virtio_console` driver
→ virtio console device interface
→ QEMU/vhost-user
→ our backend
```

We do not write or modify the Linux driver. That is the benefit of implementing
a standard interface rather than another private one.

## 11. Virtio: the standard device interface

**Virtio** is a standard family of interfaces for virtual devices. Linux already
supports virtio disks, network devices, consoles, and others.

```text
Parts I/II                         Part III
custom MSG/LEN/CMD registers   →   virtio descriptors and rings
custom `guest/vlog.c`          →   Linux `virtio_console` driver
our Unicorn VMM                →   QEMU
```

The logging stays simple so we can focus on the real device data path.

## 12. Virtqueues describe buffers in guest RAM

A **virtqueue** is shared-memory metadata used by virtio. It usually describes
data rather than containing all the data itself.

If Linux wants to send `Hello`, the bytes may be in guest RAM at `0x5000`. A
virtqueue descriptor says address=`0x5000`, length=5:

```text
virtqueue: “five bytes at guest address 0x5000”
guest RAM:  0x5000 → H e l l o
```

The device translates the address, reads the data, and marks the request
complete. A split virtqueue has a descriptor table, available ring, and used
ring; later chapters explain their exact layouts.

```text
driver publishes work
→ device finds and processes described buffers
→ device reports completion
```

## 13. The backend: where the device's work happens

The **backend** is our separate host-side `vlog-backend` process. It processes
virtqueues, validates and translates guest buffers, reads log bytes, writes the
host log, and completes requests so Linux can reuse buffers.

The distinction is:

```text
virtual device = interface/hardware model visible to Linux
backend        = host process performing the device's behavior
```

`vlog_virtq_handle()` is the central function we implement for that work.

## 14. vhost-user: why the backend can be separate

QEMU could implement the device inside its own process. **vhost-user** instead
connects QEMU's virtio infrastructure to an external device backend. Through the
provided vhost-user code, QEMU tells the backend how guest RAM is mapped, where
queues live, and when work arrives. The backend returns completion information.

QEMU and the backend use a Unix socket—a local communication channel between
host processes—for control/notifications, and shared guest memory for the data
path.

```text
QEMU running Linux
↕ vhost-user control and notifications
our external backend
↕ shared mappings of guest RAM and virtqueues
```

External backends allow device implementations to be developed or isolated from
the main VM process. Here the design teaches shared memory, virtqueues, address
translation, and completion without modifying QEMU.

## 15. Complete Part III data flow

Follow `Hello` from beginning to end:

1. A Linux program writes `Hello` to `/dev/hvc0`.
2. Linux's `virtio_console` driver stores the bytes in guest RAM.
3. The driver builds descriptors containing guest addresses and lengths.
4. It publishes the request and notifies the device.
5. QEMU's vhost-user infrastructure notifies our backend.
6. Our backend reads and validates the virtqueue metadata.
7. It translates guest addresses to host mappings of guest RAM.
8. It reads `Hello` and passes it to the host log sink.
9. It publishes completion in the used ring.
10. Linux learns the request is complete and may reuse the buffer.

```text
Linux application
→ `/dev/hvc0`
→ Linux driver
→ descriptors/virtqueue in guest RAM
→ QEMU and vhost-user
→ our backend validates, translates, and logs
→ used-ring completion returns the buffer to Linux
```

The log is a demonstration. The main lesson is the safe, standardized path from
guest data to a host-side device implementation.

## 16. The whole project side by side

| Question | Parts I and II | Part III |
|---|---|---|
| Who runs the VM? | our VMM | QEMU |
| Who executes instructions? | Unicorn | QEMU using KVM or TCG |
| Guest | bare-metal program | Linux |
| Who knows device protocol? | `guest/vlog.c` | Linux driver |
| Interface | custom MMIO registers | standard virtio console |
| Buffer description | MSG and LEN | virtqueue descriptors |
| Guest memory | one host-backed allocation | QEMU memory regions |
| Device behavior | callbacks inside our VMM | separate backend |
| External connection | none | vhost-user |
| Completion | STATUS and SEQ | used ring and interrupt |

## 17. Do not confuse these roles

```text
Parts I/II
our VMM       = creates and manages the tiny VM
Unicorn       = CPU engine used by our VMM
bare guest    = code running inside it

Part III
QEMU          = creates and manages the Linux VM
Linux         = guest OS, not part of the VMM
Linux driver  = guest software speaking virtio
virtio device = virtual hardware interface visible to Linux
our backend   = host process implementing device behavior
vhost-user    = connection between QEMU and backend
```

## 18. Vocabulary checkpoint

- **Host:** real environment running virtualization.
- **Guest:** code or OS running inside a VM.
- **VM:** software-created computer.
- **VMM:** host software creating/managing a VM.
- **Unicorn:** CPU emulator used by our Parts I/II VMM.
- **QEMU:** VM software running Linux in Part III.
- **KVM:** Linux hardware-virtualization accelerator QEMU may use.
- **TCG:** QEMU's software CPU emulator.
- **Bare metal:** running without a guest OS.
- **I/O:** communication between computer and devices.
- **Virtual device:** software-defined hardware shown to a guest.
- **MMIO:** device behavior triggered by special-address loads/stores.
- **Device driver:** software that knows a device protocol.
- **Virtio:** standard interface for virtual devices.
- **Virtqueue:** shared-memory buffer descriptions and completion metadata.
- **Backend:** host implementation of device behavior.
- **vhost-user:** mechanism connecting QEMU to an external virtio backend.
- **Guest RAM:** memory belonging to the VM's view.
- **Host pointer:** address used by the host process.

## 19. Interview-ready central story

### Parts I and II

“We built a small VMM that uses Unicorn to execute an x86-64 bare-metal guest.
Because the guest has no OS, our VMM provides serial and logging devices, and the
guest uses MMIO to communicate with them. The logging device receives a guest
address and length. Since guest addresses are not host pointers, the VMM validates
the complete range and translates it into the host-backed RAM allocation before
reading the message.”

### Part III

“We use QEMU to run Linux. Linux already has a `virtio_console` driver, so we
implement the standard device data path rather than a custom driver. The driver
uses a virtqueue to describe buffers in guest RAM. vhost-user connects QEMU to
our separate backend, which validates and translates those buffers, writes the
host log, and completes the request so Linux can reuse them.”

### The progression in one line

```text
build a VM → add devices → move data safely across the guest/host boundary
→ replace the teaching protocol with Linux + QEMU + virtio + vhost-user
```
