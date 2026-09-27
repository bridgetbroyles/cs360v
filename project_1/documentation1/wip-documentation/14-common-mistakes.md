# 14. Common Mistakes

Use this only after the fundamentals and implementation chapters. It is a list
of corrections, so it intentionally uses shortened terms already defined there.

## Things you are likely to be asked

- Why a bare-metal guest cannot call `printf` or open the host log.
- Why allocation and Unicorn mapping are separate steps.
- Why MMIO callbacks receive offsets, not full addresses.
- Why GPA translation validates a range, not only a start.
- Why fault result takes priority in `vmm_run()`.
- Why zero-length LOG ignores MSG.
- Why STAT's `memcpy` matters.
- Why Linux needs no custom Part III driver.
- Why avail and used are both required.
- Why WRITE buffers are skipped.
- Why index checks and hop limits solve different problems.
- Why memory barriers surround ring publication.

## Plausible but wrong statements for this code

- “The bare-metal guest is a small Linux guest.” It has no OS.
- “Unicorn is the VMM.” Unicorn supplies CPU/memory APIs; our code constructs the
  machine policy and devices.
- “GVA/GPA/HVA are all equal because paging is off.” Only guest virtual address
  equals guest physical address; the host virtual address is separate.
- “The callback gets `DEV_BASE+offset`.” It gets the relative offset.
- “Part III upgrades the Part II VMM.” It is a separate QEMU/backend path.
- “QEMU calls `device.c`.” Part II's `device.c` belongs to the Unicorn path;
  QEMU reaches the vhost backend path using `virtio.c`.
- “SEQ advances on every command.” Only successful LOG.
- “An unknown level is BADLEN/BADCMD.” It succeeds as `LVL?`.
- “STAT casts the output to a struct pointer.” Implementation2 uses `memcpy`.
- “Part I accepts a zero-length range at RAM end.” It rejects start-at-end.
- “Indirect LEN must be an exact multiple.” This implementation truncates to
  complete entries; remainder is ignored.
- “WRITE is checked before INDIRECT.” The exact walker checks INDIRECT first.
- “Bad heads are left pending.” They produce empty output but are completed.
- “The handler rereads avail idx each iteration.” It snapshots once.
- “Used LEN is request bytes read.” It counts device-written bytes, so it is zero.
- “The queue code null-checks every pointer.” It trusts host-side backend objects.

## A reliable explanation pattern

For any selected code, answer in this order:

1. Name whether each value is host-controlled or guest-controlled.
2. State the invariant being enforced.
3. Walk the exact state/control change.
4. Explain the failure behavior.
5. Explain why the next layer needs the result.
6. Offer one realistic alternative and its tradeoff.
