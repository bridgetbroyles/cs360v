# 10. Design Decisions and Tradeoffs

A **design tradeoff** compares two valid approaches that optimize different
qualities such as simplicity, performance, safety, or extensibility. For an
interview, do not merely say our choice is “better”: state the requirement it
serves, its cost, and when the alternative would be preferable.

Terms such as “host-backed RAM,” “descriptor snapshot,” and “used completion”
are explained in Chapters 3–7. The table intentionally comes after them.
“Zero-copy” means processing data directly in its existing buffer rather than
first copying it into a second buffer.

| Choice in implementation2 | Why | Alternative | Tradeoff/interview answer |
|---|---|---|---|
| Host-backed contiguous Part I RAM | one allocation shared by CPU/device | Unicorn-owned RAM + `uc_mem_read` | ours is simple/zero-copy; API reads isolate memory better but copy |
| Separate MMIO callbacks | clear device-specific state | centralized dispatcher | ours reduces coupling; dispatcher scales shared policies |
| Stop on unmapped access | fixed teaching map, deterministic fault | demand-map and retry | alternative suits paging/lazy allocation, not this ABI |
| Remaining-space range check | avoids overflow | checked addition | both valid; ours encodes “bytes left” directly |
| Stateful operand registers | hardware-like and specified | command struct in RAM | ours needs several MMIO writes but makes registers observable |
| Command helpers in `device.c` | isolates LOG/STAT validation | one large switch | helpers improve readability; more function boundaries |
| Local STAT + `memcpy` | safe for unaligned guest output | typed store | typed store is shorter but may be undefined when unaligned |
| Clear error before dispatch | `set_error` ORs bits | assign complete status | ours preserves READY and supports recovery |
| Local 4096-byte virtio record | hard memory cap, easy concatenate | dynamic/zero-copy scatter-gather | ours copies/uses stack; alternative is faster but complex |
| Reusable recursive `walk_chain` | same validation for direct/indirect | two separate loops | ours avoids duplication; recursion requires explicit nesting guard |
| Descriptor snapshot | consistent fields per hop | reread shared struct | snapshot narrows guest-race inconsistencies |
| Table-size hop cap | prevents cycles allocation-free | visited bitmap | bitmap detects revisits sooner but needs storage |
| Snapshot avail idx | finite batch | continuously reread | ours avoids starvation; new work waits for next call |
| Complete malformed chains | driver never waits forever | reset device/reject queue | ours favors liveness; production could expose explicit errors |
| Memory-order barriers, not locks | order shared ring publication across VM boundary | mutex | guest cannot share our ordinary process mutex; a mutex is heavier and solves a different problem |

## Limitations worth admitting

“Production” means code intended to run untrusted workloads reliably for long
periods, not just satisfy this fixed teaching contract. This branch is correct
for the assignment tests but is not a production hypervisor. `vmm_create()` does
not unwind most partial failures; some Unicorn
register/hook calls are unchecked. The queue handler assumes valid host-side
pointers. Indirect length remainder bytes are ignored, and INDIRECT takes
precedence over WRITE when both malformed flags are set. Production code would
validate negotiated features/flag combinations, define device reset/error
behavior, handle concurrency rigorously, and optimize copies/notifications.

Admitting this is strong engineering reasoning: it distinguishes required guest
safety from broader lifecycle and production-hardening concerns.
