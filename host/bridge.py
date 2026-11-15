import argparse
import json
import socket
import sys
import time

from openai import OpenAIError

from channel import Channel, ChannelError
from harness import (PROFILE_RULES, HarnessError, create_client, request_action,
                     required_environment)

AGENT_SOCKET = "/tmp/warden-agent.sock"
RESULT_TIMEOUT = 30  # seconds of silence before giving up on a command


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Drive Warden's agentd with a model.")
    parser.add_argument("--config", choices="ABCD", default="C",
                        help="A prompt only, B agentd only, C kernel only, D both")
    parser.add_argument("--profile", default="work-rw", choices=sorted(PROFILE_RULES))
    parser.add_argument("--log", help="append one JSON line per step to this file")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
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
        channel = Channel(sock)
        channel.start(args.config, args.profile)
    except ChannelError as error:
        print(f"bridge: session refused: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"bridge: {AGENT_SOCKET}: {error} (is `make run` up?)", file=sys.stderr)
        return 1

    log = open(args.log, "a", encoding="utf-8") if args.log else None
    print(f"warden bridge: config {args.config}, profile {args.profile}; "
          "type a request, Ctrl-D to quit")
    step = 0
    while True:
        try:
            prompt = input("warden> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not prompt:
            continue

        step += 1
        entry = {"step": step, "time": time.time(), "config": args.config,
                 "profile": args.profile, "model": model, "prompt": prompt}
        try:
            action = request_action(client, model, prompt, args.profile)
            command = action["arguments"]["command"]
            entry["command"] = command
            print(f"$ {command}")
            output, code = channel.run(command)
            entry.update(output=output, exit=code)
        except (HarnessError, OpenAIError, ChannelError) as error:
            entry["error"] = str(error)
            print(f"bridge: {error}", file=sys.stderr)
            continue
        except (OSError, ConnectionError) as error:
            print(f"bridge: {error}", file=sys.stderr)
            return 1
        finally:
            if log:
                log.write(json.dumps(entry) + "\n")
                log.flush()

        print(output, end="" if output.endswith("\n") or not output else "\n")
        print(f"[exit {code}]")


if __name__ == "__main__":
    raise SystemExit(main())
