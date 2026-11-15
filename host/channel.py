"""The host end of the agent protocol (userland/frame.h is the guest end).

Frames are [u32 len, little-endian][u8 type][payload]. The host sends one
SESSION, then ACTIONs; every frame it sends is answered by either a RESULT
(an ACTION's output comes first, in OUTPUT frames) or one ERROR.
"""

import socket
import struct

SESSION, ACTION, OUTPUT, RESULT, ERROR = 1, 2, 3, 4, 5
FRAME_MAX = 4096
HEADER = struct.Struct("<IB")


class ChannelError(RuntimeError):
    """agentd refused a frame (an ERROR reply): nothing ran."""


class Channel:
    def __init__(self, sock: socket.socket):
        self.sock = sock

    def send(self, kind: int, payload: bytes) -> None:
        self.sock.sendall(HEADER.pack(len(payload), kind) + payload)

    def _exact(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("warden closed the agent channel")
            data += chunk
        return data

    def receive(self) -> tuple[int, bytes]:
        length, kind = HEADER.unpack(self._exact(HEADER.size))
        if length > FRAME_MAX:
            raise ConnectionError(f"frame of {length} bytes from warden")
        return kind, self._exact(length)

    def _answer(self) -> tuple[bytes, int]:
        """Output up to the RESULT, and the RESULT's code."""
        output = b""
        while True:
            kind, payload = self.receive()
            if kind == OUTPUT:
                output += payload
            elif kind == RESULT and len(payload) == 4:
                return output, struct.unpack("<I", payload)[0]
            elif kind == ERROR:
                raise ChannelError(payload.decode(errors="replace"))
            else:
                raise ConnectionError(f"unexpected frame type {kind} from warden")

    def start(self, config: str, profile: str) -> None:
        self.send(SESSION, f"{config} {profile}".encode())
        self._answer()

    def run(self, command: str) -> tuple[str, int]:
        """One command line through sh -c: its output and exit code."""
        self.send(ACTION, command.encode())
        output, code = self._answer()
        return output.decode(errors="replace"), code
