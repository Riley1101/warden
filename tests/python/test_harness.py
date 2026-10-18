from contextlib import redirect_stderr
from io import StringIO
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from host.bridge import Action, AgentError
from host.harness import JsonlRecorder, manual_execute, parse_manual_result


class ManualResultTests(unittest.TestCase):
    def test_parses_valid_result(self):
        result = parse_manual_result(
            '{"exit_code": 0, "stdout": "ok\\n", "stderr": ""}'
        )
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.stdout, "ok\n")

    def test_rejects_unknown_fields(self):
        with self.assertRaises(AgentError):
            parse_manual_result(
                '{"exit_code": 0, "stdout": "", "stderr": "", "extra": 1}'
            )

    def test_rejects_invalid_exit_code(self):
        with self.assertRaises(AgentError):
            parse_manual_result(
                '{"exit_code": 256, "stdout": "", "stderr": ""}'
            )

    def test_rejects_oversized_output(self):
        raw = json.dumps(
            {"exit_code": 0, "stdout": "x" * (64 * 1024 + 1), "stderr": ""}
        )
        with self.assertRaises(AgentError):
            parse_manual_result(raw)

    def test_manual_executor_reprompts_without_losing_action(self):
        input_stream = StringIO(
            'not json\n{"exit_code": 1, "stdout": "", "stderr": "nope"}\n'
        )
        error_stream = StringIO()
        with patch("sys.stdin", input_stream), redirect_stderr(error_stream):
            result = manual_execute(Action("call-1", "rm example.txt"))

        self.assertEqual(result.exit_code, 1)
        self.assertIn("invalid result", error_stream.getvalue())
        self.assertEqual(error_stream.getvalue().count("RESULT JSON>"), 2)


class JsonlRecorderTests(unittest.TestCase):
    def test_writes_expected_envelope_without_environment_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            with patch.dict(os.environ, {"OPENROUTER_API_KEY": "never-log-me"}):
                recorder = JsonlRecorder(path, "google/gemini-3.8-flash")
                recorder(
                    {
                        "event": "action",
                        "step": 1,
                        "data": {"call_id": "call-1", "command": "ls"},
                    }
                )

            text = path.read_text(encoding="utf-8")
            event = json.loads(text)
            self.assertEqual(event["event"], "action")
            self.assertEqual(event["step"], 1)
            self.assertEqual(event["model"], "google/gemini-3.8-flash")
            self.assertNotIn("never-log-me", text)
            self.assertTrue(event["session_id"])
            self.assertTrue(event["timestamp"].endswith("Z"))


if __name__ == "__main__":
    unittest.main()
