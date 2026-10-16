#!/usr/bin/env python3
"""ukdeploy: build and deploy the runner.c unikernel.

    ukdeploy.py build [--arch ARCH]
    ukdeploy.py deploy [--name NAME] [--port PORT] [--arch ARCH]
    ukdeploy.py list [--json]
    ukdeploy.py stop (NAME | --all)
    ukdeploy.py bench [--count N] [--arch ARCH]

An "instance" is one running unikernel: a QEMU process, a port on this machine
that QEMU forwards to the runner inside it, and a log file of its console output.

The list of instances is saved in app/.ukdeploy/state.json, so `list` and `stop`
know about instances an earlier `deploy` started. load_state() returns it as a
dict shaped like this, and save_state(state) writes it back:

    {"instances": {
        "a": {"pid": 12345, "port": 9000, "image": "...", "log": "...",
              "arch": "x86_64", "started": 1700000000.0},
        ...
    }}

PROVIDED for you below: the command-line parsing, the state file helpers,
find_image(), and the QEMU settings for each architecture. You implement the
commands: building, picking a port, booting an instance and waiting until it is
ready, listing what is running, and stopping instances.
"""
import argparse
import errno
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.environ.get("UKDEPLOY_APP", os.path.join(os.path.dirname(HERE), "app"))
STATE_DIR = os.path.join(APP_DIR, ".ukdeploy")
STATE_FILE = os.path.join(STATE_DIR, "state.json")
LOG_DIR = os.path.join(STATE_DIR, "logs")

RUNNER_PORT = 8080          # the port the runner listens on INSIDE the unikernel
PORT_RANGE = (9000, 9100)   # host ports you may hand out
READY_TIMEOUT = 60.0        # seconds to wait for the runner to answer PING

# PROVIDED. QEMU settings for each architecture. Use these values as they are:
# the unikernel does not boot with other -cpu settings.
ARCHES = {
    "x86_64": {"qemu": "qemu-system-x86_64", "cpu": "host", "fallback_cpu": "max"},
    "arm64":  {"qemu": "qemu-system-aarch64", "cpu": "max", "fallback_cpu": "max",
               "extra": ["-machine", "virt"]},
}


def host_arch():                                            # PROVIDED
    m = os.uname().machine
    return "arm64" if m in ("aarch64", "arm64") else "x86_64"


# ---- state helpers (PROVIDED) --------------------------------------------

def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"instances": {}}


def save_state(state):
    os.makedirs(STATE_DIR, exist_ok=True)
    tmp = STATE_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, STATE_FILE)      # atomic: a crash cannot truncate state


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError as e:
        return e.errno == errno.EPERM


def find_image(arch):
    """The bootable image kraft produced, or None. (PROVIDED)"""
    build = os.path.join(APP_DIR, ".unikraft", "build")
    if not os.path.isdir(build):
        return None
    suffix = f"_qemu-{arch}"
    for f in sorted(os.listdir(build)):
        # the bootable image has no extension; skip .dbg/.bootinfo/.cmd/...
        if f.endswith(suffix) and os.path.isfile(os.path.join(build, f)):
            return os.path.join(build, f)
    return None


# ---- implementation -------------------------------------------------------

# TODO (implemented): remove dead QEMU instances from saved state.
def reap_dead(state):
    """Remove entries whose saved QEMU PID is no longer alive and return state."""
    # Build a separate list because changing a dictionary during iteration is unsafe.
    dead_names = [
        # Keep the name when its saved QEMU PID no longer belongs to a live process.
        name
        for name, entry in state["instances"].items()
        if not alive(entry["pid"])
    ]

    # Remove each stale entry so it no longer reserves its name or port.
    for name in dead_names:
        # Delete the dead instance from the in-memory state dictionary.
        del state["instances"][name]

    # Give the cleaned dictionary back to the command that loaded it.
    return state


# TODO (implemented): check whether a host TCP port can be assigned.
def port_free(port):
    """Return True when a temporary socket can bind the requested host port."""
    try:
        # Create a temporary IPv4 TCP socket solely to test the requested port.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as test_socket:
            # Allow a recently stopped instance's port to be reused promptly.
            test_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

            # Binding succeeds only when no other listener currently owns the port.
            test_socket.bind(("127.0.0.1", port))
    except OSError:
        # A bind error means this host port cannot be assigned to a new instance.
        return False

    # Reaching this point means the temporary bind succeeded and has been closed.
    return True


# TODO (implemented): select an unused deployment port.
def pick_port(state):
    """Return the first allowed port that is neither reserved nor in use."""
    # Collect ports already assigned to live instances in the saved state.
    reserved_ports = {
        # Read the host-side forwarded port from each instance record.
        entry["port"]
        for entry in state["instances"].values()
    }

    # Examine every allowed host port in ascending order.
    for port in range(*PORT_RANGE):
        # Skip a port that a saved live instance already owns.
        if port in reserved_ports:
            continue

        # Return the first unreserved port that the operating system can bind.
        if port_free(port):
            return port

    # None tells the caller that the complete configured range is unavailable.
    return None


# TODO (implemented): test whether a runner answers through its forwarded port.
def ping(port, timeout=2.0):
    """Send PING to the runner and return whether its response contains PONG."""
    try:
        # Open a short-lived TCP connection through QEMU's forwarded host port.
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as conn:
            # Send one complete runner command terminated by the required newline.
            conn.sendall(b"PING\n")

            # Read the runner's small one-line response.
            reply = conn.recv(64)
    except OSError:
        # Refusal, timeout, reset, or another socket error means it is not ready.
        return False

    # Only a response containing PONG proves that the runner handled the request.
    return b"PONG" in reply


# TODO (implemented): wait for a newly booted instance to answer PING.
def wait_ready(port, pid, timeout=READY_TIMEOUT):
    """Poll until the runner responds, QEMU exits, or the timeout expires."""
    # Convert the relative timeout into one absolute stopping time.
    deadline = time.time() + timeout

    # Keep trying while the caller's readiness window remains open.
    while time.time() < deadline:
        # Stop immediately if QEMU exited before its runner became reachable.
        if not alive(pid):
            return False

        # A successful PING proves both forwarding and the runner are ready.
        if ping(port):
            return True

        # Pause briefly so boot polling does not busy-loop and waste CPU.
        time.sleep(0.3)

    # The unikernel never answered before the readiness deadline.
    return False

# added helper
def qemu_command(image, arch, port):
    """Build the exact QEMU argument list used by deploy and bench."""
    # Look up the provided executable and CPU settings for this architecture.
    spec = ARCHES[arch]

    # Start with the arguments shared by both supported architectures.
    argv = [
        # Launch the QEMU executable selected for the requested architecture.
        spec["qemu"],
        # Boot the unikernel image directly instead of attaching a disk image.
        "-kernel", image,
        # Send the guest console through the log file instead of a graphical window.
        "-nographic",
        # Exit QEMU when the unikernel stops instead of booting it again.
        "-no-reboot",
        # Give this small unikernel 128 MiB of guest memory.
        "-m", "128M",
    ]

    # Hardware acceleration is usable only for the host's own architecture and KVM.
    kvm_available = (
        # Cross-architecture guests cannot run through the host's KVM interface.
        arch == host_arch()
        # KVM must be readable and writable by the current user.
        and os.access("/dev/kvm", os.R_OK | os.W_OK)
    )

    # Select the accelerated CPU configuration when KVM is available.
    if kvm_available:
        # Enable KVM and use the architecture's provided accelerated CPU model.
        argv.extend(["-enable-kvm", "-cpu", spec["cpu"]])
    else:
        # Use QEMU's software-emulation CPU when acceleration is unavailable.
        argv.extend(["-cpu", spec["fallback_cpu"]])

    # Add architecture-specific options such as ARM's virt machine model.
    argv.extend(spec.get("extra", []))

    # Forward the chosen host port to the runner's fixed port inside the guest.
    argv.extend([
        # Create QEMU's user-mode network and its host-to-guest TCP forwarding rule.
        "-netdev", f"user,id=n0,hostfwd=tcp::{port}-:{RUNNER_PORT}",
        # Attach a virtio network device to the user-mode network defined above.
        "-device", "virtio-net-pci,netdev=n0",
    ])

    # Return one reusable command so deploy and bench boot guests identically.
    return argv

# added helper
def signal_process_group(pid, sig):
    """Send one signal to an instance's complete QEMU process group."""
    try:
        # Find the group created for QEMU by Popen(start_new_session=True).
        process_group = os.getpgid(pid)

        # Signal the whole group so QEMU cannot leave helper processes behind.
        os.killpg(process_group, sig)
    except OSError:
        # The instance may have exited between the liveness check and the signal.
        return False

    # True tells the caller that the signal request reached an existing group.
    return True

# added helper
def start_qemu(name, image, arch, port):
    """Start one QEMU instance and return it only after PING succeeds."""
    # Ensure the directory for this instance's captured console output exists.
    os.makedirs(LOG_DIR, exist_ok=True)

    # Give each named instance a predictable log path for debugging failures.
    log_path = os.path.join(LOG_DIR, f"{name}.log")

    try:
        # Open in binary mode because Popen writes QEMU's raw output bytes here.
        log = open(log_path, "wb")
    except OSError as error:
        # Report a clean command-line error instead of exposing a Python traceback.
        print(f"error: cannot open log {log_path}: {error}", file=sys.stderr)
        return None, log_path

    # Construct the architecture-correct QEMU invocation and forwarding rule.
    argv = qemu_command(image, arch, port)

    try:
        # Start QEMU with no terminal input and with all output captured in its log.
        proc = subprocess.Popen(
            # Pass the already separated QEMU arguments without invoking a shell.
            argv,
            # Store both standard output and standard error in the console log.
            stdout=log,
            stderr=subprocess.STDOUT,
            # Prevent QEMU from inheriting this deployment tool's standard input.
            stdin=subprocess.DEVNULL,
            # Create the process group used later by stop and failure cleanup.
            start_new_session=True,
        )
    except OSError as error:
        # Close the log because no QEMU process inherited its descriptor.
        log.close()

        # Explain startup failures, such as a missing QEMU executable.
        print(f"error: cannot start QEMU: {error}", file=sys.stderr)
        return None, log_path

    # QEMU inherited its own descriptor, so this parent copy can now be closed.
    log.close()

    # Do not call the instance ready until the runner answers through forwarding.
    if not wait_ready(port, proc.pid):
        # Force the entire failed QEMU process group to stop.
        signal_process_group(proc.pid, signal.SIGKILL)

        try:
            # Reap this direct child so the deployment tool does not leave a zombie.
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            # A SIGKILLed process should exit promptly; continue cleanly if it does not.
            pass

        # Point the user to the console output that explains the boot failure.
        print(f"error: instance did not become ready; see {log_path}",
              file=sys.stderr)
        return None, log_path

    # Return the live process and its log only after the PING readiness check passes.
    return proc, log_path


# TODO (implemented): build the unikernel image for the requested architecture.
def cmd_build(args):
    """Run KraftKit, verify that it produced an image, and report its path."""
    # Refuse to start a build when the required KraftKit command is unavailable.
    if shutil.which("kraft") is None:
        # Send the actionable failure message to standard error.
        print("error: kraft is not installed or not in PATH", file=sys.stderr)
        return 1

    # Use every detected host CPU, while falling back to one if detection fails.
    jobs = os.cpu_count() or 1

    # Build the exact noninteractive KraftKit command required by the specification.
    argv = [
        # Run the KraftKit command-line tool.
        "kraft",
        # Ask KraftKit to compile the application image.
        "build",
        # Select QEMU as the platform that will boot the completed image.
        "--plat", "qemu",
        # Build for the architecture selected on this command line.
        "--arch", args.arch,
        # Reuse the package index installed during VM setup instead of downloading it.
        "--no-update",
        # Prevent an unanswered interactive prompt from hanging an automated build.
        "--no-prompt",
        # Parallelize compilation across the available host CPUs.
        "-j", str(jobs),
    ]

    # Run from APP_DIR so KraftKit reads this project's Kraftfile and Makefile.uk.
    result = subprocess.run(argv, cwd=APP_DIR)

    # Propagate a clean failure when KraftKit reports that compilation failed.
    if result.returncode != 0:
        # Explain which high-level operation failed without hiding KraftKit's output.
        print("error: kraft build failed", file=sys.stderr)
        return 1

    # Locate the extensionless QEMU image produced for the requested architecture.
    image = find_image(args.arch)

    # A zero build exit without an image is still an unsuccessful build command.
    if image is None:
        # Tell the user that the expected build artifact could not be found.
        print("error: build completed but produced no bootable image", file=sys.stderr)
        return 1

    # Print the artifact path so the user knows exactly what deploy will boot.
    print(image)

    # Return a successful shell exit status.
    return 0


# TODO (implemented): deploy one named unikernel instance and save its state.
def cmd_deploy(args):
    """Choose a name and port, start QEMU, wait for PING, and record the instance."""
    # Load saved instances and discard entries whose QEMU process has died.
    state = reap_dead(load_state())

    # Persist stale-entry removal even if a later validation step fails.
    save_state(state)

    # Find the image built for the architecture requested by this deployment.
    image = find_image(args.arch)

    # Deploy cannot boot anything until the build command has produced an image.
    if image is None:
        # Give the user the exact recovery action instead of allowing a traceback.
        print("error: no image found; run ukdeploy.py build first", file=sys.stderr)
        return 1

    # Use the requested name when one was supplied.
    name = args.name

    # Generate the first unused runnerN name when the caller omitted --name.
    if name is None:
        # Start generated names at runner1 for predictable list output.
        number = 1

        # Continue until the generated name is absent from live saved state.
        while f"runner{number}" in state["instances"]:
            # Move to the next candidate name after a collision.
            number += 1

        # Save the available generated name for the new instance.
        name = f"runner{number}"

    # Refuse to overwrite the state record for a live instance of the same name.
    if name in state["instances"]:
        # Explain the collision and leave the existing instance unchanged.
        print(f"error: instance {name!r} already exists", file=sys.stderr)
        return 1

    # Collect ports reserved by the live instances already recorded in state.
    reserved_ports = {
        # Read the host forwarding port from each instance record.
        entry["port"]
        for entry in state["instances"].values()
    }

    # Handle an explicitly requested port without silently choosing another one.
    if args.port is not None:
        # Reject either a saved reservation or a port currently bound by any process.
        if args.port in reserved_ports or not port_free(args.port):
            # Include the exact conflicting port in the command-line error.
            print(f"error: port {args.port} is already in use", file=sys.stderr)
            return 1

        # The requested port passed both state and operating-system checks.
        port = args.port
    else:
        # Ask the shared allocator for the first free port in PORT_RANGE.
        port = pick_port(state)

        # None means every allowed port was reserved or already bound.
        if port is None:
            # Tell the user why an automatically assigned port was unavailable.
            print("error: no free deployment ports are available", file=sys.stderr)
            return 1

    # Start QEMU and wait until the runner answers PING on the forwarded port.
    proc, log_path = start_qemu(name, image, args.arch, port)

    # start_qemu already stopped and reported a guest that failed to become ready.
    if proc is None:
        return 1

    # Record every value later commands need to list, stop, or inspect this instance.
    state["instances"][name] = {
        # Save QEMU's host PID for liveness checks and process-group signals.
        "pid": proc.pid,
        # Save the host port forwarded to port 8080 inside the unikernel.
        "port": port,
        # Save the exact image path this QEMU process booted.
        "image": image,
        # Save the captured console-log path for debugging and grading.
        "log": log_path,
        # Save the guest architecture used to select QEMU and CPU arguments.
        "arch": args.arch,
        # Save the host wall-clock deployment time for later inspection.
        "started": time.time(),
    }

    # Atomically persist the new instance so future tool invocations can manage it.
    save_state(state)

    # Print the name, required port field, and QEMU PID for the user and tests.
    print(f"deployed {name}  port={port}  pid={proc.pid}")

    # Return success only after state has been saved for a PING-ready instance.
    return 0


# TODO (implemented): benchmark cold-boot readiness for fresh instances.
def cmd_bench(args):
    """Boot temporary instances, time readiness, stop them, and print statistics."""
    # A benchmark needs at least one run to calculate a median and maximum.
    if args.count <= 0:
        # Reject invalid counts cleanly instead of indexing an empty result list.
        print("error: --count must be greater than zero", file=sys.stderr)
        return 1

    # Locate the image whose cold-start time and size will be measured.
    image = find_image(args.arch)

    # Benchmarking cannot proceed without a bootable image.
    if image is None:
        # Give the same actionable recovery message as deploy.
        print("error: no image found; run ukdeploy.py build first", file=sys.stderr)
        return 1

    # Remove dead entries before considering which benchmark port is available.
    state = reap_dead(load_state())

    # Hold one elapsed readiness time for every requested cold boot.
    ready_times = []

    # Boot, measure, and stop one fresh unikernel per requested run.
    for run_index in range(args.count):
        # Select a port not used by a saved instance or another host listener.
        port = pick_port(state)

        # Stop cleanly if no benchmark instance can receive a forwarded port.
        if port is None:
            # Explain that the configured range is exhausted.
            print("error: no free deployment ports are available", file=sys.stderr)
            return 1

        # Give this temporary run a unique log name without adding it to state.
        name = f"bench-{os.getpid()}-{run_index + 1}"

        # Start timing immediately before the shared QEMU launch operation.
        started = time.time()

        # Boot with the same command and readiness test used by normal deploys.
        proc, _log_path = start_qemu(name, image, args.arch, port)

        # Abort the benchmark if this run never reached a PING-ready state.
        if proc is None:
            return 1

        # Record total host-observed time from launch through the first PONG.
        ready_times.append(time.time() - started)

        # Force-stop the temporary instance so the next run is another cold boot.
        signal_process_group(proc.pid, signal.SIGKILL)

        try:
            # Reap the direct QEMU child and release its process resources and port.
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            # Report the unlikely cleanup failure rather than starting another run.
            print("error: benchmark QEMU did not stop", file=sys.stderr)
            return 1

    # Sort elapsed times so the middle value or pair represents the median.
    ordered_times = sorted(ready_times)

    # Save the middle index shared by odd and even result counts.
    middle = len(ordered_times) // 2

    # An odd count has one exact middle observation.
    if len(ordered_times) % 2:
        # Select the single middle value.
        ready_p50 = ordered_times[middle]
    else:
        # An even count uses the average of the two central observations.
        ready_p50 = (ordered_times[middle - 1] + ordered_times[middle]) / 2.0

    # Print the exact image whose size and boot performance were measured.
    print(f"image      {image}")

    # Read and print the image's current size in bytes.
    print(f"image_size {os.path.getsize(image)}")

    # Print the number of successful fresh boots included in the statistics.
    print(f"runs       {args.count}")

    # Print the median host-observed time with stable decimal precision.
    print(f"ready_p50  {ready_p50:.6f}")

    # Print the slowest host-observed ready time from this benchmark.
    print(f"ready_max  {max(ordered_times):.6f}")

    # Return success after every run was measured and stopped.
    return 0


# TODO (implemented): list saved live instances in text or JSON form.
def cmd_list(args):
    """Discard dead records, check each runner, and print the current instances."""
    # Load state and remove records whose saved QEMU process has exited.
    state = reap_dead(load_state())

    # Persist cleanup so stale names and ports remain available to later commands.
    save_state(state)

    # Produce machine-readable output before applying the human empty-state format.
    if args.json:
        # Add each dictionary key as the explicit name field in its JSON object.
        instances = [
            # Preserve every saved field while adding the instance's state key.
            {"name": name, **entry}
            for name, entry in state["instances"].items()
        ]

        # Serialize the list; an empty state naturally becomes the required [].
        print(json.dumps(instances))
        return 0

    # Human-readable output uses a sentence when there is nothing to display.
    if not state["instances"]:
        # Print the exact empty-state wording required by the specification.
        print("no instances")
        return 0

    # Print one current status line for every saved live QEMU instance.
    for name, entry in state["instances"].items():
        # Ask the runner now instead of assuming a live QEMU process means readiness.
        ready = ping(entry["port"])

        # Show the identifiers, current PING result, and console-log location.
        print(
            f"{name}  port={entry['port']}  pid={entry['pid']}  "
            f"ready={'yes' if ready else 'no'}  log={entry['log']}"
        )

    # Listing succeeds even if a live QEMU process is temporarily not PING-ready.
    return 0


# TODO (implemented): stop one saved instance or every saved instance.
def cmd_stop(args):
    """Terminate selected QEMU process groups and remove their state records."""
    # Load the records that identify the QEMU process groups to stop.
    state = load_state()

    # --all selects a snapshot of every currently recorded instance name.
    if args.all:
        # Copy the keys because each selected entry is deleted later.
        names = list(state["instances"])
    else:
        # A single-instance stop requires a positional name.
        if args.name is None:
            # Explain the two valid ways to select stop targets.
            print("error: specify an instance name or --all", file=sys.stderr)
            return 1

        # Refuse an unknown name without changing any other saved instance.
        if args.name not in state["instances"]:
            # Include the missing name in the command-line error.
            print(f"error: instance {args.name!r} does not exist", file=sys.stderr)
            return 1

        # Put the one selected name into the same loop shape used by --all.
        names = [args.name]

    # Stop each selected QEMU process group and remove its state entry.
    for name in names:
        # Read QEMU's host PID from this instance's saved state record.
        pid = state["instances"][name]["pid"]

        # Request graceful termination for QEMU and every process in its group.
        signal_process_group(pid, signal.SIGTERM)

        # Give the process about two seconds to honor SIGTERM.
        deadline = time.time() + 2.0

        # Poll briefly because this command may not be the QEMU process's parent.
        while alive(pid) and time.time() < deadline:
            # Avoid consuming CPU while waiting for process shutdown.
            time.sleep(0.1)

        # Escalate only when the process survived the graceful shutdown window.
        if alive(pid):
            # SIGKILL guarantees that a stuck instance cannot keep its port or memory.
            signal_process_group(pid, signal.SIGKILL)

        # Remove the record so its name and host port can be reused.
        del state["instances"][name]

    # Atomically save the state after all selected records have been removed.
    save_state(state)

    # Return success, including when --all was used with an already empty state.
    return 0


# ---- command line (PROVIDED) ---------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="build and deploy the runner unikernel")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build"); b.add_argument("--arch", default=host_arch(), choices=ARCHES)
    b.set_defaults(func=cmd_build)

    d = sub.add_parser("deploy")
    d.add_argument("--name"); d.add_argument("--port", type=int)
    d.add_argument("--arch", default=host_arch(), choices=ARCHES)
    d.set_defaults(func=cmd_deploy)

    l = sub.add_parser("list"); l.add_argument("--json", action="store_true")
    l.set_defaults(func=cmd_list)

    bn = sub.add_parser("bench")
    bn.add_argument("--count", type=int, default=3)
    bn.add_argument("--arch", default=host_arch(), choices=ARCHES)
    bn.set_defaults(func=cmd_bench)

    s = sub.add_parser("stop")
    s.add_argument("name", nargs="?"); s.add_argument("--all", action="store_true")
    s.set_defaults(func=cmd_stop)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
