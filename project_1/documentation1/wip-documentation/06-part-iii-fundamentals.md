# 6. Part III Fundamentals — QEMU, Virtio, and Virtqueues

This chapter expands the Part III story from Chapters 0–1. Part III still logs
guest messages, but it does not use our Unicorn VMM or the MSG/LEN/CMD registers.

## Why Part III is separate

Parts I/II prove we can invent a device protocol, but real Linux does not know
our private registers. Part III asks us to implement a standard protocol that
Linux already knows. It is a separate program and execution path; QEMU replaces
the small Unicorn VMM.

## Where Part III's code lives

All paths are relative to `project_1/`:

| File | What it does | Given or implemented by us? |
|---|---|---|
| `vhost/virtio.c` | translates guest addresses and processes descriptor chains | implemented by us |
| `vhost/virtq.h` | defines the simplified queue interface used by our code | given |
| `vhost/backend.c` | connects QEMU kicks and memory mappings to our handler | given |
| `vhost/sink.c` | turns console bytes into host log records | given |
| `vhost/guest/init.c` | Linux program that writes example records | given |
| `vhost/run-qemu.sh` | starts the backend and QEMU | given |

The exact implementation and complete code excerpts are in Chapter 7.

## Part III at a high level

Part III is event-driven like Part II, but QEMU/Linux provide the surrounding
machine. Our code does not boot Linux or continuously poll the queue. The flow is:

```text
Linux writes bytes
 -> Linux driver publishes descriptor chain
 -> Linux kicks queue
 -> provided backend calls our handler
 -> our handler validates/gathers bytes and publishes completion
 -> provided backend interrupts Linux
 -> Linux may reuse the descriptors
```

A handler return ends one host function call; it does not stop QEMU or Linux.
Another kick can invoke the handler again later.

## What QEMU is doing

QEMU creates a complete virtual machine capable of booting a real Linux kernel.
It supplies the virtual CPU platform, RAM, interrupts, and the device-connection
machinery. In this run, that machinery includes PCI: a standard bus through
which an operating system discovers and communicates with devices.
The provided `run-qemu.sh` starts QEMU with a shareable memory backend and a
virtio console device.

Our code is not inside QEMU. `vlog-backend` is built from provided host plumbing
plus our `vhost/virtio.c`; it is a separate host process.
**vhost-user** is the protocol by which QEMU tells that process about guest
memory and queues over a Unix socket. A Unix socket is a local communication
channel between processes on the same host. `backend.c` implements that
plumbing; shared RAM carries the bulk data while the socket carries setup and
notifications.

```text
Linux guest <-> QEMU <--- Unix socket / shared RAM ---> vlog-backend
```

## Why Linux uses it without changes

Virtio devices have standardized IDs, configuration, queue layouts, and
notifications. They also perform **feature negotiation**, meaning the driver and
device exchange supported optional capabilities and agree which ones will be
used. QEMU advertises virtio device ID 3, the standardized console device type.
Linux's stock `virtio_console` driver recognizes it and exposes `/dev/hvc0`.
The tiny Linux init writes text there; the driver creates queue requests.

## What a descriptor is

A descriptor is a small record describing a guest-memory buffer. Linux chooses
`addr` from its guest RAM while preparing the request:

```c
struct vring_desc {
    uint64_t addr;   // GPA of buffer or indirect table
    uint32_t len;    // bytes
    uint16_t flags;  // NEXT, WRITE, INDIRECT
    uint16_t next;   // next descriptor index
};
```

The descriptor does not contain log bytes. It points to them.

- NEXT means continue at `next`.
- WRITE means the device may write into this buffer. This queue is guest-to-host,
  so such a descriptor is not input data and must be skipped.
- INDIRECT means `addr` points to another descriptor table, not payload.

## What a split virtqueue is

“Split” means metadata is divided into three shared structures:

```text
descriptor table         available ring              used ring
buffers and chains       guest -> device             device -> guest

[0] addr,len,flags  <--- ring[0]=head       head ---> ring[0]={id,len}
[1] addr,len,flags       idx=published                idx=completed
```

The **head** is the numeric index of a chain's first descriptor. The **available
ring** says which heads the driver has published. The
**used ring** says which heads the device has finished. The arrays are circular,
so free-running indices select slots with `% num`.

## Single and chained requests

```text
single:
avail head 2 -> desc[2] -> "1 hello\n"

chain:
avail head 0 -> desc[0] "1 hel" --NEXT--> desc[4] "lo\n"
device concatenates -> "1 hello\n"
```

**Scatter/gather** means one logical request can be assembled from several
buffers that are not adjacent in memory. Here, “gather” is the important half:
our backend gathers descriptor contents into one log record.

## Indirect descriptors

The main descriptor table has limited entries. An indirect descriptor lets one
main entry point to a larger table in guest RAM:

```text
main desc[3] --INDIRECT--> indirect[0] -> indirect[1] -> end
```

The indirect chain begins at index zero. Nested indirect tables are forbidden.
Our reusable `walk_chain()` receives `allow_indirect=0` while inside one.

## Kicks, interrupts, and completion

After publishing work, the driver **kicks** the device—virtio terminology for
notifying it that work is ready. The
provided backend calls `vlog_virtq_handle()`. After processing, the handler puts
the head in the used ring. Its return count tells the backend whether to raise an
interrupt. Without used completion, Linux believes the buffer remains owned by
the device and can eventually hang waiting for it.

The used entry's `len` counts bytes the device wrote into guest buffers. Thus
used `len=0` does not mean no input was consumed. It means the device wrote zero
bytes into device-writable guest buffers.

This focused excerpt from the provided `backend.c` shows the actual connection.
`tx_kick()` is called when Linux notifies the transmit queue; it builds the
small `struct virtq` view and then calls our Part III function:

```c
struct virtq vq = {
    .desc       = (struct vring_desc *)vuq->vring.desc,
    .avail      = (struct vring_avail *)vuq->vring.avail,
    .used       = (struct vring_used *)vuq->vring.used,
    .num        = vuq->vring.num,
    .last_avail = vuq->last_avail_idx,
};

struct virtq_mem mem;
build_mem(dev, &mem);

int n = vlog_virtq_handle(&vq, &mem, g_sink);
vuq->last_avail_idx = vq.last_avail;
```

Here `vuq` is the provided library's queue, `vq` is the smaller view passed to
our code, `mem` describes shared guest-memory regions, `g_sink` is the provided
logger, and `n` is the number of completed chains.

## Memory barriers

CPU/compiler reordering matters because guest and device share memory.

- Driver writes descriptors and ring slot, then publishes avail idx.
- Device reads avail idx, executes `virtq_rmb()` (an acquire barrier), then
  trusts earlier descriptor/ring writes. “Acquire” prevents later reads from
  being moved before the observed publication.
- Device writes used element, executes `virtq_wmb()` (a release barrier), then
  publishes used idx. “Release” prevents earlier writes from being moved after
  that publication.

The barrier creates visibility ordering; it is not an index bounds check or a
mutex.

## If a descriptor is bad, does the Linux VM stop?

No. A malformed descriptor is data presented to the device, not an unmapped CPU
instruction handled by Part I's `mem_invalid()`. Our walker refuses unsafe bytes,
terminates bounded traversal, and the handler still places the chain head in the
used ring. That returns ownership to Linux so one bad request does not stall the
whole queue. A production device might additionally expose an error/reset policy;
this teaching interface has no per-request error field.

## The Part III pipeline

```text
write /dev/hvc0
 -> Linux driver describes buffers
 -> avail ring + kick
 -> backend calls our handler
 -> walk descriptors and translate GPAs
 -> sink interprets "<level> <message>"
 -> logstore writes host file
 -> used ring returns chain
 -> interrupt lets Linux continue/reuse buffers
```
