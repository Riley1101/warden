"""One Warden boot, driven from both sides.

The agent's side goes through agentd as ACTION frames (channel.py), the
way a model's commands arrive. The human's side is typed at the console
shell, which is pid 1 and unlabelled: setting things up before a run and
looking at what is left after it. Used by host/pilot.py and
tests/qemu/escape_matrix.py.
"""

import os
import socket
import subprocess
import time

from channel import Channel, ChannelError

MARK = "@@DONE@@"
MAX_LINE = 1024  # Warden's sh takes no longer line


class Console:
    """The console shell, typed at over COM1."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.pending = b""

    def until(self, token: bytes, timeout: float = 30) -> bytes:
        deadline = time.time() + timeout
        while token not in self.pending:
            if time.time() > deadline:
                raise TimeoutError(f"console: no {token!r}")
            self.sock.settimeout(max(0.1, deadline - time.time()))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("console closed")
            self.pending += chunk
        head, _, self.pending = self.pending.partition(token)
        return head

    def run(self, line: str) -> str:
        """Run `line`; what it printed. The marker is split by quotes, so
        the echo of the typed line can't be mistaken for its output."""
        half = len(MARK) // 2
        self.sock.sendall(f"{line} ; echo {MARK[:half]}''{MARK[half:]}\n".encode())
        out = self.until(MARK.encode()).decode(errors="replace")
        self.until(b"$ ")  # the prompt after it
        return out.split("\n", 1)[1] if "\n" in out else ""


def quote(text: str) -> str:
    """`text` as one word for Warden's sh, which has quotes but no escapes."""
    if "'" not in text:
        return f"'{text}'"
    if '"' not in text:
        return f'"{text}"'
    raise ValueError(f"can't quote both kinds of quote: {text!r}")


class Machine:
    """A fresh boot, with agentd's session started as `config` and `profile`."""

    def __init__(self, iso: str, tmp: str, config: str, profile: str, name: str = ""):
        tag = name or config
        con_path = os.path.join(tmp, f"con-{tag}.sock")
        agent_path = os.path.join(tmp, f"agent-{tag}.sock")
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64", "-M", "q35", "-cpu", "max", "-m", "512M",
             "-cdrom", iso, "-no-reboot", "-display", "none",
             "-serial", f"unix:{con_path},server=on,wait=off",
             "-serial", f"unix:{agent_path},server=on,wait=off"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.console = Console(self._connect(con_path))
            self.console.until(b"warden:/$ ")
            self.agent = Channel(self._connect(agent_path))
            self.agent.sock.settimeout(20)
            self.agent.start(config, profile)
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _connect(path: str) -> socket.socket:
        for _ in range(100):
            try:
                sock = socket.socket(socket.AF_UNIX)
                sock.connect(path)
                return sock
            except OSError:
                time.sleep(0.1)
        raise TimeoutError(f"{path}: QEMU never listened")

    def act(self, command: str) -> tuple[str, int]:
        """The agent's command: output and exit code, or the deny-list's
        refusal as its text and exit -1."""
        try:
            return self.agent.run(command)
        except ChannelError as error:
            return str(error), -1

    def human(self, line: str) -> str:
        return self.console.run(line)

    def write_file(self, path: str, text: str) -> None:
        """`path` holding `text`, written by the human, one echo per line
        (there is no other way in: no here-documents, no escapes)."""
        lines = text.split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        op = ">"
        if not lines:
            self.human(f"echo -n > {path}")
        for line in lines:
            cmd = f"echo {quote(line)} {op} {path}"
            if len(cmd) + 20 > MAX_LINE:
                raise ValueError(f"{path}: a line too long for the console")
            self.human(cmd)
            op = ">>"

    def close(self) -> None:
        self.qemu.kill()
        self.qemu.wait()
