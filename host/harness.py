"""Interactive, non-executing harness for Warden agent experiments."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys
from typing import Any
from uuid import uuid4

from host.bridge import (
    DEFAULT_MAX_STEPS,
    Action,
    AgentError,
    AgentSession,
    Final,
    Result,
    validate_result,
)


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LOG = PROJECT_ROOT / "build-host" / "harness.jsonl"
OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1"


class HarnessError(RuntimeError):
    """A local configuration, input, or logging error."""


def load_environment() -> None:
    try:
        from dotenv import load_dotenv
    except ImportError as error:
        raise HarnessError(
            "python-dotenv is not installed; run "
            "python3 -m pip install -r host/requirements.txt"
        ) from error
    load_dotenv(PROJECT_ROOT / ".env")


def required_environment(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise HarnessError(f"required environment variable {name} is not set")
    return value


def create_client() -> Any:
    try:
        from openai import OpenAI
    except ImportError as error:
        raise HarnessError(
            "openai is not installed; run "
            "python3 -m pip install -r host/requirements.txt"
        ) from error
    return OpenAI(
        base_url=OPENROUTER_BASE_URL,
        api_key=required_environment("OPENROUTER_API_KEY"),
    )


def parse_manual_result(raw: str) -> Result:
    try:
        value = json.loads(raw)
    except json.JSONDecodeError as error:
        raise AgentError("invalid_result", "result is not valid JSON") from error

    required = {"exit_code", "stdout", "stderr"}
    if not isinstance(value, dict) or set(value) != required:
        raise AgentError(
            "invalid_result",
            "result must contain exactly exit_code, stdout, and stderr",
        )
    return validate_result(
        Result(
            exit_code=value["exit_code"],
            stdout=value["stdout"],
            stderr=value["stderr"],
        )
    )


def manual_execute(action: Action) -> Result:
    print(f"ACTION: {action.command}", file=sys.stderr)
    while True:
        print("RESULT JSON> ", end="", file=sys.stderr, flush=True)
        line = sys.stdin.readline()
        if line == "":
            raise HarnessError("input ended while waiting for a simulated result")
        try:
            return parse_manual_result(line)
        except AgentError as error:
            print(f"invalid result: {error}", file=sys.stderr)


class JsonlRecorder:
    def __init__(self, path: Path, model: str):
        self.path = path
        self.model = model
        self.session_id = str(uuid4())

    def __call__(self, event: dict[str, Any]) -> None:
        envelope = {
            "timestamp": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
            "session_id": self.session_id,
            "step": event["step"],
            "event": event["event"],
            "model": self.model,
            "data": event["data"],
        }
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(envelope, ensure_ascii=False, sort_keys=True))
                stream.write("\n")
        except OSError as error:
            raise HarnessError(f"cannot write transcript {self.path}: {error}") from error


def run_task(
    client: Any,
    model: str,
    prompt: str,
    max_steps: int,
    log_path: Path,
) -> bool:
    recorder = JsonlRecorder(log_path, model)
    session = AgentSession(client, model, max_steps=max_steps)
    try:
        final: Final = session.run(prompt, manual_execute, recorder)
    except (AgentError, HarnessError) as error:
        print(f"harness: {error}", file=sys.stderr)
        return False

    print(f"FINAL: {final.text}")
    return True


def positive_integer(value: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a task through the offline Warden agent simulator."
    )
    parser.add_argument(
        "prompt",
        nargs="*",
        help="One task. If omitted, start an interactive task prompt.",
    )
    parser.add_argument(
        "--max-steps",
        type=positive_integer,
        default=DEFAULT_MAX_STEPS,
        help=f"maximum actions per task (default: {DEFAULT_MAX_STEPS})",
    )
    parser.add_argument(
        "--log",
        type=Path,
        default=DEFAULT_LOG,
        help=f"JSONL transcript path (default: {DEFAULT_LOG})",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()

    try:
        load_environment()
        model = required_environment("OPENROUTER_MODEL")
        client = create_client()
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2

    if args.prompt:
        prompt = " ".join(args.prompt).strip()
        return 0 if run_task(client, model, prompt, args.max_steps, args.log) else 1

    print(
        "Warden offline agent harness. Press Ctrl-D or enter 'quit' to exit.",
        file=sys.stderr,
    )
    while True:
        try:
            print("task> ", end="", file=sys.stderr, flush=True)
            line = sys.stdin.readline()
        except KeyboardInterrupt:
            print(file=sys.stderr)
            return 0

        if line == "":
            print(file=sys.stderr)
            return 0

        prompt = line.strip()
        if prompt.lower() in {"quit", "exit"}:
            return 0
        if prompt:
            run_task(client, model, prompt, args.max_steps, args.log)


if __name__ == "__main__":
    raise SystemExit(main())
