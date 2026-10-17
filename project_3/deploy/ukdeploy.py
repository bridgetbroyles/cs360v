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


# ---- what you implement ---------------------------------------------------

# TODO (implemented): remove dead QEMU instances from saved state.
def reap_dead(state):
    """Remove entries whose saved QEMU PID is no longer alive and return state."""
    # Collect first because a dictionary cannot change during iteration.
    dead_names = [
        name
        for name, entry in state["instances"].items()
        if not alive(entry["pid"])
    ]

    for name in dead_names:
        del state["instances"][name]

    return state


# TODO (implemented): check whether a host TCP port can be assigned.
def port_free(port):
    """Return True when a temporary socket can bind the requested host port."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as test_socket:
            # Allow reuse after a recently stopped instance.
            test_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            test_socket.bind(("127.0.0.1", port))
    except OSError:
        return False

    return True


# TODO (implemented): select an unused deployment port.
def pick_port(state):
    """Return the first allowed port that is neither reserved nor in use."""
    reserved_ports = {
        entry["port"]
        for entry in state["instances"].values()
    }

    for port in range(*PORT_RANGE):
        if port in reserved_ports:
            continue

        if port_free(port):
            return port

    return None


# TODO (implemented): test whether a runner answers through its forwarded port.
def ping(port, timeout=2.0):
    """Send PING to the runner and return whether its response contains PONG."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as conn:
            conn.sendall(b"PING\n")
            reply = conn.recv(64)
    except OSError:
        return False

    return b"PONG" in reply


# TODO (implemented): wait for a newly booted instance to answer PING.
def wait_ready(port, pid, timeout=READY_TIMEOUT):
    """Poll until the runner responds, QEMU exits, or the timeout expires."""
    deadline = time.time() + timeout

    while time.time() < deadline:
        if not alive(pid):
            return False

        if ping(port):
            return True

        time.sleep(0.3)

    return False

def qemu_command(image, arch, port):
    """Build the exact QEMU argument list used by deploy and bench."""
    spec = ARCHES[arch]

    argv = [
        spec["qemu"],
        "-kernel", image,
        "-nographic",
        "-no-reboot",
        "-m", "128M",
    ]

    # KVM acceleration works only for a native architecture with accessible KVM.
    kvm_available = (
        arch == host_arch()
        and os.access("/dev/kvm", os.R_OK | os.W_OK)
    )

    if kvm_available:
        argv.extend(["-enable-kvm", "-cpu", spec["cpu"]])
    else:
        argv.extend(["-cpu", spec["fallback_cpu"]])

    argv.extend(spec.get("extra", []))

    # Forward the selected host port to port 8080 in the guest.
    argv.extend([
        "-netdev", f"user,id=n0,hostfwd=tcp::{port}-:{RUNNER_PORT}",
        "-device", "virtio-net-pci,netdev=n0",
    ])

    return argv


def signal_process_group(pid, sig):
    """Send one signal to an instance's complete QEMU process group."""
    try:
        process_group = os.getpgid(pid)
        os.killpg(process_group, sig)
    except OSError:
        return False

    return True


def start_qemu(name, image, arch, port):
    """Start one QEMU instance and return it only after PING succeeds."""
    os.makedirs(LOG_DIR, exist_ok=True)
    log_path = os.path.join(LOG_DIR, f"{name}.log")

    try:
        log = open(log_path, "wb")
    except OSError as error:
        print(f"error: cannot open log {log_path}: {error}", file=sys.stderr)
        return None, log_path

    argv = qemu_command(image, arch, port)

    try:
        # A new session lets stop signal QEMU and any helpers as one process group.
        proc = subprocess.Popen(
            argv,
            stdout=log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )
    except OSError as error:
        log.close()
        print(f"error: cannot start QEMU: {error}", file=sys.stderr)
        return None, log_path

    log.close()

    if not wait_ready(port, proc.pid):
        signal_process_group(proc.pid, signal.SIGKILL)

        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            pass

        print(f"error: instance did not become ready; see {log_path}",
              file=sys.stderr)
        return None, log_path

    return proc, log_path


# TODO (implemented): build the unikernel image for the requested architecture.
def cmd_build(args):
    """Run KraftKit, verify that it produced an image, and report its path."""
    if shutil.which("kraft") is None:
        print("error: kraft is not installed or not in PATH", file=sys.stderr)
        return 1

    jobs = os.cpu_count() or 1

    argv = [
        "kraft",
        "build",
        "--plat", "qemu",
        "--arch", args.arch,
        "--no-update",
        "--no-prompt",
        "-j", str(jobs),
    ]

    result = subprocess.run(argv, cwd=APP_DIR)

    if result.returncode != 0:
        print("error: kraft build failed", file=sys.stderr)
        return 1

    image = find_image(args.arch)

    if image is None:
        print("error: build completed but produced no bootable image", file=sys.stderr)
        return 1

    print(image)
    return 0


# TODO (implemented): deploy one named unikernel instance and save its state.
def cmd_deploy(args):
    """Choose a name and port, start QEMU, wait for PING, and record the instance."""
    state = reap_dead(load_state())
    save_state(state)

    image = find_image(args.arch)

    if image is None:
        print("error: no image found; run ukdeploy.py build first", file=sys.stderr)
        return 1

    name = args.name

    # Generate the first unused runnerN name when the caller omitted --name.
    if name is None:
        number = 1
        while f"runner{number}" in state["instances"]:
            number += 1
        name = f"runner{number}"

    if name in state["instances"]:
        print(f"error: instance {name!r} already exists", file=sys.stderr)
        return 1

    reserved_ports = {
        entry["port"]
        for entry in state["instances"].values()
    }

    if args.port is not None:
        if args.port in reserved_ports or not port_free(args.port):
            print(f"error: port {args.port} is already in use", file=sys.stderr)
            return 1
        port = args.port
    else:
        port = pick_port(state)

        if port is None:
            print("error: no free deployment ports are available", file=sys.stderr)
            return 1

    proc, log_path = start_qemu(name, image, args.arch, port)

    if proc is None:
        return 1

    state["instances"][name] = {
        "pid": proc.pid,
        "port": port,
        "image": image,
        "log": log_path,
        "arch": args.arch,
        "started": time.time(),
    }

    save_state(state)
    print(f"deployed {name}  port={port}  pid={proc.pid}")
    return 0


# TODO (implemented): benchmark cold-boot readiness for fresh instances.
def cmd_bench(args):
    """Boot temporary instances, time readiness, stop them, and print statistics."""
    if args.count <= 0:
        print("error: --count must be greater than zero", file=sys.stderr)
        return 1

    image = find_image(args.arch)

    if image is None:
        print("error: no image found; run ukdeploy.py build first", file=sys.stderr)
        return 1

    state = reap_dead(load_state())
    ready_times = []

    # Each run uses a fresh QEMU process so the measurements are cold boots.
    for run_index in range(args.count):
        port = pick_port(state)

        if port is None:
            print("error: no free deployment ports are available", file=sys.stderr)
            return 1

        name = f"bench-{os.getpid()}-{run_index + 1}"
        started = time.time()
        proc, _log_path = start_qemu(name, image, args.arch, port)

        if proc is None:
            return 1

        ready_times.append(time.time() - started)
        signal_process_group(proc.pid, signal.SIGKILL)

        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            print("error: benchmark QEMU did not stop", file=sys.stderr)
            return 1

    ordered_times = sorted(ready_times)
    middle = len(ordered_times) // 2

    if len(ordered_times) % 2:
        ready_p50 = ordered_times[middle]
    else:
        ready_p50 = (ordered_times[middle - 1] + ordered_times[middle]) / 2.0

    print(f"image      {image}")
    print(f"image_size {os.path.getsize(image)}")
    print(f"runs       {args.count}")
    print(f"ready_p50  {ready_p50:.6f}")
    print(f"ready_max  {max(ordered_times):.6f}")
    return 0


# TODO (implemented): list saved live instances in text or JSON form.
def cmd_list(args):
    """Discard dead records, check each runner, and print the current instances."""
    state = reap_dead(load_state())
    save_state(state)

    if args.json:
        instances = [
            {"name": name, **entry}
            for name, entry in state["instances"].items()
        ]
        print(json.dumps(instances))
        return 0

    if not state["instances"]:
        print("no instances")
        return 0

    for name, entry in state["instances"].items():
        # A live QEMU PID does not guarantee that its runner is still responsive.
        ready = ping(entry["port"])
        print(
            f"{name}  port={entry['port']}  pid={entry['pid']}  "
            f"ready={'yes' if ready else 'no'}  log={entry['log']}"
        )

    return 0


# TODO (implemented): stop one saved instance or every saved instance.
def cmd_stop(args):
    """Terminate selected QEMU process groups and remove their state records."""
    state = load_state()

    if args.all:
        names = list(state["instances"])
    else:
        if args.name is None:
            print("error: specify an instance name or --all", file=sys.stderr)
            return 1

        if args.name not in state["instances"]:
            print(f"error: instance {args.name!r} does not exist", file=sys.stderr)
            return 1

        names = [args.name]

    for name in names:
        pid = state["instances"][name]["pid"]
        signal_process_group(pid, signal.SIGTERM)
        deadline = time.time() + 2.0

        while alive(pid) and time.time() < deadline:
            time.sleep(0.1)

        # Escalate when QEMU ignores the graceful shutdown window.
        if alive(pid):
            signal_process_group(pid, signal.SIGKILL)

        del state["instances"][name]

    save_state(state)
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
