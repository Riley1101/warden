import argparse
import json
import os
import sys
from typing import Any
from openai import OpenAI, OpenAIError
from pathlib import Path
from dotenv import load_dotenv

project_root = Path(__file__).resolve().parent.parent
load_dotenv(project_root / ".env")

OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1/"
MAX_COMMAND_LENGTH = 4096
SYSTEM_PROMPT = """
You translate operator requests into one POSIX shell command for the Warden
operating system.

Always respond by calling the run_shell tool exactly once.
Do not claim that the command has already run.
Do not wrap the command in `sh -c`; provide only its contents.
Prefer conventional, non-interactive commands.
""".strip()

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "run_shell",
            "description": (
                "Propose one POSIX shell command for Warden to run. "
                "The harness only prints this action; it does not execute it."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "command": {
                        "type": "string",
                        "description": "The command that should be passed to sh -c.",
                    }
                },
                "required": ["command"],
                "additionalProperties": False,
            },
        },
    }
]

class HarnessError(RuntimeError):
    """A user-visible harness or model-response error."""


def required_environment(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise HarnessError(f"required environment variable {name} is not set")
    return value


def create_client() -> OpenAI:
    return OpenAI(
        base_url=OPENROUTER_BASE_URL,
        api_key=required_environment("OPENROUTER_API_KEY"),
    )


def parse_action(response: Any) -> dict[str, Any]:
    if not response.choices:
        raise HarnessError("OpenRouter returned no choices")

    calls = response.choices[0].message.tool_calls or []

    if len(calls) != 1:
        raise HarnessError(
            f"expected exactly one tool call, received {len(calls)}"
        )

    call = calls[0]
    if call.type != "function" or call.function.name != "run_shell":
        raise HarnessError(f"unexpected tool call: {call.function.name!r}")

    try:
        arguments = json.loads(call.function.arguments)
    except json.JSONDecodeError as error:
        raise HarnessError(
            f"tool arguments were not valid JSON: {error}"
        ) from error

    if not isinstance(arguments, dict) or set(arguments) != {"command"}:
        raise HarnessError(
            "run_shell arguments must contain exactly one field: command"
        )

    command = arguments["command"]
    if not isinstance(command, str) or not command.strip():
        raise HarnessError("run_shell command must be a non-empty string")

    command = command.strip()
    if "\x00" in command:
        raise HarnessError("run_shell command cannot contain a NUL byte")

    if len(command) > MAX_COMMAND_LENGTH:
        raise HarnessError(
            f"run_shell command exceeds {MAX_COMMAND_LENGTH} characters"
        )

    return {
        "type": "ACTION",
        "tool_call_id": call.id,
        "tool": "run_shell",
        "arguments": {"command": command},
    }


def request_action(client: OpenAI, model: str, prompt: str) -> dict[str, Any]:
    response = client.chat.completions.create(
        model=model,
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ],
        tools=TOOLS,
        tool_choice={
            "type": "function",
            "function": {"name": "run_shell"},
        },
        parallel_tool_calls=False,
    )
    return parse_action(response)


def print_action(client: OpenAI, model: str, prompt: str) -> bool:
    try:
        action = request_action(client, model, prompt)
    except (HarnessError, OpenAIError) as error:
        print(f"harness: {error}", file=sys.stderr)
        return False

    # One JSON object per line, ready for later JSONL logging or COM2 framing.
    print(json.dumps(action, ensure_ascii=False, sort_keys=True))
    return True


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Translate natural language into a Warden tool call."
    )
    parser.add_argument(
        "prompt",
        nargs="*",
        help="One request. If omitted, start an interactive prompt.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()

    try:
        model = required_environment("OPENROUTER_MODEL")
        client = create_client()
    except HarnessError as error:
        print(f"harness: {error}", file=sys.stderr)
        return 2

    if args.prompt:
        prompt = " ".join(args.prompt).strip()
        return 0 if print_action(client, model, prompt) else 1

    print(
        "Warden tool-call harness. Press Ctrl-D or enter 'quit' to exit.",
        file=sys.stderr,
    )

    while True:
        try:
            print("warden> ", end="", file=sys.stderr, flush=True)
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
            print_action(client, model, prompt)


if __name__ == "__main__":
    raise SystemExit(main())