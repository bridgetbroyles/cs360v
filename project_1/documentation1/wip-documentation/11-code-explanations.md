# 11. Code-Level “Explain This” Preparation

Use this after the implementation chapters. Each heading quotes an actual code
expression. For any selected line, explain: where each input came from, what
invariant is checked, what state changes, and what later code relies on it.

## `putchar((int)(value & 0xff))`

`value` is the 64-bit value Unicorn received from the guest's MMIO store. The
expression selects only the serial protocol's low byte. It changes host stdout, not guest
RAM. Without masking, a wider callback value could be misinterpreted.

## `uc_mmio_map(..., serial_read, v, serial_write, v)`

`uc_mmio_map` is Unicorn's API for assigning a GPA range to read/write callbacks.
This call passes the same VMM pointer back to both
callbacks. The offset callback receives is relative to SERIAL_BASE.

## `uc_hook_add(..., 1, 0)`

`uc_hook_add` registers a callback for an execution event. `UC_HOOK_MEM_UNMAPPED`
selects invalid memory accesses. The `1, 0` range registers it across all
addresses; in Unicorn, a beginning address greater than the ending address means
“all addresses.” The callback returns false to reject the access.

## `offset >= RAM_SIZE`

Requires the starting byte to be inside RAM. It rejects the one-past address even
for LEN zero. Only after this is `RAM_SIZE-offset` safe.

## `(uint64_t)dev->msg_addr_hi << 32`

Widens before shifting. A 32-bit shift by 32 is invalid/loses data. OR with the
low half reconstructs MSG.

## `clear_error(dev)` in `run_command()`

Makes STATUS report the newest command. READY remains because only ERROR and
error-code bits are masked. Without it, OR-based codes become stale/combined.

## `memcpy(out, &stats, sizeof stats)`

Writes exact bytes to translated guest RAM without assuming `out` satisfies
struct alignment. State read is `seq/bytes`; no counters change.

## `struct vring_desc d = table[index]`

After bounds checking, snapshots guest-shared metadata so this iteration uses
one addr/len/flags/next combination. It does not eliminate every concurrent
guest race but avoids multiple inconsistent field loads.

## `for (hops=0; hops<table_size; hops++)`

Bounds work even if NEXT forms a cycle. A well-formed acyclic chain cannot visit
more entries than exist. The loop plus index check protects time and memory.

## `entries = d.len / sizeof(struct vring_desc)`

Counts complete indirect descriptors; remainder bytes are ignored in this
implementation. The translated length is the complete-entry prefix. Zero entries
causes translation to reject the empty range.

## Alignment check

The recursive walker dereferences `struct vring_desc *`, so the resulting host
address must meet `_Alignof(struct vring_desc)`. Otherwise typed access could be
undefined despite byte-range validity.

## `avail->ring[last_avail % num]`

`last_avail` is our next logical request number and `num` is the physical ring
capacity. The logical index is a free-running 16-bit counter; modulo chooses the
reused circular slot. `num==0` is checked first.

## used entry, barrier, then used idx

The driver must not observe a new used count before the entry contents. Release
ordering enforces publication order. ID is the original chain head; LEN zero
means no device writes into guest buffers.
