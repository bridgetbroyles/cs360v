# 7. Part III Implementation — `vhost/virtio.c`

Read Chapter 6 first. Below, `vq` means the current virtqueue, `mem` is QEMU's
guest-physical-to-host memory map, `sink` is the provided host logger, `d` is one
descriptor, and `rec` is our local 4096-byte record array.

## Exact source locations

All paths are relative to `project_1/`:

| File | Role | Ownership for this assignment |
|---|---|---|
| `vhost/virtio.c` | GPA translation and virtqueue consumption | our Part III implementation |
| `vhost/virtq.h` | queue/memory structures and function declarations | provided |
| `vhost/backend.c` | vhost-user connection, QEMU memory mapping, queue setup, kicks | provided |
| `vhost/sink.c` | parses `<level> <message>` and writes the log | provided |
| `vhost/run-qemu.sh` | launches backend and QEMU/Linux integration | provided |
| `vhost/guest/init.c` | Linux guest program that writes `/dev/hvc0` | provided |

## What we implemented versus what was given

The starter assigned two public functions in `vhost/virtio.c`:
`virtq_gpa_to_hva()` and `vlog_virtq_handle()`. This branch implemented both and
added private helpers `append_data()` and `walk_chain()`. “Private” here means
`static`: only `virtio.c` can call them. The vhost-user protocol, QEMU startup,
Linux guest, log sink, ring structure definitions, and notification plumbing
were provided.

## Part III method map: what each function does at a high level

| Function | High-level job |
|---|---|
| `virtq_gpa_to_hva()` | proves a nonempty guest range fits in one QEMU-shared region and returns its host pointer |
| `append_data()` | safely adds one readable descriptor's bytes to a capped local record |
| `walk_chain()` | follows a bounded direct or indirect descriptor chain without trusting guest indices |
| `vlog_virtq_handle()` | consumes the currently published chains, emits records, and returns each chain through the used ring |
| `vlog_sink_emit()` | provided helper that parses the level prefix and writes the common host log format |

The first four functions form layers: translation protects one memory range;
append protects one data descriptor; walk protects one chain; the handler
protects and completes one finite batch of chains.

## How Part III connects to Parts I and II

Part III does **not** call `vmm_create()`, `vmm_gpa_to_host()`, or the Part II
MMIO callbacks. QEMU replaces the Part I VMM, and Linux's virtio driver replaces
the Part II guest register library. The connection is a repeated design pattern:

```text
Parts I/II                           Part III
guest supplies MSG GPA + LEN         descriptor supplies addr GPA + len
vmm_gpa_to_host() validates          virtq_gpa_to_hva() validates
CMD invokes device callback          kick invokes queue handler
STATUS/SEQ reports result            used ring reports completion
provided logstore writes record      provided sink/logstore writes record
```

Part III therefore depends on the ideas learned in Parts I/II, but its code runs
in a separate `vlog-backend` process beside QEMU.

The provided `vhost/virtq.h` definitions establish every field used below:

```c
struct virtq_mem_region {
    uint64_t gpa;
    uint64_t size;
    uint8_t *hva;
};

struct virtq_mem {
    const struct virtq_mem_region *regions;
    unsigned nregions;
};

struct virtq {
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t num;
    uint16_t last_avail;
};
```

`regions/nregions` describe QEMU-shared RAM. `desc`, `avail`, and `used` point
to the three split-ring structures. `num` is their capacity, and `last_avail` is
the backend's next unconsumed request counter.

## `virtq_gpa_to_hva()`

`virtq_gpa_to_hva(mem, gpa, len)` receives a region table, an untrusted guest
physical start address, and an unsigned byte length. **Purpose:** convert a
nonempty guest-physical range to a host pointer only when one QEMU memory region
fully contains it.

Control flow:

1. Reject `len==0`.
2. Reject explicit unsigned wraparound when `gpa + len < gpa`. Wraparound means
   an addition larger than the biggest 64-bit number rolls back to a small value.
3. Scan `mem->regions`.
4. Skip regions where GPA begins before the base or at/after the end.
5. Compute `offset=gpa-region->gpa`.
6. If LEN exceeds remaining region bytes, return NULL.
7. Otherwise return `region->hva + offset`.
8. Return NULL if no region contains the start.

The function assumes `mem`, its array, and region HVA values came validly from
the host backend. It defends against guest-controlled GPA/LEN.

One subtlety: after finding the region containing the start, an overrun returns
NULL immediately rather than checking later regions. That is appropriate for a
nonoverlapping guest map and enforces “one region only.”

```c
void *virtq_gpa_to_hva(const struct virtq_mem *mem,
                       uint64_t gpa, uint64_t len)
{
    if (len == 0)
        return NULL;
    if (gpa + len < gpa)
        return NULL;

    for (unsigned i = 0; i < mem->nregions; i++) {
        const struct virtq_mem_region *region = &mem->regions[i];

        if (gpa < region->gpa || gpa - region->gpa >= region->size)
            continue;

        uint64_t offset = gpa - region->gpa;
        if (len > region->size - offset)
            return NULL;

        return region->hva + offset;
    }
    return NULL;
}
```

`i` is the current region index, `region` points to that region's description,
and `offset` is how far the requested GPA lies past the region's GPA base.

Unlike Part I's executable loader, this function does not expect offset zero.
A Linux driver may place each buffer anywhere in any advertised guest-memory
region. The loop finds which region contains the supplied GPA, then computes
the offset relative to that particular region.

## `append_data()`

`append_data(d, mem, rec, rec_len)` receives one descriptor, the map, the output
array, and its initialized byte count. It translates the descriptor's complete
`addr,len` range. Failure means skip it and keep the old record length. Otherwise
it computes remaining space in the 4096-byte local record, copies the smaller of
descriptor length and room, and returns the new length.

It validates the full descriptor even if only a prefix fits. That refuses a
descriptor whose declaration crosses out of guest memory. When `rec_len` is
already 4096, room and copy length are zero; `memcpy(..., 0)` is safe after a
successful translation.

```c
static uint32_t append_data(const struct vring_desc *d,
                            const struct virtq_mem *mem,
                            char *rec, uint32_t rec_len)
{
    const void *src = virtq_gpa_to_hva(mem, d->addr, d->len);
    if (src == NULL)
        return rec_len;

    uint32_t room = VIRTQ_MAX_RECORD - rec_len;
    uint32_t copy_len = (d->len < room) ? d->len : room;
    memcpy(rec + rec_len, src, copy_len);
    return rec_len + copy_len;
}
```

`src` is the validated host pointer, `room` is unused capacity in `rec`, and
`copy_len` is the smaller of the offered bytes and that remaining capacity.

## `walk_chain()`

`walk_chain(table, table_size, start, allow_indirect, mem, rec, rec_len)` returns
the updated record length. It handles both the main table and an indirect table.
Parameters identify the table, number of entries, starting index, whether one
level of indirect is allowed, memory map, record, and current length.

`start` is the first descriptor index. `allow_indirect` is true for the main
table and false inside an indirect table. It loops at most `table_size` hops.
Before each access it checks `index < table_size`, then copies the shared
descriptor into local `d`. The copy ensures flags/address/length/next used in
this iteration come from one snapshot.

“Alignment” below means an address is a valid multiple for storing a particular
C type. `_Alignof(struct vring_desc)` asks C for that required multiple.
“Recursively walk” means `walk_chain()` calls itself on the indirect table, with
nesting disabled so the recursion can go only one level deep.

Descriptor cases, in the exact implementation order:

1. If INDIRECT is set and `allow_indirect` is true:
   - compute `entries = d.len / sizeof(vring_desc)`; any leftover bytes are
     ignored;
   - translate exactly `entries * sizeof(desc)` bytes;
   - require the resulting HVA to satisfy `_Alignof(struct vring_desc)`;
   - recursively walk entry zero with `allow_indirect=0`.
2. If INDIRECT is set but nesting is disallowed, the descriptor contributes no
   data.
3. Else if WRITE is set, skip it.
4. Else call `append_data()`.
5. Stop without NEXT; otherwise use `d.next` next iteration.

Important exact detail: INDIRECT is checked before WRITE. A descriptor with both
flags will be treated as indirect in this branch. The virtio format considers
such malformed combinations invalid; the supplied tests focus on supported
forms. A stricter implementation could reject the combination.

The hop cap guarantees a cyclic chain terminates. A valid acyclic chain cannot
visit more entries than its table has.

```c
static uint32_t walk_chain(const struct vring_desc *table,
                           uint32_t table_size, uint32_t start,
                           int allow_indirect,
                           const struct virtq_mem *mem,
                           char *rec, uint32_t rec_len)
{
    uint32_t index = start;

    for (uint32_t hops = 0; hops < table_size; hops++) {
        if (index >= table_size)
            break;

        struct vring_desc d = table[index];

        if (d.flags & VRING_DESC_F_INDIRECT) {
            if (allow_indirect) {
                uint32_t entries =
                    d.len / (uint32_t)sizeof(struct vring_desc);
                const struct vring_desc *indirect_table =
                    virtq_gpa_to_hva(
                        mem, d.addr,
                        (uint64_t)entries * sizeof(struct vring_desc));

                if (indirect_table != NULL &&
                    (uintptr_t)indirect_table %
                        _Alignof(struct vring_desc) == 0) {
                    rec_len = walk_chain(indirect_table, entries, 0, 0,
                                         mem, rec, rec_len);
                }
            }
        } else if (d.flags & VRING_DESC_F_WRITE) {
            /* Output space for the device; not input data for this queue. */
        } else {
            rec_len = append_data(&d, mem, rec, rec_len);
        }

        if (!(d.flags & VRING_DESC_F_NEXT))
            break;
        index = d.next;
    }
    return rec_len;
}
```

`index` selects the current descriptor, `hops` limits total iterations,
`entries` counts complete indirect descriptors, and `indirect_table` is the
validated host pointer to that secondary table.

## `vlog_virtq_handle()`

`vlog_virtq_handle(vq, mem, sink)` is called after Linux kicks the queue.
**Inputs:** a queue created by the backend, QEMU's memory map, and a log sink.
**Output:** number of available chains completed.

Actual flow:

```text
if num==0 -> return 0
snapshot avail->idx
acquire barrier
while last_avail != snapshot:
    head = avail->ring[last_avail % num]
    allocate local char rec[4096]
    walk main descriptor chain from head
    emit record (sink ignores length zero)
    used ring slot = {id=head, len=0}
    release barrier
    used->idx++
    last_avail++
    completed++
return completed
```

The avail index is read once. Work arriving during the call waits for a later
kick/handler call, making the batch finite.

An out-of-range head makes `walk_chain()` return length zero without indexing the
table. The sink ignores empty records. The handler still publishes used
completion, so malformed work cannot permanently consume driver buffers.

An invalid later descriptor is skipped or terminates traversal depending on the
problem; bytes already accumulated can still be emitted. This matches the test
contract. The local buffer is never initialized, but that is safe because the
sink receives only the initialized prefix `[0,rec_len)`, and ignores length zero.

The code only checks `vq->num==0`; it assumes `vq`, ring pointers, `mem`, and
`sink` are valid host-side objects from `backend.c`. Guest-provided indices and
descriptor contents are checked.

```c
int vlog_virtq_handle(struct virtq *vq, const struct virtq_mem *mem,
                      struct vlog_sink *sink)
{
    if (vq->num == 0)
        return 0;

    uint16_t avail_idx = vq->avail->idx;
    virtq_rmb();

    int completed = 0;
    while (vq->last_avail != avail_idx) {
        uint16_t head =
            vq->avail->ring[vq->last_avail % vq->num];

        char rec[VIRTQ_MAX_RECORD];
        uint32_t rec_len =
            walk_chain(vq->desc, vq->num, head, 1, mem, rec, 0);
        vlog_sink_emit(sink, rec, rec_len);

        uint16_t used_idx = vq->used->idx;
        vq->used->ring[used_idx % vq->num].id = head;
        vq->used->ring[used_idx % vq->num].len = 0;
        virtq_wmb();
        vq->used->idx = (uint16_t)(used_idx + 1);

        vq->last_avail++;
        completed++;
    }
    return completed;
}
```

`avail_idx` is the driver's published work counter observed at entry, `head` is
one chain's first descriptor index, `used_idx` selects the completion slot, and
`completed` is returned so the provided backend knows whether to interrupt
Linux.

The handler passes each assembled record to the provided function below in
`vhost/sink.c`. This is where the Part III wire format becomes the same host log
format used in Part II:

```c
void vlog_sink_emit(struct vlog_sink *sink,
                    const void *bytes, uint32_t len)
{
    const char *p = bytes;

    while (len && (p[len - 1] == '\n' || p[len - 1] == '\r'))
        len--;
    if (!len)
        return;

    uint32_t level = 1;
    if (len >= 2 && p[0] >= '0' && p[0] <= '9' && p[1] == ' ') {
        level = (uint32_t)(p[0] - '0');
        p += 2;
        len -= 2;
    }
    logstore_append(sink->store, sink->seq++, level, p, len);
}
```

`p` begins at the assembled bytes, then advances past a recognized level prefix.
The sink owns its own `seq`; it is not the Part II device's `dev->seq` because
Part III runs in a different process.

## Malformed-input reference

| Problem | Defense/result |
|---|---|
| GPA overflow | translator returns NULL |
| address in gap/outside map | NULL; data skipped |
| range straddles region end | NULL; data skipped |
| head/next out of range | stop before table access |
| direct/indirect cycle | hop cap terminates |
| writable descriptor | skipped as non-input |
| record over 4096 | data truncated safely |
| zero-length descriptor | translation fails; skipped |
| indirect table unaligned in HVA | not walked |
| nested INDIRECT | contributes nothing |
| malformed request overall | still returned through used ring |

These failures do not call `uc_emu_stop()` because Part III does not use
Unicorn, and malformed queue input is handled as a device request failure rather
than a fatal Linux CPU fault. The handler completes the chain and returns to the
provided backend; QEMU and Linux continue running.

## Interview answer

“The handler snapshots the avail counter with acquire ordering and processes
each published head. A bounded reusable walker snapshots each descriptor,
handles one indirect level, skips writable or untranslated buffers, and appends
at most 4096 bytes. The sink emits the initialized prefix. Then we write a
zero-length used completion, release-fence, publish used idx, and advance our
avail cursor. The central invariants are no unchecked guest index, no
untranslated guest address, no unbounded chain, and no missing completion.”
