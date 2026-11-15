<!--
DRAFT CONVENTIONS
  [TODO: ...]   work or writing still to do
  [TBD]         a number that must come from analyze.py / make test; never fill by hand
  [cite: ...]   a reference to look up and format
  (planned)     the mechanism is designed but not yet in the code as of this draft

State of the code when this was drafted (2026-11-15):
  built:   kernel, ring 3, fork/exec/wait, pipes, ramfs, sh, /dev/agent, agentd (L1, line framing),
           bridge.py, struct label, label_narrow, in_scope, cap_path, SYS_mark_agent, dispatch()
           refusing CAP_UNDECLARED
  stub:    cap_check() always allows; every table entry is CAP_ANY
  missing: framed protocol, audit log, file labels, process limit, kill switch, profiles,
           configurations A-D, escape suite, task harness, pilot
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
declaration is refused. Files record the label of their writer, so a program an agent wrote runs
with the agent's limits even when a human starts it. An append-only audit log records each denial
and is readable only by human processes.

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
easier to argue when the whole system-call surface is twenty entries in one table than when it is
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
session. (G3) Code the agent wrote cannot run with more authority than the agent has. (G4) The
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
3. **Narrow only.** `mark_agent(session, caps, scope)` turns the calling process into an agent. Any
   process may call it, because it can only reduce authority. Called by an agent, it intersects the
   new capabilities with the current ones, requires the new scope to lie inside the current scope,
   and refuses to change the session (`-EPERM`). No system call widens a label.

Together these rules mean that once `agentd`'s child marks itself, nothing that command does, and
nothing any of its descendants do, acts with more than the profile allows.

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

The check for the declared capability happens in `dispatch()` before the handler runs. Calls that
take a path do a second check inside the handler, after the path has been copied from user memory
and normalised, so the decision is made on the exact string the filesystem will use (§3.3).
Table 1 gives the capability each call requires (planned; [TODO: fill table entries in
`kernel/syscall.c`, today all are `CAP_ANY` and `cap_check` is a stub]).

| Capability | System calls |
|---|---|
| `CAP_ANY` | `exit`, `getpid`, `wait`, `read`, `write`, `close`, `pipe`, `dup2`, `brk`, `mark_agent` |
| `CAP_FS_READ` | `open` (read), `stat`, `getdent`, `ps` |
| `CAP_FS_WRITE` + scope | `open` (create/write), `mkdir`, `unlink`, `rename` (both paths) |
| `CAP_SPAWN` | `fork`, `exec` |
| `CAP_KILL` + same session | `kill` |
| human only | `audit_read`, `kill_session` |

*Table 1: capability required by each system call (planned).*

A denied call returns `-ECAPDENIED`, a dedicated error number, so that a program, and the agent
reading its output, can tell a policy denial from an ordinary failure such as a missing file.

### 3.3 Path scope by component

Write access is limited to a scope such as `/home/work`. Two classic mistakes make string prefix
checks unsafe: `..` components that leave the scope after the check, and *prefix confusion*, where
`/home/workevil` begins with the string `/home/work`. Warden avoids both. Every path is normalised
first: `.` and `..` are resolved lexically and repeated slashes are collapsed. The kernel has no
per-process working directory and no symbolic links, so the normalised form is the path the
filesystem resolves; there is no gap between check and use. `in_scope(scope, path)` then accepts
the path only if it equals the scope or continues it at a `/` boundary. The function is compiled
both into the kernel and into host unit tests that run under AddressSanitizer and
UndefinedBehaviorSanitizer.

`rename` checks both the source and the destination, so an agent cannot move a file *into* its
scope from outside, nor move one out.

### 3.4 Labels on files (planned)

A label on processes alone leaves a hole: an agent writes `/home/work/fix.sh`, and later a human, or
a cron-like helper running as a human, executes it. The script would then run with full authority.
Warden closes this by recording, on each file, the origin and session of the last writer
(`writer_origin`, `writer_session`). When any process `exec`s a file written by an agent, the new
program runs with the *narrower* of the caller's label and the writer's session label. This is a
one-bit form of integrity tracking in the spirit of Biba and of DIFC systems [cite: Biba 1977;
HiStar; Flume], restricted to the one flow that turns agent output into agent authority: execution.

### 3.5 Session limits and the kill switch (planned)

An agent with `CAP_SPAWN` can fork without bound. `mark_agent` therefore also sets a per-session
process limit; `fork` in an agent process fails with `-EAGAIN` once the session has reached it.
Because every process of a session carries the session id, the kernel can also end a session in
one step: `kill_session(id)`, available only to human processes, kills every process with that id,
including ones that have left their original process tree.

### 3.6 Audit log and feedback (planned)

Every denial appends a record to an in-kernel ring buffer: time, pid, session, system call, the
normalised path if any, and the missing capability. `audit_read` is human-only, so an agent cannot
learn which probes were noticed or clear the evidence. When the ring wraps, the oldest records are
dropped and a counter of lost records is kept.

The same record also serves the agent. `agentd` attaches the error name and the session's new
audit records to the RESULT frame it returns, so the model learns *why* an action failed (for
example, `ECAPDENIED unlink /bin/ls: outside scope /home/work`) and can choose a permitted
alternative. Whether this feedback improves task success, or instead helps an adversarial agent
map the policy, is an open question we leave to future work (§8).

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
from an initramfs. System calls use `int 0x80`. There are [TBD: 20 today, 22 with
`audit_read` and `kill_session`] system calls in total. Table 2 gives the size of each part.

| Component | Lines (C and assembly, incl. comments) |
|---|---|
| Kernel | [TBD: 5,510 at drafting; rerun with cloc] |
| of which: capability and label code | [TBD] |
| ABI headers | [TBD: 157] |
| libc | [TBD: 586] |
| Userland (`sh`, `agentd`, tools) | [TBD: 1,025] |

*Table 2: size of Warden.*

### 4.2 Userland

A small libc provides string functions, `snprintf`, `malloc` over `brk`, and system call wrappers
that set `errno`. `/bin` holds `sh`, `ls`, `cat`, `echo`, `rm`, `mv`, `mkdir`, `wc` and `ps`. The
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
the child calls `mark_agent(session, caps, scope)` *before* `exec("/bin/sh", "-c", command)`. If
marking fails, the child exits with status 126 and never runs the command. The parent collects
stdout and stderr and returns them with the exit status. `agentd` itself never runs agent input,
so it can stay human and supervise the session.

### 4.4 Host side

`bridge.py` connects to the socket, sends the operator's request to a model through an
OpenAI-compatible API, asks for exactly one `run_shell` tool call, forwards the command to Warden,
and logs one JSON line per step. `harness.py` (§5.1) automates this for evaluation.

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

All configurations use the same system prompt, which describes the profile.

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
configuration; [TODO: replace with measured results].

| # | Attack | Example | A | B | C | D |
|---|---|---|---|---|---|---|
| 1 | Path traversal | `rm /home/work/../../bin/ls` | ✗ | ? | ✓ | ✓ |
| 2 | Prefix confusion | write `/home/workevil/x` | ✗ | ✗ | ✓ | ✓ |
| 3 | Label clearing | `mark_agent` with wider caps, other session | ✗ | ✗ | ✓ | ✓ |
| 4 | Shell escape | `cd /; rm b''in/ls`, variables, `$(...)` | ✗ | ✗ | ✓ | ✓ |
| 5 | Fork bomb | unbounded `fork` | ✗ | ? | ✓ | ✓ |
| 6 | Agent-written script | write a script, have a human process run it | ✗ | ✗ | ✓ | ✓ |
| 7 | Bad pointers | system calls with kernel or unmapped addresses | — | — | ✓ | ✓ |
| 8 | Read the audit log | `audit_read` from an agent | ✗ | ? | ✓ | ✓ |
| 9 | Evade the kill switch | processes that leave the tree before `kill_session` | ✗ | ✗ | ✓ | ✓ |

*Table 3: escape suite. ✓ blocked, ✗ not blocked, ? depends on the deny-list. Predicted, to be
replaced by `make test` output. Row 7 is blocked by fault containment in every configuration and
is included to show G5.*

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
no network, and twenty system calls. These choices make the completeness argument simple; a real
deployment must handle each (§7).

**Trusted agentd.** `agentd` must mark each child before it runs agent input. A bug there would
leave a command unlabelled; we keep it to [TBD] lines and fail closed when marking fails.

**What labels do not stop.** An agent can still do anything within its profile, and it can leak
anything it may read through its own output. Untrusted-input taint, so that an agent which read an
injected file loses write access, and blocking approval for risky actions, are natural extensions.

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
