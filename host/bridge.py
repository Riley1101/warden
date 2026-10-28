import re
import socket
import sys

from openai import OpenAIError

from harness import HarnessError, create_client, request_action, required_environment

AGENT_SOCKET = "/tmp/warden-agent.sock"
EXIT_LINE = re.compile(rb"\x00EXIT (\d+)\n$")
RESULT_TIMEOUT = 30  # seconds of silence before giving up on a command


def run_in_warden(sock: socket.socket, command: str) -> tuple[str, int]:
    """Send one command line to agentd; return its output and exit code."""
    sock.sendall(command.encode() + b"\n")
    received = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("warden closed the agent channel")
        received += chunk
        match = EXIT_LINE.search(received)
        if match:
            output = received[: match.start()].decode(errors="replace")
            return output, int(match.group(1))


def main() -> int:
    try:
        model = required_environment("OPENROUTER_MODEL")
        client = create_client()
    except HarnessError as error:
        print(f"bridge: {error}", file=sys.stderr)
        return 1

    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(RESULT_TIMEOUT)
    try:
        sock.connect(AGENT_SOCKET)
    except OSError as error:
        print(f"bridge: {AGENT_SOCKET}: {error} (is `make run` up?)", file=sys.stderr)
        return 1

    print("warden bridge: type a request, Ctrl-D to quit")
    while True:
        try:
            prompt = input("warden> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not prompt:
            continue

        try:
            action = request_action(client, model, prompt)
            command = action["arguments"]["command"]
            print(f"$ {command}")
            output, code = run_in_warden(sock, command)
        except (HarnessError, OpenAIError) as error:
            print(f"bridge: {error}", file=sys.stderr)
            continue
        except (OSError, ConnectionError) as error:
            print(f"bridge: {error}", file=sys.stderr)
            return 1

        print(output, end="" if output.endswith("\n") or not output else "\n")
        print(f"[exit {code}]")


if __name__ == "__main__":
    raise SystemExit(main())
