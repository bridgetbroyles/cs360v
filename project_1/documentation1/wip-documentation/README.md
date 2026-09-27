# Project 1 Study Guide — `claude/implementation2`

This guide describes the code on the `claude/implementation2` branch. It is
ordered as a course: each chapter establishes vocabulary before relying on it.

Do not begin with a random implementation chapter. Chapters 0–1 establish the
central story; Chapter 3 explains address vocabulary; Chapters 4–7 introduce
exact functions and fields only after explaining the system role they serve.
Reference chapters 8–15 intentionally become progressively more concise.

Chapters 4, 5, and 7 are the source-guided implementation chapters. They name
the exact files, distinguish starter-provided code from our work, and include C
blocks for each project function they explain. Later reference chapters point
back to those explanations instead of duplicating every full function body.

## Recommended reading order

1. [Virtualization Fundamentals](00-virtualization-fundamentals.md)
2. [Big Picture and Why This Project Exists](01-big-picture.md)
3. [Repository and How to Use It](02-repository-usage.md)
4. [Addresses, Memory, and Safety](03-addresses-and-memory.md)
5. [Part I — The Unicorn VMM](04-part-i-vmm.md)
6. [Part II — The Custom MMIO Logging Device](05-part-ii-device.md)
7. [Part III Fundamentals — QEMU, Virtio, and Virtqueues](06-part-iii-fundamentals.md)
8. [Part III Implementation — `vhost/virtio.c`](07-part-iii-implementation.md)
9. [Critical Data Structures](08-data-structures.md)
10. [End-to-End Execution Traces](09-execution-traces.md)
11. [Design Decisions and Tradeoffs](10-design-tradeoffs.md)
12. [Code-Level “Explain This” Preparation](11-code-explanations.md)
13. [Quiz and Answer Key](12-quiz.md)
14. [Mock Interview](13-mock-interview.md)
15. [Common Mistakes](14-common-mistakes.md)
16. [Final Cheat Sheet](15-final-cheat-sheet.md)

## How to study

- First pass: read chapters 0 and 1 in full, then chapters 3, 6, and 9. Aim to
  understand the story before memorizing names.
- Second pass: chapters 4, 5, and 7 with the source files open.
- Third pass: answer chapter 12 without notes, then practice chapter 13 aloud.
- Final review: chapters 14 and 15.

The specification remains authoritative. When this guide discusses a design
limitation, it distinguishes “correct for this assignment” from “what a
production hypervisor might do.”
