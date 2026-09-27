# 2. Repository and How to Use It

This chapter is operational. Read Chapters 0–1 first so “host,” “bare-metal
guest,” and “QEMU” are familiar. Run the project inside the Ubuntu development
VM described by `SETUP.md`. That Ubuntu VM is the development environment; this
project then runs another guest inside Unicorn or QEMU.

## Important directories

| Path | Role |
|---|---|
| `emulator/` | host program for Parts I/II: VMM, custom device, log store |
| `guest/` | code compiled into the bare-metal program run by Unicorn |
| `vhost/` | separate Part III backend and QEMU integration files |
| `tests/` | guest tests, random oracle, virtqueue harnesses, expected output |
| `SPEC.md` | authoritative required behavior |
| `SETUP.md` | authoritative build/run instructions |

## Build Parts I and II

From `project_1`:

```bash
cd emulator
make
cd ../guest
make
```

`make` reads the Makefile in its current directory. The first command compiles
host code and links Unicorn, producing `emulator/emulator`. The second compiles
guest C/assembly without an OS, links it at the expected guest address, and
converts it into flat `.bin` images such as `test_log.bin`.

Run the sample logger from `project_1/guest`:

```bash
../emulator/emulator --log out.log test_log.bin
echo $?
cat out.log
```

`--log out.log` selects a host output file; `test_log.bin` is the guest image.
Exit status 0 means clean guest poweroff. The log should contain four numbered
records. To trace each guest instruction to stderr:

```bash
../emulator/emulator --trace test_nop.bin 2>trace.log
```

## Run the full suite

```bash
cd project_1/tests
./run_tests.sh
```

This performs the graded workflow: builds the submitted sources, builds test
guests, runs Part I micro-tests, runs deterministic/random Part II scenarios,
and runs staged plus adversarial Part III tests. Increase random coverage with:

```bash
P1_SEEDS=20 ./run_tests.sh
```

The suite does not have separate Part I/II scripts. Its output has separate
sections. The Part I tests run in dependency order:

- `m_boot`: entry point, basic RAM, serial, run loop, poweroff;
- `m_stack`: initial RSP supports C;
- `m_ram`: writable host-backed RAM;
- `m_exit`: exit-code propagation;
- `m_fault`: controlled unmapped-memory fault.

Tests whose names begin `t_` cover deterministic Part II behavior. The `oracle`
host program generates randomized inputs and expected results. Fix the first
Part I failure before
debugging later device failures because later guests depend on a working VMM.

To inspect one deterministic test manually:

```bash
cd project_1/tests
make
../emulator/emulator --log /tmp/test.log m_stack.bin > /tmp/test.obs
diff -u expected/m_stack.obs /tmp/test.obs
```

For tests with expected logs, also diff `/tmp/test.log`. `m_exit` and `m_fault`
intentionally use nonzero statuses listed in `expected/*.exit`.

## Run Part III tests

```bash
cd project_1/tests
./run_virtq_tests.sh
```

This does not boot Linux. It builds a normal host test program that creates fake
guest memory and queue structures, calls our functions directly, and runs four stages:
translation, single descriptor, chained descriptors, and indirect descriptors.
Success is `16 passed, 0 failed`. The full suite adds hostile-ring sanitizer
tests such as cycles, invalid heads, gaps, and writable-data leakage.

## Run real QEMU integration

```bash
cd project_1/vhost
./run-qemu.sh
```

An **initramfs** is a small in-memory filesystem Linux uses during boot. This
optional script builds `vlog-backend`, creates a tiny initramfs, starts the
backend on a Unix socket, and boots a real Linux kernel in QEMU with a virtio
console device. It prints the actual and expected four-record logs. If the guest
hangs after one write, inspect used-ring handling. If it reports no readable
kernel, fix the `/boot/vmlinuz-*` permissions or set `KERNEL=`. Guest output is
saved in `vhost/qemu.out`.

## Clean, inspect, and submit

```bash
make -C project_1/emulator clean
make -C project_1/guest clean
make -C project_1/tests clean
make -C project_1/vhost clean
```

“Clean” removes generated objects and executables, not source. Then rebuild and
run the suite. Review only the deliverables:

```bash
git status --short
git diff --check
git diff -- project_1/emulator/vmm.c \
  project_1/emulator/device.c project_1/vhost/virtio.c
```

Submit exactly those three C files using the course submission system. There is
no Project 1 submission script in this branch.

## From-scratch checklist

- Set up and enter the course Ubuntu VM.
- Read `SPEC.md` before coding.
- Build `emulator/`, `guest/`, and `tests/`.
- Run VMM micro-tests in order.
- Run all device/random tests.
- Run `run_virtq_tests.sh` and hostile tests through `run_tests.sh`.
- Optionally run `run-qemu.sh`.
- Review diff and submit only the three implementation files.
