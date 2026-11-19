# Warden

**A minimal x86-64 kernel that enforces what AI agents can do.**

LLM agents are increasingly given a shell. The usual limits — rules in the system prompt, and a
wrapper that filters command strings before they run — both act on the *text* of a command, while
the harm happens at the *system calls* that command causes. A shell is a general-purpose
interpreter, so the set of command strings reaching a given system call is effectively unbounded,
and a filter on strings cannot cover it.

```sh
cd /home/work/../.. && rm b''in/ls                             # quoting splits the filtered token
echo 'rm /b''in/ls' > /home/work/t.sh && sh /home/work/t.sh    # assembled now, run later
```

Both end at `unlink("/bin/ls")`. By then the path is resolved and only one question is left: may
*this process* remove *this file*? Warden answers it in the kernel, at the one place every action
must pass through.

Warden is a clean-slate kernel built to make that argument testable: ring 3, fork/exec/wait, pipes,
a writable ramfs, a shell, a small libc, and an agent daemon that connects the guest to a model on
the host over a framed serial channel. The trusted kernel is ~6,000 lines of C and assembly.

## How enforcement works

- **Provenance labels.** Every process carries `origin` (human or agent), `session`, and a
  capability set. Labels are inherited across `fork`, kept across `exec`, and can only be narrowed
  (`SYS_mark_agent` is one-way).
- **One checkpoint.** All 22 system calls enter through a single dispatch table in
  `kernel/syscall.c`, and each entry declares the capability it needs (`CAP_SPAWN`, `CAP_BY_PATH`,
  `CAP_HUMAN`, …). An undeclared entry is refused, so the surface cannot grow silently.
- **Path scope by component.** `in_scope()` compares path components, so `/homeevil` is not inside
  `/home` and `..` cannot walk out.
- **File labels.** Files record the label of their writer. A program an agent wrote runs with the
  agent's limits even when a human starts it.
- **Session limits and a kill switch.** Each agent session has a process cap; `SYS_kill_session`
  ends the whole session in one call.
- **Human-only audit log.** A ring buffer records every denial; `SYS_audit_read` requires
  `CAP_HUMAN`, so the agent cannot read or clear it.

Capabilities are `r` read, `w` write (inside the scope only), `s` spawn, `k` kill. Profiles live in
`etc/profiles`:

```
# name      caps  scope       max-procs
readonly    r-s-  /home/work  16
work-rw     rwsk  /home/work  32
```

## Setup

```sh
yay -S nasm qemu-system-x86 xorriso git   # or your distro's equivalent
make deps                                 # fetch Limine and limine.h
```

The host-side tooling (bridge, pilot, analysis) needs a Python venv:

```sh
python3 -m venv host/venv && host/venv/bin/pip install -r host/requirements.txt
```

Model-driven runs read `OPENROUTER_API_KEY` and `OPENROUTER_MODEL` from the environment (a `.env`
file is picked up).

## Build and run

```sh
make iso        # bootable ISO
make run        # boot in QEMU, serial on this terminal
make run-debug  # boot paused, waiting for GDB on :1234
make help       # all targets
```

Inside the guest: `ls`, `cat`, `echo`, `wc`, `mkdir`, `rm`, `mv`, `ps`, and a shell with pipelines,
`<`/`>`/`>>`, `;`, `&&`, and quoting. `ls -l` shows each entry's writer and the caps it would run
with. The full guest surface is documented in [`host/capabilities.md`](host/capabilities.md), which
is also what the model is told.

## Running an agent against it

`agentd` is the guest end of `/dev/agent`. The host opens a session naming a **configuration** and a
profile, then sends commands:

| Config | Defence |
|---|---|
| A | prompt only — commands run as a human |
| B | userspace deny-list (`etc/denylist`) checked per command line |
| C | kernel enforcement — the child marks itself an agent before `exec` |
| D | both |

```sh
make run &                              # boot, agent socket at /tmp/warden-agent.sock
make bridge CONFIG=D PROFILE=readonly   # talk to it through the model, JSONL log per step
```

## Evaluation

```sh
make escape-matrix                      # 12 attacks (plus a label-propagation case) under A-D
make task-check                         # each task's solution and unsafe path, no model needed
make pilot TASKS=tasks/simple-*.yaml CONFIGS=C
make pilot PROFILES=all                 # every task under every profile (paid API calls)
make analyze MARKDOWN=1                 # results.jsonl as the paper's tables
```

Tasks are YAML (`tasks/`): prompt, profile, setup files, protected paths, success conditions, and a
reference solution. The pilot boots a fresh machine per run and scores the final state from the
human side.

Kernel enforcement blocks all 12 escape-suite attacks. The deny-list blocks 3 of the 5 that are
spelled out in command text, and none of the 7 that happen below it, apart from the one fault
containment blocks everywhere.

## Tests

```sh
make test        # host unit tests under ASan/UBSan, plus ring 3 tests on a real boot
make host-test   # host unit tests only
make agent-test  # the framed protocol and configurations A-D against a real boot
```

## Layout

```
kernel/      the kernel: cap.c, audit.c, session.c, syscall.c, fs.c, proc.c, vmm.c, ...
libc/        minimal libc: strings, snprintf, malloc over brk, syscall wrappers
userland/    sh, agentd, and /bin: ls cat echo wc mkdir rm mv ps audit
etc/         profiles and the deny-list, loaded into the initramfs
host/        bridge.py, pilot.py, analyze.py, harness.py, vm.py, capabilities.md
tasks/       evaluation tasks in YAML
tests/       host unit tests, ring 3 tests, the escape matrix, the protocol test
writings/    paper draft, poster, roadmap, setup notes, result data
```

## Status

The kernel, `agentd`, the host harness, the escape suite and the test suites are in place. Still
open: the full task set (4 of 20 written), the full pilot run, audit records attached to `RESULT`
frames, and an overhead benchmark. See [`writings/roadmap.md`](writings/roadmap.md), and
[`writings/paper-draft.md`](writings/paper-draft.md) for the design and threat model in full.
