"""LLM orchestration for the host-side Warden bridge.

This module deliberately knows nothing about terminals, subprocesses, or COM2.
An executor callback supplies command results today; a COM2 transport can supply
the same callback later.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
from typing import Any, Callable, NoReturn


MAX_COMMAND_LENGTH = 4096
MAX_OUTPUT_BYTES = 64 * 1024
DEFAULT_MAX_STEPS = 8

SYSTEM_PROMPT = """
You control tools in the Warden operating system.

Call run_shell when you need Warden to perform an operation or inspect state.
Never invent a command result: wait for the run_shell result before continuing.
After each result, either call run_shell once more or, if the task is complete,
respond with a concise final summary.

Provide only the command contents to run_shell; do not wrap them in `sh -c`.
Prefer conventional, non-interactive POSIX shell commands.
""".strip()

TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "run_shell",
            "description": (
                "Ask Warden to run one POSIX shell command and return its "
                "exit code, standard output, and standard error."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "command": {
                        "type": "string",
                        "description": "The command to pass to sh -c.",
                    }
                },
                "required": ["command"],
                "additionalProperties": False,
            },
        },
    }
]


@dataclass(frozen=True)
class Action:
    call_id: str
    command: str


@dataclass(frozen=True)
class Result:
    exit_code: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class Final:
    text: str


class AgentError(RuntimeError):
    """A stable, user-visible failure from the model or bridge contract."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


Record = Callable[[dict[str, Any]], None]
Execute = Callable[[Action], Result]


def _noop_record(_event: dict[str, Any]) -> None:
    pass


def validate_result(result: Result) -> Result:
    if not isinstance(result, Result):
        raise AgentError("invalid_result", "executor must return a Result")
    if isinstance(result.exit_code, bool) or not isinstance(result.exit_code, int):
        raise AgentError("invalid_result", "exit_code must be an integer")
    if not 0 <= result.exit_code <= 255:
        raise AgentError("invalid_result", "exit_code must be between 0 and 255")
    if not isinstance(result.stdout, str) or not isinstance(result.stderr, str):
        raise AgentError("invalid_result", "stdout and stderr must be strings")
    if len(result.stdout.encode("utf-8")) > MAX_OUTPUT_BYTES:
        raise AgentError("invalid_result", "stdout exceeds the 64 KiB limit")
    if len(result.stderr.encode("utf-8")) > MAX_OUTPUT_BYTES:
        raise AgentError("invalid_result", "stderr exceeds the 64 KiB limit")
    return result


def _parse_action(message: Any) -> Action:
    calls = message.tool_calls or []
    if len(calls) != 1:
        raise AgentError(
            "invalid_tool_calls",
            f"expected exactly one tool call, received {len(calls)}",
        )

    call = calls[0]
    function = getattr(call, "function", None)
    name = getattr(function, "name", None)
    if getattr(call, "type", None) != "function" or name != "run_shell":
        raise AgentError("unknown_tool", f"unexpected tool call: {name!r}")

    try:
        arguments = json.loads(function.arguments)
    except (TypeError, json.JSONDecodeError) as error:
        raise AgentError(
            "invalid_arguments", "run_shell arguments are not valid JSON"
        ) from error

    if not isinstance(arguments, dict) or set(arguments) != {"command"}:
        raise AgentError(
            "invalid_arguments",
            "run_shell arguments must contain exactly one field: command",
        )

    command = arguments["command"]
    if not isinstance(command, str) or not command.strip():
        raise AgentError("invalid_arguments", "command must be a non-empty string")

    command = command.strip()
    if "\x00" in command:
        raise AgentError("invalid_arguments", "command cannot contain a NUL byte")
    if len(command) > MAX_COMMAND_LENGTH:
        raise AgentError(
            "invalid_arguments",
            f"command exceeds the {MAX_COMMAND_LENGTH}-character limit",
        )

    call_id = getattr(call, "id", None)
    if not isinstance(call_id, str) or not call_id:
        raise AgentError("invalid_tool_calls", "tool call has no call ID")
    return Action(call_id=call_id, command=command)


class AgentSession:
    """Run one user task through zero or more run_shell calls."""

    def __init__(self, client: Any, model: str, max_steps: int = DEFAULT_MAX_STEPS):
        if max_steps < 1:
            raise ValueError("max_steps must be at least 1")
        self.client = client
        self.model = model
        self.max_steps = max_steps

    def _fail(
        self,
        code: str,
        message: str,
        record: Record,
        step: int,
        cause: Exception | None = None,
    ) -> NoReturn:
        record(
            {
                "event": "error",
                "step": step,
                "data": {"code": code, "message": message},
            }
        )
        error = AgentError(code, message)
        if cause is None:
            raise error
        raise error from cause

    def run(
        self,
        prompt: str,
        execute: Execute,
        record: Record = _noop_record,
    ) -> Final:
        prompt = prompt.strip()
        if not prompt:
            raise AgentError("invalid_prompt", "prompt must not be empty")

        messages: list[Any] = [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": prompt},
        ]
        record({"event": "task", "step": 0, "data": {"prompt": prompt}})

        completed_steps = 0
        while True:
            try:
                response = self.client.chat.completions.create(
                    model=self.model,
                    messages=messages,
                    tools=TOOLS,
                    tool_choice="auto",
                    parallel_tool_calls=False,
                )
            except Exception as error:
                self._fail(
                    "model_request_failed",
                    "OpenRouter model request failed",
                    record,
                    completed_steps,
                    error,
                )

            if not getattr(response, "choices", None):
                self._fail(
                    "empty_response",
                    "OpenRouter returned no choices",
                    record,
                    completed_steps,
                )

            message = response.choices[0].message
            calls = getattr(message, "tool_calls", None) or []
            if not calls:
                content = getattr(message, "content", None)
                if not isinstance(content, str) or not content.strip():
                    self._fail(
                        "empty_response",
                        "model returned neither a tool call nor final text",
                        record,
                        completed_steps,
                    )
                final = Final(text=content.strip())
                record(
                    {
                        "event": "final",
                        "step": completed_steps,
                        "data": {"text": final.text},
                    }
                )
                return final

            if len(calls) != 1:
                self._fail(
                    "invalid_tool_calls",
                    f"expected exactly one tool call, received {len(calls)}",
                    record,
                    completed_steps,
                )
            if completed_steps >= self.max_steps:
                self._fail(
                    "step_limit",
                    f"model exceeded the {self.max_steps}-action limit",
                    record,
                    completed_steps,
                )

            try:
                action = _parse_action(message)
            except AgentError as error:
                self._fail(error.code, str(error), record, completed_steps, error)

            completed_steps += 1
            record(
                {
                    "event": "action",
                    "step": completed_steps,
                    "data": {
                        "call_id": action.call_id,
                        "command": action.command,
                    },
                }
            )

            try:
                result = validate_result(execute(action))
            except AgentError as error:
                self._fail(error.code, str(error), record, completed_steps, error)
            except Exception as error:
                self._fail(
                    "execution_failed",
                    "the result simulator failed",
                    record,
                    completed_steps,
                    error,
                )

            record(
                {
                    "event": "result",
                    "step": completed_steps,
                    "data": {
                        "call_id": action.call_id,
                        "exit_code": result.exit_code,
                        "stdout": result.stdout,
                        "stderr": result.stderr,
                    },
                }
            )

            messages.append(message)
            messages.append(
                {
                    "role": "tool",
                    "tool_call_id": action.call_id,
                    "content": json.dumps(
                        {
                            "exit_code": result.exit_code,
                            "stdout": result.stdout,
                            "stderr": result.stderr,
                        },
                        ensure_ascii=False,
                    ),
                }
            )
