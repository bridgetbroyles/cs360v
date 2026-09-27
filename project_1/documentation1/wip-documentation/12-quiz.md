# 12. Quiz and Answer Key

Try these after Chapters 0–11. Every abbreviation and identifier used here is
defined earlier. Start with questions 1–19 (Parts I/II), then 20–34 (Part III).
Answers are separated below so you can test recall rather than pattern matching.

## Quiz

1. What is the difference between host and guest?
2. What makes the Parts I/II guest “bare metal”?
3. What service does Unicorn provide, and what does our VMM still provide?
4. Why did this assignment choose a logging device?
5. Define MMIO using the serial device as an example.
6. Why does GVA equal GPA here, and why does neither equal HVA?
7. What roles do the x86-64 registers RIP, RSP, and RDI play, and what are they
   initialized to?
8. What is host-backed guest RAM?
9. What happens when the guest writes value `0x141` to SERIAL_TX?
10. Trace an unmapped instruction fetch to the emulator's exit result.
11. Why check fault state before the Unicorn error in `vmm_run()`?
12. Why does `vmm_gpa_to_host()` check offset before remaining length?
13. Does `vmm_gpa_to_host()` in implementation2 accept a zero-length range
    starting exactly at RAM end?
14. Which device registers persist across commands?
15. Why is CMD called a commit point?
16. Starting at SEQ=4/bytes=20, what does a zero-length level-9 LOG do?
17. Why does every command clear error state first?
18. STAT receives LEN=7 and a valid MSG. What happens?
19. Why does STAT use `memcpy()`?
20. What is QEMU, and how is it different from Unicorn's role here?
21. Why can Linux use Part III without our custom `guest/vlog.c`?
22. Explain the descriptor table, available ring, and used ring, including who writes each.
23. What do NEXT, WRITE, and INDIRECT mean?
24. Why is a WRITE descriptor skipped on this queue?
25. What two separate bounds protect a descriptor chain?
26. How does implementation2 prevent nested indirect recursion?
27. What happens to remainder bytes when indirect LEN is not a descriptor-size multiple?
28. Why check indirect-table HVA alignment?
29. What happens when an avail head is out of range?
30. What happens when valid bytes are followed by an unmapped descriptor?
31. Why is the used length zero even after reading a large request?
32. What does `virtq_rmb()` order? What does it not do?
33. Why snapshot `avail->idx` once?
34. Name four improvements for production code.

## Answer Key

1. Host runs virtualization; guest runs inside the virtual machine.
2. It has no guest OS/services and talks directly to the machine/MMIO ABI.
3. Unicorn executes x86-64 instructions; our code supplies RAM layout, image,
   device callbacks, entry state, fault policy, and lifecycle.
4. It is a small data-transfer device that teaches address validation,
   completion, and host side effects—the same issues as larger devices.
5. Special addresses invoke device behavior. Store at serial offset zero prints
   rather than changing RAM.
6. Paging is disabled, so guest translation is identity. Host is a separate
   address space backed by `v->ram`.
7. RIP selects the next instruction and equals RAM_BASE; RSP is the stack pointer
   and equals BOOTINFO_BASE-16; RDI holds the first argument and equals
   BOOTINFO_BASE.
8. A host allocation mapped into Unicorn so CPU and device share the bytes.
9. Masking leaves `0x41`, so it prints `A`.
10. Unicorn hook -> record fault/address -> stderr -> stop -> false -> run returns 77.
11. The fault hook may also produce a Unicorn error; fault 77 is the semantic cause.
12. It prevents subtraction underflow and makes remaining-space math safe.
13. No; `offset >= RAM_SIZE` rejects it.
14. MSG_LO, MSG_HI, LEN, LEVEL.
15. Operand writes only latch state; writing CMD executes synchronously.
16. Appends `[4] LVL? \n`, SEQ becomes 5, bytes stays 20, MSG ignored.
17. `set_error` ORs bits and successful commands must clear stale errors.
18. BADLEN; no guest write, log, or counter change.
19. Guest output may be unaligned; byte copying avoids undefined typed access.
20. QEMU runs a complete Linux VM/virtio platform; Unicorn executes the tiny
   guest CPU inside our own minimal VMM.
21. Linux already includes the standardized virtio console driver.
22. Descriptors describe buffers/chains; avail publishes heads; used returns
   completed heads.
23. Continue chain; device-writable buffer; pointer to secondary table.
24. It grants device output space, not guest-to-device input; reading can leak data.
25. Index `< table_size` and hop count `< table_size`.
26. Recursive call passes `allow_indirect=0`; nested indirect contributes nothing.
27. Division truncates; only complete-descriptor prefix is translated/walked.
28. The walker dereferences typed structs; unaligned typed access can be undefined.
29. Walker returns empty; sink ignores it; handler still posts used completion.
30. Already gathered bytes remain; invalid descriptor adds nothing; request can emit.
31. Used LEN counts bytes the device wrote to writable guest buffers; there were none.
32. It acquire-orders reads after observed avail idx; it does not validate, lock,
   or itself read any descriptor.
33. To process a finite batch; later work waits for another call.
34. Cleanup on setup failure, check all Unicorn calls, validate host pointers and
   flag combinations, explicit reset/errors, concurrency, event-index support,
   zero-copy/scatter-gather processing.
