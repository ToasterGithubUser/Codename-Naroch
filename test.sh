#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"
PATH="$HOME/opt/cross/bin:$PATH"
export PATH
. ./config.sh

if [ ! -f fat16.img ]; then
    echo "test.sh: fat16.img is required." >&2
    exit 1
fi

./iso.sh
tmpdir=$(mktemp -d)
qemu_pid=
cleanup() {
    if [ -n "$qemu_pid" ]; then
        kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

cp fat16.img "$tmpdir/fat16.img"
qemu_system="qemu-system-$(./target-triplet-to-arch.sh "$HOST")"
serial_log="$tmpdir/serial.log"
monitor_socket="$tmpdir/monitor.sock"

"$qemu_system" \
    -boot order=d -cdrom myos.iso \
    -drive "file=$tmpdir/fat16.img,format=raw,if=ide,index=0" \
    -M pc -m 2G -netdev user,id=net0 -device e1000,netdev=net0 \
    -display none -serial "file:$serial_log" \
    -monitor "unix:$monitor_socket,server,nowait" -no-reboot &
qemu_pid=$!

python3 - "$monitor_socket" "$serial_log" <<'PY'
import socket
import sys
import time

monitor_path, serial_path = sys.argv[1:]
deadline = time.monotonic() + 40
monitor = None
while time.monotonic() < deadline:
    try:
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.connect(monitor_path)
        break
    except OSError:
        if monitor:
            monitor.close()
        monitor = None
        time.sleep(0.1)
if monitor is None:
    raise SystemExit("test.sh: QEMU monitor did not become available.")

monitor.settimeout(3)

def read_prompt():
    response = bytearray()
    while b"(qemu) " not in response:
        chunk = monitor.recv(4096)
        if not chunk:
            raise RuntimeError("QEMU monitor disconnected.")
        response.extend(chunk)
    return bytes(response)

read_prompt()

def send_key(key):
    monitor.sendall(("sendkey " + key + "\n").encode())
    read_prompt()
    time.sleep(0.08)

def log_text():
    try:
        with open(serial_path, encoding="utf-8", errors="replace") as stream:
            return stream.read()
    except FileNotFoundError:
        return ""

def run_command(command, tab_after=None):
    before = log_text().count("naroch:")
    for character in command:
        send_key({" ": "spc", ".": "dot", "-": "minus"}.get(character, character))
    if tab_after is not None:
        send_key("tab")
    send_key("ret")

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if log_text().count("naroch:") > before:
            return
        time.sleep(0.1)
    raise RuntimeError("Timed out waiting for the shell after: " + command)

deadline = time.monotonic() + 40
while time.monotonic() < deadline and "naroch:" not in log_text():
    time.sleep(0.1)
if "naroch:" not in log_text():
    print(log_text()[-6000:], file=sys.stderr)
    raise SystemExit("test.sh: kernel did not reach the shell.")

prompt_count = log_text().count("naroch:")
send_key("f2")
deadline = time.monotonic() + 10
while time.monotonic() < deadline and log_text().count("naroch:") <= prompt_count:
    time.sleep(0.1)
if log_text().count("naroch:") <= prompt_count:
    raise SystemExit("test.sh: first terminal switch did not show a shell prompt.")

run_command("dis", tab_after=True)
run_command("help")
run_command("version")
run_command("display")
run_command("devices")
run_command("lspci")
run_command("ls")
run_command("write regression.txt regression-data")
run_command("cat regression.txt")
run_command("rm regression.txt")
run_command("mkdir regression-dir")
run_command("rmdir regression-dir")
run_command("format")
run_command("format yes")
run_command("ls")
run_command("write formatted.txt filesystem-ready")
run_command("cat formatted.txt")

output = log_text()
required = [
    "Available commands:",
    "Architecture: i386 Protected Mode",
    "Storage:",
    "regression-data",
    "Removed file: regression.txt",
    "Removed directory: regression-dir",
    "Type 'format yes' to confirm.",
    "FAT16 format completed and mounted.",
    "filesystem-ready",
]
missing = [text for text in required if text not in output]
if "Framebuffer:" not in output:
    missing.append("active framebuffer mode")
if missing:
    print(output[-6000:], file=sys.stderr)
    raise SystemExit("test.sh: missing expected output: " + ", ".join(missing))

mode_line = next(line.strip() for line in output.splitlines()
                 if "Framebuffer:" in line)
print("Detected " + mode_line)
print("PASS: shell completion, display, device, PCI, and FAT16 smoke tests.")
monitor.close()
PY
