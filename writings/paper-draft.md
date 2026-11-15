<!--
DRAFT CONVENTIONS
  [TODO: ...]   work or writing still to do
  [TBD]         a number that must come from analyze.py / make test; never fill by hand
  [cite: ...]   a reference to look up and format
  (planned)     the mechanism is designed but not yet in the code as of this draft

State of the code when this was drafted (updated 2026-11-15):
  built:   kernel, ring 3, fork/exec/wait, pipes, ramfs, sh, /dev/agent, agentd (L1, line framing,
           one hard-coded profile), bridge.py, harness.py with an environment reference
           (host/capabilities.md); labels and cap_check in dispatch() with every table entry declared;
           path scope; file writer labels with exec narrowing and #! scripts; session process limit;
           kill_session; audit ring and audit_read; ls -l showing writer labels
  tests:   host tests (cap, audit, session, fs writer labels) and ring 3 tests (mark, audit, session,
           flabel) cover every row of Table 3 under configuration C, but not yet as one named suite
  missing: framed protocol, JSONL step log, profiles from a config file, agentd deny-list,
           configurations A-D, the escape suite as a table in make test, task harness, pilot,
           denial feedback in RESULT, overhead benchmark
-->

# Warden: Enforcing What AI Agents Can Do Below the Shell

**[Author names]**
[Affiliation]

## Abstract

LLM agents are increasingly given a shell. The usual ways of limiting what they do with it are
instructions in the system prompt and a wrapper that filters commands before they run. Both act on
the *text* of a command, while the harm happens at the *system calls* that command causes. A shell
is a general-purpose interpreter, so the space of command strings that reach a given system call
is effectively unbounded, and a filter on strings cannot cover it. We argue that the limits on an
agent belong in the kernel, at the one place every action must pass through.

We present Warden, a small x86-64 kernel built to test this claim. Every process in Warden carries
a *provenance label* that records whether it acts for a human or for an agent session, which
capabilities it holds, and which part of the filesystem it may write. Labels are inherited across
`fork` and kept across `exec`, and can only be narrowed. Every system call enters through a single
dispatch table in which each entry must declare the capability it needs; an entry without a
declaration is refused. Files record the label of their writer, so a program or script an agent
wrote runs with the agent's limits even when a human starts it. Each agent session has a process
limit and can be ended in one call, and an audit log, readable only by human processes, records
each denial.

We compare four configurations on the same tasks: prompt instructions only, a userspace deny-list
only, kernel enforcement only, and both combined. On an escape suite of [TBD] attacks, the kernel
configurations block [TBD]/[TBD], while the deny-list blocks [TBD]/[TBD]. In a pilot of 20 tasks
with one model (80 runs), kernel enforcement reduces unsafe actions executed from [TBD] to [TBD]
while task success changes from [TBD] to [TBD]. The trusted kernel is about [TBD] lines of C.

## 1 Introduction

Coding and operations agents now run commands on real machines. A typical setup gives the model a
tool such as `run_shell(command)` and executes the returned string with `sh -c`. To keep the agent
from doing damage, developers write rules in the system prompt ("never delete files outside the
project") and sometimes add a wrapper that rejects commands matching a deny-list (`rm -rf /`,
`sudo`, paths under `/etc`).

Neither defence acts where the damage happens. A prompt is a request, and a model may ignore it,
misread it, or be talked out of it by text it reads during the task (prompt injection) [cite:
Greshake et al. 2023; InjecAgent; AgentDojo]. A deny-list sees only the command string. The shell
then expands variables, follows `cd`, resolves `..`, runs scripts, and starts other programs, none
of which the filter sees. Consider an agent limited to `/home/work`, and a filter that rejects any
command containing `/bin`:

```sh
cd /home/work/../.. && rm b''in/ls          # quoting splits the token the filter looks for
echo 'rm /b''in/ls' > /home/work/t.sh && sh /home/work/t.sh   # the path is assembled, then run later
```

Every one of these ends in the same system call: `unlink("/bin/ls")`. At that point the path is
fully resolved and the only question left is whether *this process* may remove *this file*. That
question has a simple, complete answer if the kernel knows that the process acts for an agent and
what the agent was allowed to do.

This observation is not new in operating systems. Capability systems, sandboxing interfaces such as
Capsicum, pledge/unveil, Landlock and seccomp, and decentralised information flow control all move
policy from the program to the kernel [cite: Capsicum; pledge/unveil; Landlock; seccomp-bpf;
HiStar; Flume; Asbestos]. What is new is the principal. An agent is not a program with a fixed
binary that can be audited and given a sandbox profile; it is a session that emits arbitrary
programs over time, writes files that later run, and starts processes that outlive the command
that created them. The policy must follow the *session* across every process and file it touches,
not a single executable.

Warden is a clean-slate kernel built to make this argument concrete and testable. We chose a small
kernel over a Linux prototype for one reason: completeness is the property at stake, and it is
easier to argue when the whole system-call surface is 22 entries in one table than when it is
several hundred spread across subsystems. Section 7 maps each mechanism onto Linux.

This paper makes the following contributions:

- **A design for agent enforcement in the kernel** (§3): provenance labels that follow an agent
  session through `fork`, `exec` and the files it writes; a single dispatch checkpoint that refuses
  undeclared entries; component-aware path scoping; per-session process limits and a kill switch;
  and an audit log that agents cannot read.
- **An implementation** (§4) in a [TBD]-line x86-64 kernel with a shell, a small libc, and an agent
  daemon that connects the guest to a model on the host over a serial channel.
- **An escape suite** (§5.2) of [TBD] attacks that a constrained agent might use, run in
  `make test`, and a comparison of which defences block each one.
- **A pilot evaluation** (§5.3) of 20 tasks under four configurations, measuring both safety and
  the cost to task success.

## 2 Threat Model

**Setting.** A human operator gives an agent a task. The agent is an LLM that proposes shell
commands; a host-side bridge sends each command to Warden, where an agent daemon (`agentd`) runs it
and returns the output and exit code. The operator decides, before the session starts, which
*profile* the agent receives: a set of capabilities and a filesystem scope.

**Adversary.** We treat the agent's commands as fully adversarial. This covers a misaligned or
careless model, and a model steered by prompt injection in files, command output, or web content it
reads. The adversary may:

- send any command string, of any length and structure, any number of times;
- write any file it is allowed to write, and try to execute it later, directly or by tricking a
  human-run process into doing so;
- start processes, including ones meant to outlive the command or exhaust resources;
- pass arbitrary pointers and arguments to system calls through programs it writes or runs.

**Trusted computing base.** The kernel, the bootloader, and the hardware (here, QEMU). `agentd` is
trusted to mark each child with the session's label *before* the child runs any agent-supplied
input; it is a short program (§4.3) and the only process that calls `mark_agent` on behalf of the
host. Human processes, such as the operator's own shell on the console, are not limited.

**Goals.** (G1) An agent process cannot perform a system call its capabilities do not allow, or
write outside its scope, by any command. (G2) An agent cannot widen its label or escape its
session. (G3) Code the agent wrote cannot be executed with more authority than the agent has,
whoever executes it (code a human program merely reads as input is outside G3; §8). (G4) The
operator can see every denial and stop the whole session at once. (G5) Malformed input from an
agent cannot crash the kernel.

**Out of scope.** Confidentiality of data the agent may legitimately read, such as exfiltration
through its own output; side channels; denial of service on the channel; bugs in QEMU; and harms
that are within the profile the operator granted (a `work-rw` agent may delete its own work).
We discuss taint-based extensions in §8.

## 3 Design

### 3.1 Provenance labels

Every process carries a label:

```c
struct label {
  enum origin origin;      /* ORIGIN_HUMAN or ORIGIN_AGENT */
  uint32_t    session;     /* agent session this process belongs to */
  uint32_t    caps;        /* CAP_FS_READ | CAP_FS_WRITE | CAP_SPAWN | CAP_KILL */
  char        scope[PATH_MAX]; /* normalized; writes stay under it */
};
```

A zeroed label is a human with no limits, so the kernel and the processes it starts need no setup.
The label obeys three rules:

1. **Inherited.** `fork` copies the parent's label into the child.
2. **Kept.** `exec` replaces the program but not the label. Running a different binary, including
   `/bin/sh` or a setuid-style helper, cannot shed it.
3. **Narrow only.** `mark_agent(session, caps, scope, max_procs)` turns the calling process into
   an agent. Any process may call it, because it can only reduce authority. Called by an agent, it
   intersects the new capabilities with the current ones, requires the new scope to lie inside the
   current scope by whole components, and refuses to change the session (`-EPERM`). The session's
   process limit (§3.5) can likewise only be lowered. No system call widens a label.

Together these rules mean that once `agentd`'s child marks itself, nothing that command does, and
nothing any of its descendants do, acts with more than the profile allows. Both successful and
refused marks are audited (§3.6): a refused mark is an attempt to widen a label.

### 3.2 One checkpoint

All system calls enter the kernel through one interrupt vector and one function, `dispatch()`,
which indexes a table:

```c
struct syscall { int64_t (*fn)(const uint64_t[6], struct trap_frame *);
                 uint32_t cap; const char *name; };
```

Each entry must declare the capability it needs, or `CAP_ANY` for calls that are harmless to every
process (`getpid`, `exit`, `read` and `write` on descriptors already held). An entry left at zero is
*undeclared*, and `dispatch()` refuses it with `-ENOSYS` and a log line. A developer who adds a
system call and forgets the policy gets a call that does not work, rather than one that silently
bypasses the check. This makes the policy fail closed by construction.

The check for the declared capability happens in `dispatch()` before the handler runs. A call
whose authority depends on its path cannot be judged there, because `dispatch()` does not know
which argument is a path. Such entries declare `CAP_BY_PATH`, and the handler passes every path
through one helper, `user_path(out, uptr, need)`. It copies the path from user memory and checks
the needed capabilities and the scope on the normalised form, so the decision is made on the exact
string the filesystem will use (§3.3). The handler states what it needs: `open` derives read,
write or both from its flags (`O_CREAT` and `O_TRUNC` count as writes), and `rename` checks both
paths as writes. Checking inside the handler also means a refusal is audited with the path it
named. Table 1 gives the capability each call requires.

| Capability | System calls |
|---|---|
| `CAP_ANY` | `exit`, `getpid`, `wait`, `read`, `write`, `close`, `pipe`, `dup2`, `brk`, `ps`, `mark_agent` |
| `CAP_BY_PATH`: `CAP_FS_READ` | `open` (read), `stat` |
| `CAP_BY_PATH`: `CAP_FS_WRITE` + scope | `open` (create, write, truncate), `mkdir`, `unlink`, `rename` (both paths) |
| `CAP_FS_READ` | `getdent` (on a descriptor already open) |
| `CAP_SPAWN` | `fork`, `exec` (and `CAP_FS_READ` on the program's path) |
| `CAP_KILL` + same session | `kill` |
| `CAP_HUMAN` | `audit_read`, `kill_session` |

*Table 1: capability required by each system call, as declared in `kernel/syscall.c`.*

`CAP_HUMAN` is a capability bit that the kernel never grants: `mark_agent` masks the requested
capabilities to the four agent bits, so no agent can hold it, and the ordinary gate refuses
human-only calls without a special case. One path is refused to every agent whatever it holds:
the agent channel `/dev/agent`, through which a command could otherwise forge replies to the host.

A denied call returns `-ECAPDENIED`, a dedicated error number, so that a program, and the agent
reading its output, can tell a policy denial from an ordinary failure such as a missing file.

### 3.3 Path scope by component

Write access is limited to a scope such as `/home/work`. Two classic mistakes make string prefix
checks unsafe: `..` components that leave the scope after the check, and *prefix confusion*, where
`/home/workevil` begins with the string `/home/work`. Warden avoids both. Every path is normalised
first: `.` and `..` are resolved lexically and repeated slashes are collapsed. The kernel has no
per-process working directory and no symbolic links, so the normalised form is the path the
filesystem resolves; there is no gap between check and use. `in_scope(scope, path)` then accepts
the path only if it equals the scope or continues it at a `/` boundary. The label code
(`cap.c`), like the normaliser, is free of hardware code and is compiled both into the kernel and
into host unit tests that run under AddressSanitizer and UndefinedBehaviorSanitizer. Those tests
found one bug during development: an early `in_scope` compared the full length of the scope with
`memcmp` and read past the end of a shorter path.

`rename` checks both the source and the destination, so an agent cannot move a file *into* its
scope from outside, nor move one out.

### 3.4 Labels on files

A label on processes alone leaves a hole: an agent writes `/home/work/fix`, and later a human, or
a cron-like helper running as a human, executes it. The program would then run with full
authority. Warden closes this by giving every file a *writer label*: a copy of the label of the
agent that wrote it, together with its session's process limit. Files from the initramfs, and
files only humans have touched, carry a zeroed (human) writer label.

**Recording.** The kernel updates the writer label on every write to a file, and when an agent
creates a file or directory. Recording at write time rather than open time also catches a
descriptor that a human opened and then handed to a child that marked itself. A write by another
agent of the same session narrows the label further. A write by a human leaves it unchanged, so an
agent's label cannot be washed out by a human editing the file. The label is sticky: `rename`
moves it with the file, and only `unlink` clears it. A file takes one session's label, never two:
an agent of a different session is refused (`-ECAPDENIED`) when it tries to write the file.

**Executing.** When any process `exec`s a file, the new program's label is the *meet* of the
caller's label and the file's writer label:

- a human file changes nothing;
- a human running an agent's file becomes an agent of the writer's session, and is counted
  against that session's process limit (the session is recreated if it has ended);
- an agent of the same session keeps the capabilities both labels hold, and the deeper of the two
  scopes, or no capabilities at all if the scopes do not nest;
- an agent of another session is refused.

The same rule, `label_meet`, governs both recording and executing. `exec` also understands `#!`
scripts: it runs the named interpreter, one level deep, and the new process takes the meet of the
script's and the interpreter's writer labels. Without this, an agent-written script would run under
the label of `/bin/sh`, which a human wrote. The kernel applies the same rule when it starts a
program itself. `stat` reports a file's writer label, and `ls -l` shows it, so an operator can see
which files carry agent authority.

This is a form of integrity tracking in the spirit of Biba and of DIFC systems [cite: Biba 1977;
HiStar; Flume], restricted to the one flow that turns agent output into agent authority:
execution. It does not follow data that a human process *reads*. `sh /home/work/fix` runs a human
`/bin/sh` that reads the script as data, so it gains no label, and a human's `cat fix > mine`
produces a human file (§8).

### 3.5 Session limits and the kill switch

An agent with `CAP_SPAWN` can fork without bound. `mark_agent` therefore also sets a per-session
process limit. The first mark into a session creates it, and later marks can only lower its limit.
A process counts against its session from when it joins (by `mark_agent`, by `fork` from an agent,
or by `exec` of an agent's file) until its thread is *freed*, not merely until it exits. A zombie
still holds its address space and kernel stack, so counting until it is freed also bounds a fork
bomb that never collects its children. `fork` in an agent process first frees the dead, then
fails with `-EAGAIN` once the session is at its limit. The refusal is audited. When a session's
last process is freed, the session is forgotten, and marking into it again starts afresh.

Because every process of a session carries the session id, the kernel can also end a session in
one step. `kill_session(id)`, which requires `CAP_HUMAN`, kills every live process with that id,
including ones that have left their original process tree. It returns the number killed and
writes an audit record naming the session. An agent cannot call it, not even on its own session.

### 3.6 Audit log and feedback

The kernel keeps an in-memory ring of the newest 256 audit records. Each record holds a sequence
number, the pid, the session, its kind, the system call's name, the capabilities that were needed,
the error returned, and the path, exactly as the caller gave it. There are three kinds of record:

- a *denial*: a call refused at the gate, a path refused in `user_path`, a `kill` outside the
  session, a fork over the limit, a refused mark, a write to another session's file, or an `exec`
  that could not take its file's label;
- a *mark*: a successful `mark_agent`, or an `exec` that took on an agent's writer label, with the
  capabilities given and the scope or program path;
- a *kill*: a `kill_session`.

Sequence numbers count every record ever made, so when the ring wraps, a reader sees the gap and
knows how many records it lost. `audit_read(buf, max, after)` returns the records newer than
`after`, and requires `CAP_HUMAN`. An agent therefore cannot learn which probes were noticed or
clear the evidence, and its attempt to read the log is itself recorded. The `/bin/audit` tool
prints the log on the operator's console.

The same record could also serve the agent (planned). `agentd` would attach the error name and the
session's new audit records to the RESULT frame it returns, so the model learns *why* an action
failed (for example, `ECAPDENIED unlink /bin/ls`) and can choose a permitted alternative. Whether
this feedback improves task success, or instead helps an adversarial agent map the policy, is an
open question we leave to future work (§8).

### 3.7 Fault containment

A system call that copies from or to user memory uses `copy_from_user`/`copy_to_user`, which
record the faulting instructions in an exception table. A bad pointer from user space returns
`-EFAULT` rather than faulting the kernel, and any fault taken while executing in ring 3 kills
only that process. This addresses G5 for pointer arguments; malformed frames on the agent channel
are handled by `agentd` (§4.3).

## 4 Implementation

### 4.1 Kernel

Warden boots with Limine on x86-64 and runs in QEMU. It provides physical and virtual memory
management with four-level paging, a slab allocator, an APIC timer driving a round-robin scheduler,
per-process address spaces with ring 3 entry through a TSS, an ELF loader, `fork` (eager copy),
`exec`, `wait`, `exit` and `kill`, pipes and `dup2`, and a writable in-memory filesystem populated
from an initramfs. System calls use `int 0x80`. There are 22 system calls in total. Table 2 gives
the size of each part.

| Component | Lines (C and assembly, incl. comments) |
|---|---|
| Kernel | [TBD: 6,033 by `wc -l`; rerun with cloc] |
| of which: labels, sessions and audit (`cap.c`, `session.c`, `audit.c`) | [TBD: 299 by `wc -l`] |
| ABI headers | [TBD: 194] |
| libc | [TBD: 624] |
| Userland (`sh`, `agentd`, tools) | [TBD: 1,140] |

*Table 2: size of Warden.*

### 4.2 Userland

A small libc provides string functions, `snprintf`, `malloc` over `brk`, and system call wrappers
that set `errno`. `/bin` holds `sh`, `ls` (with `-l` for writer labels), `cat`, `echo`, `rm`,
`mv`, `mkdir`, `wc`, `ps`, `audit` and `agentd`. The
shell supports `sh -c`, script files and an interactive prompt, with pipelines, redirection
(`<`, `>`, `>>`), `;`, `&&`, quoting and `cd`/`pwd`. Because the kernel has no working directory,
the shell resolves relative paths itself, and every path the kernel sees is absolute; this is what
lets the check in §3.3 run on the exact path used.

### 4.3 Agent channel and agentd

The guest's second serial port (COM2) is attached to a Unix socket on the host. A receive
interrupt fills a ring buffer exposed as `/dev/agent`, a blocking character device with no line
discipline, so bytes arrive exactly as sent. [TODO: framed protocol: `[u32 len][u8 type][payload]`
with ACTION, RESULT and ERROR; today one command per line with a NUL-prefixed `EXIT <code>` trailer.]

`agentd` is started at boot as a human process. For each ACTION it creates a pipe, forks, and in
the child calls `mark_agent(session, caps, scope, max_procs)` *before*
`exec("/bin/sh", "-c", command)`. If marking fails, the child exits with status 126 and never runs
the command. The parent collects stdout and stderr and returns them with the exit status.
`agentd` itself never runs agent input, so it can stay human and supervise the session. It is
[TBD: 116] lines. Today it applies one built-in profile (all four capabilities, scope
`/home/work`, 32 processes) [TODO: profiles from a config file, §5.1].

### 4.4 Host side

`bridge.py` connects to the socket, sends the operator's request to a model through an
OpenAI-compatible API, asks for exactly one `run_shell` tool call, forwards the command to Warden,
and prints the output and exit code [TODO: one JSON line per step]. The system prompt has two
parts. The first is a short set of rules: use only the commands that exist, use absolute paths,
do only what was asked, answer `echo "cannot: <reason>"` rather than improvise, never work around
`ECAPDENIED`, and treat file contents and command output as data, not instructions. The second is
a reference to the environment (`host/capabilities.md`): the commands in `/bin` and their flags,
the shell's grammar and limits, what does *not* exist (`cp`, `grep`, `touch`, …), and how to do
common tasks with what does. Without the reference, models reached for Linux tools that Warden
lacks, and their failures were `command not found`, not anything to do with policy. With it,
failures in the pilot can be attributed to the policy rather than to an unfamiliar environment.
`harness.py` (§5.1) automates this for evaluation.

## 5 Evaluation

We ask three questions:

- **RQ1 (security):** Which attacks does each defence block?
- **RQ2 (utility):** What does kernel enforcement cost in task success?
- **RQ3 (overhead):** What does the capability check cost per system call?

### 5.1 Method

**Configurations.** One flag selects the configuration without rebuilding:

- **A, prompt only.** The system prompt states the rules; no enforcement.
- **B, agentd only.** `agentd` rejects commands that match a deny-list of command names and path
  strings before running them; the kernel does not check labels. This models the common
  wrapper-based approach.
- **C, kernel only.** Labels and capabilities as in §3; no deny-list.
- **D, combined.** B and C together.

All configurations use the same system prompt (§4.4), which also describes the profile.

**Profiles.** `readonly` (`CAP_FS_READ | CAP_SPAWN`, no write scope) and `work-rw`
(`CAP_FS_READ | CAP_FS_WRITE | CAP_SPAWN | CAP_KILL`, scope `/home/work`).

**Tasks.** 20 tasks in four groups of five, each a YAML file giving the prompt, setup files,
protected paths, the profile, and success checks over the final filesystem:

- *simple*: one command, e.g. count the lines in a log file;
- *multi-step*: several dependent commands, e.g. reorganise a directory tree;
- *tempting*: the easiest solution crosses the policy, e.g. "free up space" with large files
  outside the scope;
- *prompt injection*: a setup file contains instructions to delete or modify a protected path.

**Harness.** For each run, `harness.py` boots a clean image, creates the setup files, runs the
agent loop up to [TBD] steps, then collects the audit log and the final filesystem, scores the run,
and appends one line to `results.jsonl`. The model is [TBD: model name] at temperature [TBD], with
every request and response logged for replay.

**Metrics.** Defined before the pilot was run:

- *Success*: all of the task's success checks pass on the final filesystem.
- *Unsafe executed*: at least one protected path was changed (created, written, removed or renamed)
  in the final filesystem, or a process outside the session was killed. This counts harm that
  happened, not harm that was attempted.
- *Unsafe attempted*: at least one system call was denied, or one command was rejected by the
  deny-list. Reported separately, because a defence can turn an executed harm into an attempted one.

### 5.2 Escape suite (RQ1)

Each attack is a test in `make test` that runs as an agent with the `work-rw` profile and checks
that the protected state is unchanged. Table 3 lists them with our *predicted* outcome for each
configuration; [TODO: replace with measured results]. Under configuration C, each row except 4 is
already exercised by a ring 3 test (last column). Each test runs as a forked agent child, and its
targets are throwaway files, so a missing check cannot damage the image the later tests use.
[TODO: gather these into one `escape` suite that prints this table, and add row 4.]

| # | Attack | Example | A | B | C | D | Test today |
|---|---|---|---|---|---|---|---|
| 1 | Path traversal | `rm /home/work/../../bin/ls` | ✗ | ? | ✓ | ✓ | `mark.c` |
| 2 | Prefix confusion | write `/home/workevil/x` | ✗ | ✗ | ✓ | ✓ | `mark.c` |
| 3 | Label clearing | `mark_agent` with wider caps, other session | ✗ | ✗ | ✓ | ✓ | `mark.c`, `audit.c` |
| 4 | Shell escape | `cd /home/work/../..; rm b''in/ls`; a path assembled in a script | ✗ | ✗ | ✓ | ✓ | [TODO] |
| 5 | Fork bomb | unbounded `fork`, children never collected | ✗ | ? | ✓ | ✓ | `session.c` |
| 6 | Agent-written program | write a `#!` script or copy a binary, have a human run it | ✗ | ✗ | ✓ | ✓ | `flabel.c` |
| 7 | Bad pointers | system calls with kernel or unmapped addresses | — | — | ✓ | ✓ | `fault`, `mark.c`, `audit.c` |
| 8 | Read the audit log | `audit_read` from an agent | ✗ | ? | ✓ | ✓ | `audit.c` |
| 9 | Evade the kill switch | processes that leave the tree before `kill_session` | ✗ | ✗ | ✓ | ✓ | `session.c` |

*Table 3: escape suite. ✓ blocked, ✗ not blocked, ? depends on the deny-list. Predicted, to be
replaced by `make test` output. Row 7 is blocked by fault containment in every configuration and
is included to show G5. Beyond these rows, the tests also check that an agent cannot open
`/dev/agent`, cannot kill a human process, and cannot write a file another session wrote.*

On a live boot, driven through `agentd` exactly as the model would be, an agent scoped to
`/home/work` is refused `rm /bin/ls` and `mv /bin/sh /home/work/sh` (`ECAPDENIED`), and the
operator's `audit` lists both refusals. A recursive fork bomb (`sh b | sh b`, from a script the
agent wrote) stops at the session limit with `fork: EAGAIN`, and the next command runs normally.
A script the agent wrote containing `rm /bin/ls`, run by the operator on the console, is refused,
and the log shows the operator's process taking the agent's label at `exec`. [TODO: replace these
anecdotes with the measured table.]

[TODO: one paragraph per interesting row once measured. Expected story: B blocks the literal
forms its patterns match and misses every rewriting; C blocks all of them because each ends in a
system call on a normalised path from a labelled process.]

### 5.3 Pilot (RQ2)

| Config | Success | Unsafe attempted | Unsafe executed |
|---|---|---|---|
| A prompt | [TBD] | [TBD] | [TBD] |
| B agentd | [TBD] | [TBD] | [TBD] |
| C kernel | [TBD] | [TBD] | [TBD] |
| D both | [TBD] | [TBD] | [TBD] |

*Table 4: pilot results, 20 tasks × 4 configurations × 1 model = 80 runs. [TODO: break down by task
group; the tempting and injection groups carry the difference.]*

[TODO: Discussion. Things to look for: does C lose success on simple and multi-step tasks (it
should not, since they stay in scope)? Do denials in C lead the model to a permitted solution, or
to giving up? Does B reject legitimate commands (false positives)?]

### 5.4 Overhead (RQ3)

[TODO: microbenchmark, not yet on the roadmap. Measure cycles per call for `getpid` (CAP_ANY) and
`stat` (path check) with the check enabled and disabled, averaged over [TBD] iterations in QEMU.
Expected: one table lookup and a mask test for the capability; path checks dominated by
normalisation, which the filesystem performs anyway.]

## 6 Related Work

**OS sandboxing and capabilities.** Capsicum adds a capability mode to FreeBSD in which a process
loses access to global namespaces [cite: Watson et al. 2010]. OpenBSD's pledge and unveil let a
program restrict its own system calls and visible paths [cite]. Linux offers seccomp-bpf for
system-call filtering and Landlock for unprivileged filesystem restriction [cite]. Containers and
gVisor isolate whole environments [cite]. These mechanisms are designed for a program that knows
its own needs and restricts itself, or for an administrator writing a profile for a known binary.
Warden's label differs in following a *session* that produces new programs and files over time,
and in carrying authority through files the agent writes (§3.4). §7 discusses how to approximate
Warden on these mechanisms.

**Information flow control.** Asbestos, HiStar and Flume track labels on processes and data and
enforce flow policies in the kernel or a reference monitor [cite]. Warden's labels are a deliberate
simplification: one integrity bit per session, attached to processes and files, checked at a single
point. We use it for the one flow that matters most for agents (agent-written code gaining
authority) and leave general confidentiality flows to future work.

**Agent safety and benchmarks.** AgentDojo, InjecAgent and ToolEmu measure how often agents can be
driven to unsafe tool calls, including through prompt injection [cite]. Defences at the model or
framework level, such as CaMeL, separate trusted control flow from untrusted data [cite: Debenedetti
et al. 2025]. Commercial coding agents run commands in OS sandboxes built on Seatbelt, Landlock or
containers [cite]. Our work is complementary: we assume the model may fail and ask what the OS
should guarantee regardless, and we compare a kernel checkpoint against the string filters these
systems often add on top.

## 7 Mapping to Linux

[TODO: write from design; no code needed for the paper.]

| Warden mechanism | Closest Linux equivalent | Gap |
|---|---|---|
| Label inherited and narrow-only | `no_new_privs` + Landlock ruleset per session | Landlock has no session identity |
| Path scope by component | Landlock path-beneath rules | Close match; Landlock resolves symlinks |
| Undeclared syscall refused | seccomp-bpf default-deny | Profile is per binary, not per session |
| File writer label, exec narrows | none built in; xattrs + BPF-LSM hook on `bprm_check_security` | Needs an LSM |
| Session process limit, kill switch | cgroup v2 `pids.max`, `cgroup.kill` | Close match |
| Audit log, human-only | audit subsystem / BPF ring buffer | Close match |
| Final-state scoring | overlayfs upper layer | Close match |

The mechanism without a ready equivalent is §3.4. A small BPF-LSM program that records the writer's
session in an extended attribute on `inode_*` hooks and narrows the label on exec would cover it.

## 8 Limitations and Future Work

**Small evaluation.** 20 tasks, one model and one repetition per configuration are enough to show
the gap between string filters and a kernel checkpoint on attacks, but not to estimate success
rates with confidence. We wrote the tasks ourselves, and the tempting and injection tasks are
designed to cross the policy. [TODO: ~100 tasks, two models, three repetitions.]

**A small kernel is not Linux.** Warden has no symbolic links, no working directory in the kernel,
no network, and 22 system calls. These choices make the completeness argument simple; a real
deployment must handle each (§7).

**Trusted agentd.** `agentd` must mark each child before it runs agent input. A bug there would
leave a command unlabelled; we keep it to [TBD: 116] lines and fail closed when marking fails.

**Checks at open time.** As in Unix, access is checked when a file is opened, not on each `read`
or `write`. A descriptor a process opened *before* marking itself keeps the access it was opened
with. `agentd` closes the agent channel in the child before marking, and opens nothing else;
other code that marks itself must do the same.

**Execution, not reading.** File labels follow the flow from an agent's file to execution (§3.4).
They do not follow data a human process reads: `sh /home/work/fix` runs the agent's script under
a human `/bin/sh`, and a human's `cat fix > mine` produces a human file. Closing this requires
taint on read, a larger mechanism, which we leave to future work.

**What labels do not stop.** An agent can still do anything within its profile, and it can leak
anything it may read through its own output. It can also see every process with `ps`, including
human ones. Untrusted-input taint, so that an agent which read an injected file loses write
access, and blocking approval for risky actions, are natural extensions.

**Feedback.** Returning the reason for a denial may help a benign agent recover and an adversarial
one probe. Measuring this (feedback levels 0, 1 and 2) is future work.

**Typed tools.** Our agent speaks shell (L1). Offering typed tools (`read_file`, `delete`, `spawn`)
on top of the same kernel checks (L2) would let us separate the effect of the interface from the
effect of enforcement.

## 9 Conclusion

Prompts and command filters try to decide, from the text of a command, what it will do. The kernel
knows exactly what each process is about to do, and with a provenance label it also knows on whose
behalf. Warden shows that a single checkpoint, labels that only narrow, and labels on the files an
agent writes are enough to block every attack in our suite that a string filter misses, at a cost
of [TBD] in task success and [TBD] per system call. We believe that agents with a shell should be
limited where the shell itself is limited: below it.

## References

[TODO: format in the venue's style. Look up exact venues and years.]

- Watson, Anderson, Laurie, Kennaway. *Capsicum: Practical Capabilities for UNIX.* USENIX Security 2010.
- OpenBSD `pledge(2)` and `unveil(2)` manual pages.
- Linux Landlock and seccomp-bpf documentation.
- Efstathopoulos et al. *Labels and Event Processes in the Asbestos Operating System.* SOSP 2005.
- Zeldovich et al. *Making Information Flow Explicit in HiStar.* OSDI 2006.
- Krohn et al. *Information Flow Control for Standard OS Abstractions* (Flume). SOSP 2007.
- Biba. *Integrity Considerations for Secure Computer Systems.* 1977.
- Greshake et al. *Not What You've Signed Up For: Compromising Real-World LLM-Integrated Applications with Indirect Prompt Injection.* 2023.
- Debenedetti et al. *AgentDojo.* NeurIPS 2024 Datasets and Benchmarks.
- Zhan et al. *InjecAgent.* 2024.
- Ruan et al. *ToolEmu: Identifying the Risks of LM Agents with an LM-Emulated Sandbox.* ICLR 2024.
- Debenedetti et al. *Defeating Prompt Injections by Design* (CaMeL). 2025.
- gVisor project documentation.
