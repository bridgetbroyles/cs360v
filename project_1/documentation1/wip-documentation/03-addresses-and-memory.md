# 3. Addresses, Memory, and Safety

This chapter gives precise names to Chapter 0's address conversion:

- **GVA** means guest virtual address, used by a guest instruction/program.
- **GPA** means guest physical address, a location in the VM's hardware map.
- **HVA** means host virtual address, a C pointer used by the host process.

## Why there are several kinds of address

An address is a location within one machine's view of memory. Virtualization
creates multiple views.

| Term | Meaning here | Example | Host may dereference directly? |
|---|---|---:|---|
| GVA | address used by guest instructions | guest pointer `0x00102000` | No |
| GPA | location in the virtual machine's physical map | message address or descriptor `addr` | No |
| HVA | real pointer in host process | `v->ram + 0x2000` | Yes, after validation |
| MMIO address | GPA assigned to device behavior | `0x2000000c` | No |
| MMIO offset | address minus region base | `0x0c` (`CMD`) | Not a pointer |

## Why GVA equals GPA in Parts I and II

Modern operating systems normally use page tables to translate virtual to
physical addresses. Our bare-metal VMM does not enable paging, so the CPU uses
identity translation: guest virtual address equals guest physical address.

That does **not** make it a host pointer. The host allocation can live anywhere.
`v->ram` is a host pointer returned by `calloc`. `uc_mem_map_ptr()` tells Unicorn
that this allocation backs the GPA range beginning at `RAM_BASE`.

## Part I memory map

```text
0x00100000  RAM_BASE
             16 MiB: code, globals, heap-like space, stack
0x010F0000  BOOTINFO_BASE
             64 KiB reserved boot-info area
0x01100000  end of RAM

0x10000000  serial/control MMIO (4 KiB)
0x20000000  logging-device MMIO (4 KiB)
```

RIP is the CPU instruction pointer, RSP is its stack pointer, and RDI holds the
first function argument in the x86-64 calling convention. Initially they are
RIP=`0x00100000`, RSP=`0x010EFFF0`, and RDI=`0x010F0000`.

## Translating safely in `vmm_gpa_to_host()`

The implementation checks:

```text
gpa >= RAM_BASE
offset = gpa - RAM_BASE
offset < RAM_SIZE
len <= RAM_SIZE - offset
```

Only then does it return `v->ram + offset`. Requiring `offset < RAM_SIZE` means
even a zero-length request starting exactly at RAM end is rejected. A zero-length
range starting inside RAM is accepted, although Part II bypasses translation for
empty LOG messages.

The subtraction form avoids overflow. If code instead computed `gpa + len`, a
value near `UINT64_MAX` could wrap to a small number and fool an upper-bound test.

## Part III memory is fragmented

In Part III, the provided backend describes QEMU-shared guest RAM with several
`struct virtq_mem_region` entries. Each region
says GPA `[region.gpa, region.gpa + region.size)` corresponds to host pointer
`region.hva`. There may be gaps. `virtq_gpa_to_hva()` requires a nonempty range
to fit completely within one region.

Example:

```text
region 0 GPA [0x40000000, 0x40008000)
gap
region 1 GPA [0x50000000, 0x50008000)
```

A 64-byte request beginning eight bytes before region 0 ends is invalid. It may
not jump the gap or combine separate host allocations.

This branch explicitly checks `gpa + len < gpa` for wraparound, then also uses
remaining-space subtraction inside a region. It returns NULL immediately when a
range starts in a region but runs past that region.

## Trust boundary

In a virtqueue, a descriptor describes a guest buffer, `next` links another
descriptor, and an available-ring head identifies a submitted chain's first
descriptor. Those values, message addresses, lengths, flags, and indirect-table
contents are guest-controlled. Host-provided
objects such as `vq`, `mem`, ring pointers, and sink are assumed valid by this
implementation because `backend.c` constructs them. This is important: the code
defends against hostile guest metadata, but it does not null-check every trusted
host plumbing pointer.

## Interview answer

“A guest address is a number in the guest's address space, not a host pointer.
Our translators find the corresponding host backing region and prove the whole
half-open range fits before returning an HVA. Part I has one contiguous region;
Part III searches QEMU's region table. Remaining-space checks prevent both
out-of-bounds access and unsigned-addition overflow.”
