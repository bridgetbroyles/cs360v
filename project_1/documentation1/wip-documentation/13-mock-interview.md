# 13. Mock Interview

Read the implementation chapters first. For each prompt, practice the ideal
answer aloud rather than memorizing words. “Testing” means the knowledge the
interviewer is evaluating; “weak” shows what is missing; “3/3” lists required
content; “ideal” is a model spoken answer.

## “What did you build?”

**Testing:** architecture. **Weak:** “A VM that logs.” **A 3/3 answer includes:**
the two separate logging paths, bare-metal versus Linux guests, Unicorn versus
QEMU, custom MMIO versus virtio,
translation and completion.

**Ideal:** “All parts deliver guest log messages to a host file, but at different
layers. We built a minimal Unicorn VMM and a custom MMIO logging device for
a bare-metal x86-64 guest, then implemented the same logging data path as a
vhost-user virtio console backend for a real Linux guest under QEMU. Both paths
validate guest-described memory before host access; the custom device completes
through status/sequence state, while virtio completes through the used ring.”

## “What exactly is a bare-metal guest?”

**Testing:** fundamentals. **Weak:** “A lightweight VM.” **3/3:** no guest OS,
freestanding binary, direct machine ABI.

**Ideal:** “It is machine code running without Linux underneath. Our startup
assembly enters C, but there are no system calls or standard devices. It prints,
logs, and exits by writing addresses our VMM defines as MMIO.”

## “Walk through `vmm_create()`.”

**Testing:** setup/state. **Weak:** “It starts Unicorn.” **3/3:** ordered list of
store, engine, RAM/map, serial, device, registers, hooks and limitations.

**Ideal:** State that `v` points to the central VMM state, then give the 11-step
sequence in Chapter 4 and explain host-backed RAM
and distinct callback contexts. Mention that this branch does not unwind most
partial failures or check RSP/RDI writes.

## “Why not cast MSG to a pointer?”

**Testing:** address spaces/security. **Weak:** “It may crash.” **3/3:** GPA is
guest-controlled and belongs to another map; whole-range translation.

**Ideal:** “The numeric GPA has no direct host meaning. We prove its offset and
length lie inside host-backed guest RAM, then add that offset to the HVA base.
Otherwise a guest could select unrelated host memory or overflow the range.”

## “Explain LOG versus STAT.”

**Testing:** direction/side effects. **Weak:** “LOG writes a log and STAT reads
stats.” **3/3:** LOG host-reads guest and changes counters; STAT host-writes
guest, checks offered length, uses memcpy, changes no counters.

## “What is QEMU doing in Part III?”

**Testing:** Part III mental model. **Weak:** “It runs virtio.” **3/3:** complete
Linux VM, advertises console, shares RAM/queue through provided vhost-user layer.

**Ideal:** “QEMU replaces our small VMM. It boots Linux and presents a virtio
console device. Linux prepares standard virtqueue metadata in guest RAM. QEMU's
vhost-user connection gives our external backend access to those regions and
notifications; our code implements queue consumption, not CPU emulation.”

## “Draw and explain a virtqueue.”

**Testing:** descriptor/avail/used ownership. **Weak:** “It is a ring buffer.”
**3/3:** three structures, heads/chains, free-running indices, completion.

**Ideal:** Draw chapter 6's diagram; explain descriptors point to GPA buffers,
avail transfers work to device, used transfers ownership back to driver.

## “Walk through `walk_chain()`.”

**Testing:** exact code. **Weak:** “It follows next.” **3/3:** hop and index
bounds, local snapshot, indirect recursion guard, WRITE skip, translation/cap.

**Ideal:** Include implementation order—INDIRECT before WRITE—and the fact that
indirect length truncates to complete entries and requires aligned HVA.

## “Why complete a malformed chain?”

**Testing:** liveness tradeoff. **Weak:** “Because the tests expect it.” **3/3:**
driver ownership and alternative reset/error policy.

**Ideal:** “Completion returns the descriptor resource. If we silently consume
without posting used, the driver may wait forever. This interface has no per-item
error field, so we log no invalid bytes but still return the head. A production
device might instead reset or report a device-level error.”

## “Critique this implementation.”

**Testing:** design maturity. **Weak:** “It passes.” **3/3:** strengths and exact
limitations.

**Ideal:** “It has strong guest-data safety for the assignment: bounded indices,
bounded hops, full-range GPA translation, direction checks, record cap, alignment
check, and ordered completion. It trusts backend pointers, ignores indirect
remainder bytes, treats INDIRECT before WRITE, copies through a stack buffer, and
has incomplete VMM setup cleanup/checking. Production code would harden those.”
