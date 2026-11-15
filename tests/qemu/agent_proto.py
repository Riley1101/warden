"""The agent protocol against a real boot: agentd, over QEMU's socket.

Boots the normal image once per configuration, speaks frames to agentd
the way host/bridge.py does, and checks every answer, including the
frames agentd has to refuse without running anything.

    usage: python3 tests/qemu/agent_proto.py <iso>
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "host"))
from channel import ACTION, ERROR, OUTPUT, RESULT, SESSION, Channel, ChannelError  # noqa: E402

failures = 0


def check(cond: bool, what: str) -> None:
    global failures
    print(f"  {'ok  ' if cond else 'FAIL'} {what}")
    failures += not cond


def boot(iso: str, tmp: str, name: str) -> tuple[subprocess.Popen, Channel]:
    """Boot, and connect once the console shows the shell's prompt.

    Bytes sent before then can be lost before COM2 is set up, and a frame
    that loses its first bytes puts the stream out of step for good: the
    protocol has no resynchronisation, so the host waits for the guest.
    """
    sock_path = os.path.join(tmp, f"agent-{name}.sock")
    console = os.path.join(tmp, f"console-{name}.log")
    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-M", "q35", "-cpu", "max", "-m", "512M",
         "-cdrom", iso, "-no-reboot", "-display", "none",
         "-serial", f"file:{console}", "-serial", f"unix:{sock_path},server,nowait"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 30
    while time.time() < deadline:
        if os.path.exists(console) and b"warden:/$" in open(console, "rb").read():
            break
        time.sleep(0.1)
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(sock_path)
    sock.settimeout(20)
    return qemu, Channel(sock)


def refused(ch: Channel, kind: int, payload: bytes) -> str:
    """Send a frame that should get an ERROR: its text, or '' if not."""
    ch.send(kind, payload)
    got, body = ch.receive()
    return body.decode() if got == ERROR else ""


def config_c(ch: Channel) -> None:
    print("config C")
    check("no session" in refused(ch, ACTION, b"echo hi"), "action before session refused")
    check("A, B, C or D" in refused(ch, SESSION, b"X work-rw"), "bad configuration refused")
    check("unknown profile" in refused(ch, SESSION, b"C nope"), "unknown profile refused")
    ch.start("C", "work-rw")
    check(True, "session C work-rw accepted")
    check("already set" in refused(ch, SESSION, b"A work-rw"), "second session refused")

    check(ch.run("echo hi") == ("hi\n", 0), "echo: output and exit 0")
    check(ch.run("exit 7")[1] == 7, "exit code carried")
    out, code = ch.run("rm /bin/ls")
    check(code != 0 and "ECAPDENIED" in out, "agent label applied: rm /bin/ls denied")
    out, code = ch.run("ls -l /home/work")
    check(code == 0, "ls in scope works")

    big = "x" * 400  # two of these stay under sh's 1024-byte line
    out, code = ch.run(f"echo {big} ; echo {big}")
    check(out == f"{big}\n{big}\n" and code == 0, "long output across OUTPUT frames")

    check("too long" in refused(ch, ACTION, b"e" * 5000), "frame over 4096 refused")
    check(ch.run("echo still") == ("still\n", 0), "stream in step after a long frame")
    check("unexpected" in refused(ch, 9, b"?"), "unknown frame type refused")
    check("unexpected" in refused(ch, RESULT, b"\0\0\0\0"), "guest-side type refused")
    check("NUL" in refused(ch, ACTION, b"echo a\0rm /home/work/x"), "NUL in payload refused")

    # one frame dribbled in byte by byte
    frame = struct.pack("<IB", 8, ACTION) + b"echo bit"
    for b in frame:
        ch.sock.sendall(bytes([b]))
        time.sleep(0.005)
    kind, body = ch.receive()
    check(kind == OUTPUT and body == b"bit\n" and ch.receive()[0] == RESULT,
          "frame split across writes")


def denied(ch: Channel, command: str) -> bool:
    """The deny-list refused `command`: an ERROR, and nothing ran."""
    return "deny-list" in refused(ch, ACTION, command.encode())


def config_b(ch: Channel) -> None:
    print("config B")
    ch.start("B", "work-rw")
    check(ch.run("echo hi > /home/work/f ; cat /home/work/f") == ("hi\n", 0),
          "work in scope runs")
    check(denied(ch, "rm /bin/ls"), "write outside scope denied before running")
    check(denied(ch, "cat /home/work/../../etc/profiles"), "'..' denied")
    check(denied(ch, "cd /bin ; rm ls"), "relative write denied")
    # what the text doesn't show gets through: no kernel behind it in B
    out, code = ch.run("sh -c 'echo x > /a-file ; ls -l /a-file ; rm /a-file'")
    check(code == 0 and "human" in out, "nested sh gets past the deny-list alone")


def config_d(ch: Channel) -> None:
    print("config D, readonly")
    ch.start("D", "readonly")
    check(ch.run("cat /etc/profiles")[1] == 0, "reading works")
    check(denied(ch, "echo x > /home/work/f"), "deny-list: readonly writes nothing")
    out, code = ch.run("sh -c 'echo x > /home/work/f'")
    check(code != 0 and "ECAPDENIED" in out, "past the deny-list, the kernel refuses")
    out, code = ch.run("ls /home/work")
    check(code == 0 and "f" not in out.split(), "and nothing was written")


def config_a(ch: Channel) -> None:
    print("config A")
    ch.start("A", "work-rw")
    out, code = ch.run("echo x > /a-file && ls -l /a-file && rm /a-file")
    check(code == 0 and "human" in out, "no label: a write outside scope works")


def main() -> int:
    iso = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in (("C", config_c), ("A", config_a), ("B", config_b),
                           ("D", config_d)):
            qemu, ch = boot(iso, tmp, name)
            try:
                body(ch)
            except (OSError, ConnectionError, ChannelError) as error:
                check(False, f"config {name}: {error!r}")
            finally:
                log = open(os.path.join(tmp, f"console-{name}.log"), "rb").read()
                check(b"dropping" not in log, f"config {name}: nothing dropped on the way in")
                qemu.kill()
                qemu.wait()
    print("==> PASS" if failures == 0 else f"==> FAIL ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
