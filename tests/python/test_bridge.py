import json
from types import SimpleNamespace
import unittest

from host.bridge import AgentError, AgentSession, Result


def tool_call(call_id="call-1", name="run_shell", arguments=None):
    if arguments is None:
        arguments = json.dumps({"command": "ls /home/work"})
    return SimpleNamespace(
        id=call_id,
        type="function",
        function=SimpleNamespace(name=name, arguments=arguments),
    )


def response(*, calls=None, content=None, choices=True):
    if not choices:
        return SimpleNamespace(choices=[])
    message = SimpleNamespace(tool_calls=calls or [], content=content)
    return SimpleNamespace(choices=[SimpleNamespace(message=message)])


class FakeCompletions:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def create(self, **request):
        self.requests.append(request)
        return self.responses.pop(0)


def fake_client(*responses):
    completions = FakeCompletions(responses)
    client = SimpleNamespace(chat=SimpleNamespace(completions=completions))
    return client, completions


class AgentSessionTests(unittest.TestCase):
    def test_one_action_then_final(self):
        client, completions = fake_client(
            response(calls=[tool_call()]),
            response(content="There is one file."),
        )
        events = []

        final = AgentSession(client, "test-model").run(
            "count files",
            lambda action: Result(0, "example.txt\n", ""),
            events.append,
        )

        self.assertEqual(final.text, "There is one file.")
        self.assertEqual(
            [event["event"] for event in events],
            ["task", "action", "result", "final"],
        )
        second_messages = completions.requests[1]["messages"]
        self.assertEqual(second_messages[-1]["role"], "tool")
        self.assertEqual(second_messages[-1]["tool_call_id"], "call-1")
        self.assertEqual(
            json.loads(second_messages[-1]["content"]),
            {"exit_code": 0, "stdout": "example.txt\n", "stderr": ""},
        )

    def test_multiple_actions_keep_their_call_ids(self):
        client, _ = fake_client(
            response(calls=[tool_call("call-1")]),
            response(
                calls=[
                    tool_call(
                        "call-2",
                        arguments=json.dumps({"command": "wc -l example.txt"}),
                    )
                ]
            ),
            response(content="Done."),
        )
        seen = []
        results = iter([Result(0, "example.txt\n", ""), Result(0, "4\n", "")])

        final = AgentSession(client, "test-model").run(
            "inspect the file",
            lambda action: (seen.append(action), next(results))[1],
        )

        self.assertEqual(final.text, "Done.")
        self.assertEqual([action.call_id for action in seen], ["call-1", "call-2"])

    def assert_session_error(self, first_response, code):
        client, _ = fake_client(first_response)
        events = []
        with self.assertRaises(AgentError) as raised:
            AgentSession(client, "test-model").run(
                "task", lambda _action: Result(0, "", ""), events.append
            )
        self.assertEqual(raised.exception.code, code)
        self.assertEqual(events[-1]["event"], "error")

    def test_rejects_malformed_arguments(self):
        self.assert_session_error(
            response(calls=[tool_call(arguments="not json")]),
            "invalid_arguments",
        )

    def test_rejects_unknown_tool(self):
        self.assert_session_error(
            response(calls=[tool_call(name="delete_file")]),
            "unknown_tool",
        )

    def test_rejects_multiple_tool_calls(self):
        self.assert_session_error(
            response(calls=[tool_call("one"), tool_call("two")]),
            "invalid_tool_calls",
        )

    def test_rejects_no_choices(self):
        self.assert_session_error(response(choices=False), "empty_response")

    def test_rejects_empty_final_response(self):
        self.assert_session_error(response(content=""), "empty_response")

    def test_allows_eight_actions_and_rejects_the_ninth(self):
        replies = [
            response(calls=[tool_call(f"call-{index}")]) for index in range(1, 10)
        ]
        client, _ = fake_client(*replies)
        actions = []
        events = []

        with self.assertRaises(AgentError) as raised:
            AgentSession(client, "test-model", max_steps=8).run(
                "keep going",
                lambda action: (actions.append(action), Result(0, "", ""))[1],
                events.append,
            )

        self.assertEqual(raised.exception.code, "step_limit")
        self.assertEqual(len(actions), 8)
        self.assertEqual(
            len([event for event in events if event["event"] == "result"]), 8
        )


if __name__ == "__main__":
    unittest.main()
