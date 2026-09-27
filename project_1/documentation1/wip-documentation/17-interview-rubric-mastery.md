# 17. Interview Rubric Mastery Workbook

This document prepares you specifically for the three interview rubric areas:

1. implementation understanding;
2. design and tradeoff reasoning;
3. debugging and modification.

It assumes you have read Chapter 16 once. Use this as an active workbook: cover
the model answer, speak your answer aloud, then compare.

## 1. How to structure every interview answer

### Implementation question

Use this order:

```text
role in system
 -> who calls it
 -> meaning and trust level of each input
 -> validation/control flow
 -> state or side effects
 -> return value/failure behavior
 -> what consumes the result next
```

Do not begin by translating C syntax one line at a time. First tell the
interviewer why the function exists. Then connect individual lines to that
purpose.

### Design question

Use this order:

```text
requirement
 -> choice we made
 -> benefit
 -> cost or limitation
 -> realistic alternative
 -> when the alternative would be better
```

A tradeoff answer is incomplete if it only says our solution is good. Strong
reasoning identifies what it gives up.

### Debugging or modification question

Use this order:

```text
reproduce and classify
 -> identify earliest failing layer
 -> locate relevant files/functions
 -> inspect observable state
 -> form a testable hypothesis
 -> make the smallest coherent change
 -> predict tests and downstream effects
```

The interview is testing whether you can navigate the system, not whether you
can guess a patch immediately.

## 2. Implementation Understanding

### Code ownership map

| Concern | Start here | Related code |
|---|---|---|
| VM construction and lifecycle | `project_1/emulator/vmm.c` | `vmm.h`, `main.c` |
| Machine constants and memory map | `project_1/emulator/vmm.h` | `guest/link.ld`, `guest/start.S` |
| Custom logging device | `project_1/emulator/device.c` | `device.h`, `guest/vlog.c` |
| Host record formatting | `project_1/emulator/logstore.c` | `logstore.h` |
| Virtqueue implementation | `project_1/vhost/virtio.c` | `virtq.h` |
| QEMU/vhost-user connection | `project_1/vhost/backend.c` | `run-qemu.sh` |
| Part III record parsing | `project_1/vhost/sink.c` | `logstore.c` |
| Expected behavior/tests | `project_1/tests/` | `SPEC.md` |

### Every implemented function: detailed navigation map

Use this table to identify a selected function quickly, then expand it with the
drills below.

| Function | Called by / trigger | Inputs and trust | Main checks | Success effects | Failure behavior |
|---|---|---|---|---|---|
| `serial_write` | Unicorn on guest store to serial page | offset/value are produced by guest instruction; `v` is trusted callback context | recognizes TX or POWEROFF offset | TX prints low byte; poweroff records code/flag and stops Unicorn | unknown offset ignored |
| `mem_invalid` | Unicorn after unmapped fetch/read/write | address/type/size/value describe failed guest access; `v` trusted | Unicorn already determined mapping is absent | records fault/address, diagnostic, stops run | returns false so no retry; `vmm_run` returns 77 |
| `vmm_create` | provided `main` before loading | paths/trace from host CLI; `v` host-owned | allocation/API return checks for major setup steps | establishes store, engine, RAM, MMIO, registers, hooks | returns -1; current version does not fully unwind partial setup |
| `vmm_load_binary` | provided `main` after create | host path/file contents | open, nonnegative size, size <= RAM, complete read | bytes at `v->ram+0`; RIP=`RAM_BASE` | closes file and returns -1; VM not run |
| `vmm_run` | provided `main` after loading | initialized `v` | fault state first; engine error only fatal when not powered off | returns guest poweroff code | fault=77; unexpected engine error=1 |
| `vmm_gpa_to_host` | Part II LOG/STAT and tests | GPA/LEN can be guest-controlled; `v->ram` trusted | below-base, start-at/end, remaining capacity | returns HVA `v->ram+offset` | NULL, no side effect |
| `msg_addr` | Part II commands | two latched guest-written halves | casts high half to 64 bits before shift | returns reconstructed GPA | no independent error; caller validates range |
| `set_error` / `clear_error` | command dispatch/helpers | device-owned status/code selected by implementation | masks only ERROR/code on clear | latest command status while preserving READY | misuse of OR without clear could retain stale codes |
| `cmd_log` | `run_command(LOG)` | LEN/MSG/LEVEL latched from guest | LEN <= 4096; nonempty full GPA range translates | append, then SEQ++ and bytes+=LEN | sets BADLEN/BADADDR; no append/counter change |
| `cmd_stat` | `run_command(STAT)` | LEN/MSG guest-controlled; counters device-owned | LEN >= 8; exact output range translates | copies local stats into guest RAM | BADLEN/BADADDR; no write/counter change |
| `run_command` | MMIO write callback at CMD | command value guest-controlled | known command switch after clearing stale error | invokes NOP/LOG/FLUSH/STAT | unknown command sets BADCMD; VM continues |
| `vlog_device_mmio_read` | Unicorn on guest read from device page | offset guest-selected within mapped page; `dev` trusted | switch recognizes readable register | returns constant or latched state | CMD/unknown offset returns zero |
| `vlog_device_mmio_write` | Unicorn on guest store to device page | offset/value guest-controlled; callback width provided by Unicorn | truncates to 32-bit protocol value; offset switch | latches operands or synchronously dispatches CMD | read-only/unknown offset ignored |
| `virtq_gpa_to_hva` | descriptor and indirect-table processing | GPA/LEN from guest; region table from backend | nonzero LEN, addition wrap, containing region, remaining capacity | returns region HVA+offset | NULL; caller skips unsafe data/table |
| `append_data` | walker for readable descriptor | descriptor guest-controlled; record state local | full range must translate; copy limited to remaining 4096 capacity | appends initialized bytes and returns new length | leaves old record length unchanged |
| `walk_chain` | queue handler or itself for indirect table | table contents/indices guest-controlled; map trusted | index before access, hop cap, indirect translation/alignment/nesting, WRITE direction | accumulates bounded readable bytes | stops/skips unsafe component; preserves bytes already gathered |
| `vlog_virtq_handle` | provided backend after queue kick | rings/descriptors guest-shared; wrapper pointers backend-trusted | num nonzero, acquire ordering, helper validations | sink emission, used completion, cursor advance, completion count | malformed work logs no unsafe bytes but is still completed |

For any row, a complete verbal explanation should also state which subsequent
function consumes the output. For example, `walk_chain` returns `rec_len` to the
handler; the handler passes exactly that initialized prefix to the sink.

### Drill 1: explain `vmm_create()` in detail

#### What a good answer must include

- It constructs the machine; it does not yet execute guest instructions.
- `v` is host-owned central state.
- It creates the log store and Unicorn x86-64 engine.
- It allocates zeroed host RAM and maps it at guest `RAM_BASE`.
- It maps serial MMIO with `v` as callback context.
- It allocates/initializes the Part II device and maps it with `v->dev` as
  callback context.
- It initializes RSP and RDI.
- It installs the unmapped-memory hook and optional tracing hook.
- It returns zero only after the VM is usable.
- It has incomplete cleanup/checking on partial failure.

#### Model answer

“`vmm_create` assembles the virtual machine around Unicorn. It first clears the
host-side state, opens the provided log store, and creates a 64-bit x86 Unicorn
engine. It allocates 16 MiB of zeroed host memory and maps that allocation at
guest `RAM_BASE`, so Unicorn and our callbacks share the same bytes. It maps the
serial page using the VMM pointer as context. It then allocates the Part II
device, gives it a back-pointer to the VMM, and maps its MMIO page using the
device pointer as context. Finally it establishes startup state—RSP below boot
info and RDI pointing to boot info—and installs fault and optional trace hooks.
The function checks major allocation and mapping failures, but a production
version would use one cleanup path and check every register/hook call.”

#### Follow-up: why different callback contexts?

Serial needs fields such as `powered_off` and `exit_code`, so it receives
`struct vmm *`. The custom logger primarily operates on latched device registers,
so it receives `struct vlog_device *`; that object contains `dev->vmm` when it
needs Part I RAM or log state.

### Drill 2: explain `vmm_load_binary()`

#### Model answer

“This is a fixed-address flat-image loader, not an ELF loader. It opens and
measures the file, rejects a negative or larger-than-RAM size, rewinds, and
copies exactly that many bytes to `v->ram+0`. Part I mapped that host byte as
guest `RAM_BASE`; the linker also placed `_start` at `RAM_BASE`. The function
then writes RIP=`RAM_BASE`, completing the agreement between linker address,
binary offset, RAM mapping, and entry point. A short read fails rather than
running a partial image.”

#### Likely trap

`SEEK_END` measures the input file. It does not load the binary at the end of
RAM. The destination passed to `fread` is still `v->ram`, meaning offset zero.

### Drill 3: explain `mem_invalid()` and `vmm_run()` together

#### Model answer

“Unicorn, not `mem_invalid`, determines that a guest fetch/read/write is outside
all mapped RAM and MMIO. The callback records the semantic cause and bad GPA,
prints to stderr, stops the current Unicorn execution, and returns false so the
access is not retried. `uc_emu_start` then returns to `vmm_run`. The run function
checks `faulted` first because Unicorn may also report a generic engine error;
the more meaningful result is the specified fault code 77. A clean serial
poweroff instead sets `powered_off` and an exit code before stopping.”

#### Important consequence

The guest does not resume after this fault. By contrast, a rejected Part II
command sets STATUS and guest execution does resume.

### Drill 4: explain `vmm_gpa_to_host()`

#### Model answer

“The input GPA and length may be guest-controlled, so the function treats them
as an untrusted half-open range. It first rejects addresses below `RAM_BASE`,
then subtracts the base only when safe. It rejects an offset at or past RAM's
end, then compares LEN with `RAM_SIZE-offset`. The remaining-space comparison
avoids an overflowing `gpa+len` calculation. Only after the whole range is
proven does it return `v->ram+offset`.”

#### Edge cases to predict

| Input | Result |
|---|---|
| GPA below RAM | NULL |
| GPA exactly at RAM end, LEN zero | NULL because start is not inside RAM |
| GPA inside RAM, LEN zero | pointer inside RAM |
| final byte exactly reaches RAM end | valid |
| range extends one byte past end | NULL |

### Drill 5: explain Part II LOG from guest call to host file

#### Model answer

“The provided guest helper receives a guest pointer, which numerically equals a
GPA because paging is disabled. It splits that address into MSG_LO/MSG_HI,
writes LEN and LEVEL, then commits with CMD=LOG. Unicorn invokes our MMIO write
callback at the CMD offset. `run_command` clears stale error state and calls
`cmd_log`. LOG rejects LEN over 4096, deliberately ignores MSG for an empty
message, otherwise translates the complete range through Part I, and reports
BADADDR on failure. Only after validation does it call the provided logstore,
then increment SEQ and total bytes. The callback returns synchronously and the
guest can read STATUS.”

### Drill 6: explain why STAT uses `memcpy()`

#### Model answer

“STAT writes eight bytes from host device state into guest RAM. The GPA
translator proves byte-range validity but does not promise alignment for
`struct vlog_stats`. Dereferencing a misaligned typed pointer can be undefined
in C. Building a local aligned structure and copying its bytes with `memcpy`
works for every byte-valid destination. STAT translates only the eight bytes it
will write, requires the offered LEN to be at least eight, and does not modify
SEQ or byte counters.”

### Drill 7: explain `virtq_gpa_to_hva()`

#### Model answer

“This is the Part III counterpart of Part I translation, but QEMU may expose
multiple noncontiguous regions. It rejects empty ranges and explicit 64-bit
addition wraparound, scans for the region containing the starting GPA, computes
the relative offset, and rejects LEN greater than that region's remaining
capacity. It never joins two regions across a gap. On success it returns that
region's HVA plus offset.”

### Drill 8: explain `walk_chain()` precisely

#### Model answer

“The function walks either the main descriptor table or one indirect table. It
checks the current index before every table access and caps hops at table size,
so invalid links and cycles cannot escape memory or run forever. It snapshots
the shared descriptor locally. INDIRECT is handled first: when allowed, it
counts complete entries, translates the table, checks host alignment, and
recurses at index zero with further indirection disabled. A nested indirect
descriptor contributes no bytes. WRITE descriptors are skipped because they
are device-output buffers on a guest-to-host queue. Other descriptors go
through `append_data`. NEXT selects another checked iteration; otherwise the
chain ends.”

### Drill 9: explain `vlog_virtq_handle()` precisely

#### Model answer

“The handler returns immediately for queue size zero, then snapshots the
driver's avail index and uses an acquire barrier before trusting published ring
metadata. It processes a finite batch until `last_avail` reaches that snapshot.
For each head it gathers a bounded record, passes the initialized prefix to the
provided sink, writes `{id=head,len=0}` in the used ring, release-orders that
entry before incrementing used idx, advances its consume cursor, and counts the
completion. Even malformed heads are completed so Linux regains ownership.
The returned count tells the backend whether an interrupt is useful.”

## 3. Design and Tradeoff Reasoning

### What “tradeoff reasoning” actually means

For each decision, be able to name five things:

1. the concrete requirement or problem;
2. the design this branch chose;
3. the benefit produced by that design;
4. the cost, assumption, or behavior it cannot provide;
5. a realistic alternative and the situation where it would be preferable.

Avoid imaginary alternatives such as “write perfect code.” An alternative must
be implementable and must optimize a different concern.

### Tradeoff 1: host-backed RAM versus Unicorn-owned RAM

**Problem:** Unicorn must execute guest instructions against RAM, while Part II
must safely read or write guest buffers from ordinary host C code.

**Our choice:** allocate RAM with `calloc()` in the host and give that same
pointer to Unicorn through `uc_mem_map_ptr()`.

```text
guest GPA RAM_BASE+offset
        ↓
Part I validates offset and length
        ↓
host pointer v->ram+offset
```

**Benefits:**

- CPU and device operate on the same allocation without synchronizing copies.
- GPA translation is understandable checked pointer arithmetic.
- RAM begins zeroed, which provides predictable BSS/unused memory.
- Part II can use normal C operations after validation.

**Costs and assumptions:**

- The host pointer is exposed to more VMM code, so an unchecked offset would be
  dangerous.
- This Part I mapping assumes one fixed contiguous RAM region.
- The design is tied to a Unicorn API that accepts caller-owned backing memory.
- Memory permissions are broad (`UC_PROT_ALL`) rather than segment-specific.

**Alternative:** let Unicorn own its internal mapped memory and use
`uc_mem_read()`/`uc_mem_write()` for every device transfer.

**When the alternative wins:** when stronger encapsulation matters more than
copy overhead, or when the emulator cannot expose a stable host pointer. The API
boundary can centralize checks, but every device operation performs a copy and
must handle partial/API failures.

**Model answer:** “We chose host-backed RAM because both the emulated CPU and
our device need the same bytes. It makes translation and teaching the address
relationship simple and avoids copies. The tradeoff is that host code can touch
the backing allocation directly, so validation is a critical security boundary
and the layout is less flexible. Unicorn-owned memory plus access APIs would
encapsulate RAM better at the cost of extra copying and error handling.”

### Tradeoff 2: fixed flat binary versus an ELF loader

**Problem:** the VMM needs to know where to place guest code and which
instruction executes first.

**Our choice:** use one flat binary whose byte zero was linked as `_start` at
`RAM_BASE`; copy it to `v->ram+0` and set RIP=`RAM_BASE`.

**Benefits:**

- The loader is only file measurement, one read, and one register write.
- There is no parser attack surface or complicated segment policy.
- Entry state and expected addresses are deterministic.
- It is appropriate for a tiny freestanding teaching guest.

**Costs and assumptions:**

- The image, linker script, RAM mapping, and RIP must all agree on one address.
- The file contains no load addresses, entry metadata, or permissions.
- Loading a second independent image would overwrite the first unless the API
  and layout changed.
- It cannot naturally represent sparse code/data segments or dynamic linking.

**Alternative:** parse ELF program headers, map/load each `PT_LOAD` segment,
zero BSS, honor permissions, reject overlaps, and use the ELF entry point.

**When the alternative wins:** when running general compiled programs, multiple
segments, read-only/executable permissions, or relocatable images. It is much
more capable, but parsing untrusted executable metadata creates additional
validation and cleanup obligations.

**Model answer:** “The flat format minimizes the loader because the assignment
controls the guest and linker. Its cost is a rigid ABI: one fixed image and no
segment metadata. An ELF loader would support realistic executables and
permissions, but it would require careful parsing, range/overlap validation,
BSS handling, and entry-point checks.”

### Tradeoff 3: fatal unmapped access versus recoverable demand mapping

**Problem:** decide what an unmapped guest CPU access means for this machine.

**Our choice:** record it as a fault, stop Unicorn, return false so the
instruction is not retried, and return exit code 77.

**Benefits:**

- The memory map remains explicit and deterministic.
- Invalid guest behavior cannot cause arbitrary allocation or continue in an
  uncertain state.
- Tests and callers get one clear semantic fault result.
- The hook is small and does not need allocation/rollback logic.

**Cost:** there is no page-fault recovery, lazy allocation, or guest virtual
memory. One invalid instruction ends the current VM run.

**Alternative:** validate that the fault lies in an allowed demand-paged range,
allocate backing memory, map it into Unicorn, and return true to retry.

**When the alternative wins:** when implementing virtual memory, lazy stacks,
copy-on-write, or memory overcommit. It requires a page-ownership policy,
permission tracking, out-of-memory behavior, cleanup, and protection against a
guest forcing unlimited allocation.

**Model answer:** “Stopping is not universally better; it matches this fixed
machine ABI. Demand mapping would be appropriate for an OS-like VMM, but
returning true safely requires actually repairing a permitted mapping and
tracking its lifecycle. Mapping every fault would hide bugs and let the guest
drive host allocation.”

### Tradeoff 4: MMIO operand registers versus an in-memory command structure

**Problem:** the bare-metal guest needs to describe a message address, length,
severity, and requested operation.

**Our choice:** expose individual MSG_LO, MSG_HI, LEN, LEVEL, and CMD registers.
Operand writes latch state; CMD commits the command.

**Benefits:**

- It resembles a small hardware register protocol.
- Each register can be discovered and tested independently.
- A 64-bit GPA works even though each register is 32 bits.
- The commit point clearly separates setup from execution.

**Costs:**

- One logical request requires several MMIO operations.
- Operands persist, so the guest can accidentally reuse a stale value.
- If multiple guest CPUs wrote the same device concurrently, register sequences
  could interleave without additional serialization.
- Extending the protocol consumes register offsets and shared constants.

**Alternative:** write one packed command structure in guest RAM and expose a
single submit register containing its GPA, or use a queue of such commands.

**When the alternative wins:** for larger requests, batching, multiple
producers, or asynchronous operation. The device then must validate the command
structure itself, version its layout, handle alignment/endian details, and
define ownership while guest and device share it.

**Model answer:** “Registers make the protocol visible and hardware-like, which
is ideal for this small teaching device. The tradeoff is more MMIO traffic and
latched-state/concurrency concerns. A command structure or queue scales better,
but shifts complexity into guest-memory validation, ownership, and versioning.”

### Tradeoff 5: synchronous CMD versus asynchronous processing

**Problem:** decide when the guest considers a command executed and how it
learns the result.

**Our choice:** writing CMD executes the entire command inside the MMIO callback;
the guest reads STATUS after the store returns.

**Benefits:** simple ordering, no worker thread, no request identifiers, and no
interrupt/completion queue. The guest can reason sequentially.

**Costs:** the virtual CPU is blocked while the host appends or flushes. A slow
host file operation directly increases guest latency, and there is no batching
or overlap.

**Alternative:** CMD enqueues work; a worker processes it and later updates a
completion queue/status plus interrupt.

**When the alternative wins:** high-throughput devices, slow I/O, or a guest
that should continue computing. It requires request lifetime rules, concurrency
control, queue-full behavior, ordering, cancellation/reset, and notification.

**Model answer:** “Synchronous execution is a deliberate simplicity choice for
tiny logs. It gives an immediate status contract but couples guest progress to
host I/O. An asynchronous queue would improve throughput and overlap at the
cost of ownership, synchronization, and completion machinery—essentially the
kind of complexity Part III introduces.”

### Tradeoff 6: remaining-space validation versus end-address calculation

**Problem:** prove `[gpa, gpa+len)` fits without integer overflow.

**Our Part I choice:** after safely computing an in-range offset, require:

```c
len <= RAM_SIZE - offset
```

**Benefit:** subtraction expresses exactly how many bytes remain, and no
potentially overflowing end address is needed.

**Cost:** the checks must occur in the correct order. Computing
`RAM_SIZE-offset` before proving `offset<RAM_SIZE` would underflow.

**Alternative:** use a checked-addition primitive to compute `end=gpa+len`,
reject overflow, then compare the end with the mapping's end. Part III actually
uses an explicit wrap check plus remaining-space comparison.

**When the alternative wins:** when a codebase already has audited overflow
helpers or when computed end addresses are used elsewhere. Both strategies are
correct if their preconditions are explicit.

**Model answer:** “We did not merely choose subtraction for style. It avoids
forming an unchecked end address. The dependency is that start and offset must
be validated first. Checked addition is equally valid and can be clearer in a
codebase with standard overflow helpers.”

### Tradeoff 7: `memcpy()` versus a typed STAT store

**Problem:** write a `struct vlog_stats` into a guest-selected byte-valid buffer
that may not satisfy the C type's alignment.

**Our choice:** construct a local aligned structure and `memcpy` its bytes.

**Benefits:** defined behavior at any byte alignment, exact eight-byte write,
and a clear separation between validation and side effect.

**Cost:** a small copy and an implicit assumption that guest/host agree on this
simple structure's byte layout and endianness.

**Alternative:** require MSG alignment as part of the ABI and use a typed store,
or serialize each fixed-width field explicitly.

**When alternatives win:** required alignment can simplify fast hardware-style
access but makes the interface less forgiving. Explicit serialization is better
across different endianness/layout contracts, but needs a defined wire format.

**Model answer:** “Range validity does not imply alignment. `memcpy` avoids
undefined unaligned typed access for negligible cost on eight bytes. If this
were a cross-platform wire protocol, I would go further and serialize each
field with defined endianness rather than copying a host struct layout.”

### Tradeoff 8: one local 4096-byte record versus zero-copy scatter/gather

**Problem:** one logical virtio record may arrive through several noncontiguous
descriptors, while the sink expects one byte sequence.

**Our choice:** validate each complete readable descriptor and concatenate at
most 4096 bytes into a local stack array.

**Benefits:**

- One explicit upper bound prevents unbounded memory use.
- The sink receives one simple pointer/length pair.
- Guest buffer changes after copying cannot alter already gathered bytes.
- Chained and indirect descriptors share the same append logic.

**Costs:**

- Every byte is copied.
- Each handler invocation uses a 4096-byte stack array.
- Long records are deliberately truncated.
- It does not preserve segment boundaries for a scatter/gather-aware consumer.

**Alternative:** pass an array of validated iovecs to a scatter/gather-aware
sink, stream each segment, or allocate a dynamically sized record.

**When alternatives win:** large/high-throughput records where copying dominates.
But the design must preserve a single record boundary, cap total bytes, keep
guest memory valid/stable for the sink's lifetime, and decide what happens if
the guest changes shared memory concurrently.

**Model answer:** “The local array trades throughput for boundedness and a very
simple ownership model. Zero-copy iovecs could be faster, but they extend the
time we depend on guest-shared memory and require the sink to understand
scatter/gather. Dynamic allocation removes the stack cost but adds allocation
failure and guest-driven memory-pressure concerns.”

### Tradeoff 9: reusable recursive walker versus separate or iterative walkers

**Problem:** direct and indirect tables contain the same descriptor format and
chain semantics, but indirect tables cannot nest.

**Our choice:** one `walk_chain()` handles both; recursion enters an indirect
table with `allow_indirect=0`.

**Benefits:** validation, NEXT handling, WRITE handling, record capping, and hop
limits are implemented once. The argument explicitly enforces one nesting level.

**Costs:** recursion requires careful table-size/start arguments and a guard.
Malformed INDIRECT+WRITE precedence follows branch order. Although depth is
bounded to one, recursive code can be harder to audit than a flat state machine.

**Alternatives:** separate main/indirect loops, or an iterative walker with an
explicit current-table state/stack.

**When alternatives win:** separate loops can be easier to specialize but risk
duplicated validation diverging. An iterative state machine is preferable if
the protocol permits deeper nesting or strict environments prohibit recursion,
but it carries more explicit state.

**Model answer:** “Recursion here is bounded by protocol, not unbounded general
recursion. Reuse keeps the security checks identical for both tables. The
tradeoff is more subtle control flow and the need for an explicit no-nesting
flag. If nesting depth grew, I would prefer an iterative explicit stack.”

### Tradeoff 10: descriptor snapshot versus repeated shared-memory reads

**Problem:** descriptor fields reside in memory shared with the guest and could
change while the backend is processing them.

**Our choice:** after checking the index, copy `table[index]` into local `d` and
use that snapshot for the iteration.

**Benefits:** addr, len, flags, and next are one locally stable field set; code
does not validate one value and later accidentally use a different reread.

**Limitation:** an ordinary structure copy is not a complete concurrency
protocol. A malicious guest could still race during the copy, and the code
relies on virtio ownership expectations plus the acquire barrier for normal
driver behavior.

**Alternatives:** individually atomic field access with a version/seqlock,
copy-and-verify twice, or move descriptor ownership into protected backend
memory.

**When alternatives win:** adversarial concurrent mutation guarantees beyond
the assignment contract. They cost extra reads, protocol metadata, or copies.

**Model answer:** “The snapshot narrows race inconsistency but does not magically
make guest memory immutable. It is a pragmatic defense layered on virtio's
ownership model. Stronger adversarial-race protection would require an explicit
versioning or copying protocol, not merely rereading fields.”

### Tradeoff 11: hop bound versus a visited bitmap

**Problem:** a guest may construct `NEXT` links that cycle forever.

**Our choice:** allow at most `table_size` descriptor visits.

**Why it is correct:** a valid acyclic chain cannot visit more distinct entries
than the table contains. If traversal wants another hop after that limit, it is
cyclic or otherwise invalid.

**Benefits:** constant extra memory, no allocation, and one simple termination
argument that works for both direct and indirect tables.

**Cost:** a short cycle may still consume up to `table_size` iterations before
termination, and the function does not identify which index repeated.

**Alternative:** maintain a bitmap/set of visited indices and stop immediately
on a revisit.

**When the alternative wins:** very large tables where early cycle detection or
diagnostics matter. It requires memory proportional to table size, clearing or
allocation, and its own bounds-safe indexing.

**Model answer:** “The hop cap protects time with zero allocation. A visited
bitmap detects cycles more precisely and sooner, but uses extra memory and
complexity. For this bounded teaching queue, the hop proof is sufficient.”

### Tradeoff 12: snapshot avail index versus continuously draining

**Problem:** work can arrive while the handler is already running.

**Our choice:** read `avail->idx` once and process only that finite snapshot.

**Benefits:** one call has a finite amount of work, so a continuously busy guest
cannot starve other backend duties. Reasoning and tests are deterministic.

**Cost:** requests published after the snapshot wait for the provided backend's
recheck or another kick/call.

**Alternative:** reread avail index after each request and drain until observed
empty, possibly with a fixed budget.

**When the alternative wins:** lower latency under bursts. An unlimited drain
can monopolize the backend; a budgeted drain combines responsiveness with
fairness but adds scheduling policy.

**Model answer:** “The snapshot is a fairness decision, not lost-work behavior.
Later work remains published and is handled by backend recheck or a later
invocation. Continuous draining reduces latency but can starve other work, so a
production compromise might use an explicit batch budget.”

### Tradeoff 13: complete malformed chains versus abandon/reset

**Problem:** the driver transferred a descriptor resource to the device, but
the request contains unsafe metadata and cannot be processed normally.

**Our choice:** emit no unsafe bytes but still publish the original head in the
used ring.

**Benefits:** Linux regains ownership, the queue continues making progress, and
one bad request cannot consume resources forever.

**Cost:** the used entry does not carry an error field, so the driver sees
completion without a detailed reason. Partial valid bytes before a later bad
descriptor may still be emitted under this assignment's contract.

**Alternatives:** leave the item pending, reset the device/queue, expose a
device-status error, or use a protocol with per-request completion status.

**When alternatives win:** security policies that require fail-closed device
shutdown or protocols that define explicit errors. Simply leaving it pending is
usually the worst option because it leaks ownership and may hang the guest.

**Model answer:** “Completion is primarily an ownership/liveness decision. We
cannot safely consume invalid bytes, but we also should not strand the driver's
resource. A production design might report an error or reset, but that requires
protocol support absent from this simplified used element.”

### Tradeoff 14: acquire/release barriers versus locks

**Problem:** driver and device communicate through shared memory. The device
must not observe a published avail index before descriptors are visible, and the
driver must not observe a published used index before the used entry is visible.

**Our choice:** acquire after reading avail idx and release before publishing
used idx.

**Benefits:** precisely expresses the required publication order without an
ordinary lock shared across guest and host processes.

**Limits:** barriers do not validate indices, prevent concurrent access, wake a
peer, or make a multi-field structure atomic. Kicks and interrupts provide
notification; bounds checks provide safety; the virtio ownership protocol
provides the normal access discipline.

**Alternative:** a shared mutex/spinlock designed into both driver and device,
or stronger sequentially consistent atomics for all publications.

**When alternatives win:** a different shared-memory protocol requiring mutual
exclusion. An ordinary backend mutex cannot coordinate with an unmodified Linux
driver. Stronger ordering may simplify proof but can cost performance and still
does not replace a lock or notification.

**Model answer:** “Barriers and locks solve different problems. Virtio needs
publication visibility across a defined ownership protocol, so acquire/release
is appropriate. A mutex would require both sides to participate and would add
exclusion, while still not replacing bounds checks, kicks, or interrupts.”

### How to compare two acceptable designs under pressure

Use a compact matrix rather than claiming one is universally superior:

| Dimension | Ask yourself |
|---|---|
| Safety | Which inputs are untrusted? What new validation is required? |
| Performance | Copies, allocations, MMIO operations, cache effects, batching? |
| Complexity | More states, ownership rules, cleanup, concurrency, parsing? |
| Liveness | Can a guest or request block future progress? |
| Compatibility | Does the guest ABI, driver, linker, or protocol change? |
| Observability | Are errors and intermediate state easy to test/debug? |
| Scalability | What happens with more data, queues, CPUs, or regions? |

An interview-quality conclusion sounds like this:

> “For this assignment's small synchronous workload, choice A minimizes state
> and makes the safety proof explicit. Choice B would be preferable under
> requirement X, but it adds obligations Y and Z. I would not switch without
> also changing these downstream interfaces and tests.”

## 4. Debugging and Modification

### First classify the failure

Before changing code, determine whether the failure is:

- **environment/build:** missing compiler, dependency, or GNU `timeout`;
- **Part I machine construction:** guest cannot boot, use stack/RAM, exit, or
  fault correctly;
- **Part II protocol:** VM works but registers/log commands are wrong;
- **Part III translation:** GPA mapping tests fail before queue tests;
- **Part III traversal/completion:** translation passes but chain or hostile
  tests fail;
- **integration only:** unit tests pass but QEMU/Linux setup, notification, or
  environment is wrong.

Fix the earliest failing dependency. Part II cannot be trusted when Part I
cannot boot a guest, and queue traversal cannot be trusted while address
translation fails.

### Symptom-to-code map

| Symptom | First place to inspect | Likely issue |
|---|---|---|
| all tests say guests did not build | test-suite build output/environment | wrong OS, missing cross compiler |
| Part III runner says `timeout` missing | environment | running Linux-oriented harness on macOS |
| `m_boot` fails | `vmm_create`, `vmm_load_binary`, `vmm_run`, serial callback | RAM/RIP/MMIO/run-loop wiring |
| `m_stack` fails | RSP initialization in `vmm_create` | bad stack address/alignment |
| `m_ram` fails | allocation and `uc_mem_map_ptr` | RAM not writable/shared |
| `m_exit` fails | `serial_write`, `vmm_run` | exit value or powered-off ordering |
| `m_fault` crashes/wrong code | `mem_invalid`, fault hook, fault-first run logic | hook/context/stop/return policy |
| discovery registers wrong | Part II MMIO map/read callback | offset dispatch or constants |
| basic LOG fails | write callback, `run_command`, `cmd_log` | operands, translation, append, counters |
| STAT fails only unaligned case | `cmd_stat` | typed store instead of `memcpy` |
| errors persist after valid command | `clear_error`, `run_command` | stale STATUS bits |
| Part III translation stage fails | `virtq_gpa_to_hva` | gap, wraparound, boundary logic |
| single descriptor fails | handler + `append_data` | avail head, translation, sink, used completion |
| chained descriptor fails | `walk_chain` NEXT logic | index/next/termination error |
| indirect stage fails | INDIRECT branch | entry count, translation, alignment, start index |
| cycle test hangs | hop limit | unbounded traversal |
| Linux logs once then hangs | used completion/event notification | used entry/index or provided event-index path |
| writable leakage test fails | WRITE branch ordering | treated output buffer as input |

### Debugging strategy example: Part II basic LOG fails

1. Confirm Part I boot/RAM/exit tests pass.
2. Confirm guest can read ID/VERSION, proving the device MMIO mapping works.
3. Inspect latched MSG_LO/HI, LEN, LEVEL after guest writes.
4. Confirm CMD offset reaches `run_command(LOG)`.
5. Check whether failure is BADLEN, BADADDR, or an absent append.
6. Test `vmm_gpa_to_host(MSG,LEN)` independently at the same boundary.
7. Confirm counters change only after the append.
8. Compare host log bytes separately from guest STATUS/SEQ output.

Do not start by changing log formatting when the command never reaches
`cmd_log`, or by changing translation when device discovery itself fails.

### Debugging strategy example: virtqueue chain test fails

1. Run translation tests first.
2. Reduce to one descriptor and verify avail head selection.
3. Check index before reading `table[index]`.
4. Snapshot `d` and inspect flags, length, and next.
5. Verify readable descriptors reach `append_data`; WRITE does not.
6. Verify NEXT updates `index` and missing NEXT stops.
7. Verify record length never exceeds 4096.
8. Independently verify used id, used len, barrier, used idx, and `last_avail`.

### Modification scenario 1: change `RAM_BASE`

Relevant locations:

- `emulator/vmm.h` memory-map constant;
- `guest/link.ld` link address;
- initial RIP/load convention;
- tests or guests that assume fixed addresses;
- derived `BOOTINFO_BASE` automatically changes because it uses RAM base.

Downstream consequences:

Changing only `vmm.h` maps RAM and starts RIP at a new address, but the flat
binary remains linked for the old address. Absolute references may be wrong and
the machine ABI becomes inconsistent. The linker layout and VMM must move
together, followed by a complete rebuild of guest binaries.

### Modification scenario 2: support two executable images

Required design work:

- extend the loader API with load address and possibly entry point;
- validate each image range and overlap;
- link or relocate each image for its assigned address;
- decide which image supplies initial RIP;
- define how images call or share data with one another;
- prevent collision with boot-info and stack regions.

Simply calling `vmm_load_binary()` twice is incorrect because both copies begin
at `v->ram+0`, and the second overwrites the first.

### Modification scenario 3: add a new Part II command

Relevant locations:

- command constant in shared `device.h` and corresponding guest header;
- guest wrapper that writes operands/CMD and reads STATUS;
- device state if new operands or counters are needed;
- `run_command()` dispatch and a focused command helper;
- tests for success, malformed inputs, stale-error recovery, and side effects;
- documentation/register ABI.

Downstream consequences to predict:

Define direction explicitly: does the command read guest RAM, write guest RAM,
or neither? That determines translation length, alignment concerns, when
counters change, and whether failure may leave partial side effects.

### Modification scenario 4: increase the record limit

Part II uses `VLOG_MAX_MSG`; Part III uses `VIRTQ_MAX_RECORD`. They are separate
protocol/implementation constants even though both equal 4096 today. Decide
whether the requirement changes one or both. Increasing Part III also increases
stack usage because `rec` is a local array. Update boundary tests and consider
heap allocation or streaming for a much larger maximum.

### Modification scenario 5: recover from an unmapped Part I access

A recoverable fault handler would need to decide which addresses may be mapped,
allocate/initialize backing memory, call the appropriate Unicorn mapping API,
update ownership/cleanup state, and return true so Unicorn retries. Doing this
for arbitrary addresses would violate the fixed machine map and could hide guest
bugs or create security problems. It is appropriate only if the design adds a
real demand-paging policy.

### Modification scenario 6: process Part III without copying

A zero-copy design could pass validated scatter/gather segments directly to a
sink. The sink API would have to accept multiple buffers or streaming chunks.
You must preserve record boundaries, the 4096 cap, GPA validation, and descriptor
ownership while considering that guest-shared memory could change during
processing. It may improve performance but substantially complicates lifetime
and consistency reasoning.

### Modification scenario 7: reject malformed chains without completion

The immediate code change is easy—skip the used-ring write—but the downstream
behavior is dangerous. Linux still believes the device owns the chain, cannot
reuse that resource, and may eventually hang or exhaust the queue. A correct
strict-rejection design needs a defined device-level error/reset mechanism, not
silent abandonment.

### Modification scenario 8: support device writes into WRITE descriptors

You would need a command with host-to-guest semantics, validate writable
descriptor ranges, define exactly which bytes are written, prevent partial
writes on validation failure, and set used `len` to the number of bytes written.
The current transmit logger deliberately skips WRITE descriptors and publishes
used `len=0` because it only consumes guest input.

## 5. Consequence-prediction checklist

Whenever proposing a modification, check all of these:

- Does the guest-visible ABI change?
- Must a provided guest header or Linux-visible feature change too?
- Does the memory map or linker layout change?
- Is the value host-controlled or guest-controlled?
- Is the complete address range still validated before use?
- Does data direction change: guest-to-host or host-to-guest?
- Can failure occur after a partial side effect?
- Which counters/status/completion fields change, and when?
- Does the guest continue, receive an error, wait, or terminate?
- Does concurrency or memory ordering change?
- Do resource ownership and cleanup change?
- Which boundary, malformed-input, and integration tests need updating?

## 6. Timed practice round

Give yourself two minutes per prompt.

1. Explain why `vmm_gpa_to_host()` performs checks in that order.
2. Trace a guest `vlog(WARN, "cache miss")` into the host file.
3. Compare a Part I unmapped access with a Part II BADADDR.
4. Explain why `walk_chain()` needs both an index check and a hop limit.
5. Explain why a valid descriptor can still contribute only a prefix.
6. Propose an ELF loader and name at least four affected assumptions.
7. Diagnose: discovery passes, basic LOG fails with BADADDR.
8. Diagnose: all virtqueue functional tests pass, cycle test hangs.
9. Modify: add a RESET-STATS command and predict every affected file/state.
10. Critique the use of a local 4096-byte record buffer.

### Short answer guide

1. Prevent subtraction underflow, require start inside RAM, then compare length
   with remaining capacity without overflowing an end address.
2. Guest pointer/GPA -> operand registers -> CMD -> callback -> LOG validation
   -> translation -> append -> counters -> STATUS.
3. CPU unmapped access ends the Part I run; BADADDR is a recoverable device
   protocol result because the MMIO access itself was valid.
4. Index check protects memory; hop limit protects time against cycles.
5. Record cap truncates copied data, but the full descriptor must first
   translate so a malformed declared range is not partially trusted.
6. Parser, segments, permissions, load addresses, entry point, overlap, BSS,
   relocation policy, tests.
7. Inspect MSG reconstruction and the complete translated range before touching
   log formatting.
8. Add/repair the table-size hop bound in both direct and indirect traversal.
9. Shared command constant/header, guest wrapper, dispatch/helper, counters,
   STATUS semantics, tests, and documentation; define whether SEQ resets too.
10. Simple hard cap and record assembly versus copies, stack consumption, and
    limited scalability.

## 7. Self-grading rubric

### Implementation Understanding: Good

You can, without notes:

- locate the selected function and its callers/callees;
- explain every parameter and important local variable;
- distinguish guest-controlled from host-controlled data;
- trace normal and failure control flow;
- name state changes and side effects in exact order;
- predict boundary behavior rather than only describing the common case;
- connect the function to the previous and next system layer.

### Design and Tradeoff Reasoning: Good

You can:

- state the requirement behind each major choice;
- explain both benefit and cost;
- identify a realistic implementable alternative;
- say when that alternative is preferable;
- distinguish assignment constraints from production requirements;
- discuss safety, performance, complexity, and liveness where relevant.

### Debugging and Modification: Good

You can:

- classify the failing layer before editing;
- identify the smallest relevant group of files/functions;
- propose observations or tests that distinguish hypotheses;
- avoid masking an earlier dependency failure;
- describe a coherent modification rather than one isolated line;
- predict ABI, state, ownership, safety, test, and performance consequences;
- explain how you would verify both success and malformed-input behavior.

## 8. Final interview habit

When you feel stuck, return to ownership and trust:

```text
Who produced this value?
Which address space or protocol does it belong to?
What must be proven before the host trusts it?
Who must be notified or regain ownership afterward?
```

Those four questions explain most of this project and turn an unfamiliar code
question into a structured engineering answer.
