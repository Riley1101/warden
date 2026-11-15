"""The pilot's results table, from results.jsonl (paper, §5.3).

Per configuration: how many runs succeeded, executed something unsafe,
and attempted something unsafe, overall and per task group. Runs under
the profile the task was written for make the main table; runs under
another profile (pilot.py --profiles) are reported apart, since a task's
success checks only mean something under its own profile.

    python analyze.py                      # results.jsonl
    python analyze.py --markdown runs.jsonl
    python analyze.py --model google/gemini-3.8-flash
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results.jsonl"
CONFIGS = "ABCD"
GROUPS = ("simple", "multi-step", "tempting", "injection")
METRICS = (("success", "success"), ("unsafe executed", "unsafe_executed"),
           ("unsafe attempted", "unsafe_attempted"))


def load(path: Path) -> list[dict[str, Any]]:
    runs = []
    for n, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if text.strip():
            try:
                runs.append(json.loads(text))
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{n}: {error}") from error
    return runs


def rate(runs: list[dict[str, Any]], key: str) -> str:
    """'k/n (p%)', or '-' with no runs."""
    if not runs:
        return "-"
    k = sum(1 for r in runs if r.get(key))
    return f"{k}/{len(runs)} ({100 * k / len(runs):.0f}%)"


def table(title: str, rows: list[tuple[str, list[str]]], heads: list[str], md: bool) -> str:
    if md:
        out = [f"**{title}**", "", "| " + " | ".join(heads) + " |",
               "|" + "---|" * len(heads)]
        out += ["| " + " | ".join([name] + cells) + " |" for name, cells in rows]
        return "\n".join(out) + "\n"
    widths = [max(len(h), *(len(([n] + c)[i]) for n, c in rows)) for i, h in enumerate(heads)]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    out = [title, fmt.format(*heads)]
    out += [fmt.format(name, *cells) for name, cells in rows]
    return "\n".join(out) + "\n"


def by_config(runs: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    groups = defaultdict(list)
    for r in runs:
        groups[r["config"]].append(r)
    return groups


def present(runs: list[dict[str, Any]]) -> list[str]:
    return [c for c in CONFIGS if any(r["config"] == c for r in runs)]


def report(runs: list[dict[str, Any]], md: bool) -> str:
    own = [r for r in runs if r.get("profile") == r.get("task_profile", r.get("profile"))]
    other = [r for r in runs if r not in own]
    parts = []

    configs = present(own)
    cfg = by_config(own)
    heads = ["config", "runs", *(m for m, _ in METRICS), "errors"]
    rows = [(c, [str(len(cfg[c])), *(rate(cfg[c], k) for _, k in METRICS),
                 str(sum(1 for r in cfg[c] if str(r.get("stopped", "")).startswith("error")))])
            for c in configs]
    if rows:
        parts.append(table("Each task under its own profile", rows, heads, md))

    for label, key in METRICS:
        rows = []
        for g in GROUPS:
            in_group = [r for r in own if r["group"] == g]
            if in_group:
                gc = by_config(in_group)
                rows.append((g, [rate(gc[c], key) for c in configs]))
        if rows:
            parts.append(table(f"{label.capitalize()}, by task group", rows,
                               ["group", *configs], md))

    if other:
        configs = present(other)
        rows = []
        for profile in sorted({r["profile"] for r in other}):
            pc = by_config([r for r in other if r["profile"] == profile])
            for label, key in METRICS:
                rows.append((f"{profile}: {label}", [rate(pc[c], key) for c in configs]))
        parts.append(table("Under a profile the task wasn't written for (success means little here)",
                           rows, ["profile: metric", *configs], md))

    models = sorted({r.get("model", "?") for r in runs})
    parts.append(f"{len(runs)} runs; model {', '.join(models)}; "
                 f"temperature {', '.join(sorted({str(r.get('temperature')) for r in runs}))}\n")
    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarise pilot results by configuration.")
    parser.add_argument("results", nargs="?", type=Path, default=RESULTS)
    parser.add_argument("--model", help="only runs with this model")
    parser.add_argument("--markdown", action="store_true", help="tables for the paper")
    args = parser.parse_args()

    try:
        runs = load(args.results)
    except (OSError, ValueError) as error:
        print(f"analyze: {error}", file=sys.stderr)
        return 2
    runs = [r for r in runs if r.get("model") != "script"]
    if args.model:
        runs = [r for r in runs if r.get("model") == args.model]
    if not runs:
        print("analyze: no model runs to summarise", file=sys.stderr)
        return 1
    print(report(runs, args.markdown), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
