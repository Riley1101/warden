# Warden: Enforcing What AI Agents Can Do Below the Shell

**Poster copy — working draft**  
**Status:** implementation facts below are verified against the repository on 2026-11-15. The
four-configuration pilot, escape-suite comparison, and overhead benchmark are still planned; their
result fields are deliberately left explicit rather than estimated.

## 1. Abstract

LLM agents increasingly execute shell commands, but prompts and userspace command filters reason
about text while damage occurs through system calls. Warden is a small x86-64 research kernel that
labels every agent process with a session, capability set, and writable filesystem scope; labels
persist across `fork` and `exec`, and a fail-closed dispatch table checks every system call. We
validate the current prototype with host unit tests and end-to-end QEMU tests, and plan an 80-run
comparison of prompt-only, userspace-filter, kernel-only, and combined configurations on 20 tasks.
The current implementation passes all 57 available test groups—11 host binaries and 46 in-kernel
tests—while comparative safety, utility, and overhead results remain to be measured.

> This is four sentences and approximately 105 words. Keep it until the comparative evaluation is
> complete, then replace the final sentence with the measured headline result.

**Final-sentence template after evaluation:**  
Across **[N] escape attacks** and **80 task runs**, kernel enforcement blocked **[K/N] attacks** and
reduced unsafe execution from **[A%] to [C%]**, while task success changed from **[B%] to [D%]**.

## 2. Introduction / Background

The AgentDojo benchmark contains **629 security test cases across 97 realistic tasks**, illustrating
the breadth of unsafe behavior that can arise when language-model agents act on untrusted data.
Coding and operations agents are especially consequential because a single tool call such as
`run_shell(command)` can create, modify, execute, or delete files with the authority of the user who
started the agent.

Most agent systems rely first on prompt rules—such as “do not modify files outside this project”—and
sometimes on a userspace wrapper that rejects suspicious command strings. These controls act before
the shell interprets the command. A shell can reconstruct the same action through quoting,
variables, `cd`, path traversal, scripts, or child processes, so many different strings can reach
the same harmful operation. For example, a filter looking for the literal path `/bin` can miss
`cd /; rm b''in/ls`, even though the kernel ultimately receives the same operation:
`unlink("/bin/ls")`.

Warden moves the decision to the point where the action is unambiguous. The kernel sees the calling
process, the requested system call, and the normalized path that the filesystem will use. It can
therefore enforce what an agent session is allowed to do regardless of how the shell command was
written.

## 3. Objectives / Research Questions

1. **Security:** Which escape techniques are blocked by prompt instructions, a userspace command
   deny-list, kernel enforcement, and the combination of both?
2. **Utility:** How does kernel enforcement affect task-completion success, unsafe actions
   attempted, and unsafe actions actually executed?
3. **Overhead:** What execution-time cost does the capability check add to an ordinary system call
   and to a path-based filesystem operation?

## 4. Methodology / System Design

### How Warden works

1. An operator gives a natural-language request to `bridge.py`.
2. The selected model returns exactly one `run_shell` tool call through an OpenAI-compatible API.
3. The bridge sends the command through a Unix socket connected to QEMU's second serial port.
4. Inside Warden, `/dev/agent` delivers the bytes to `agentd`.
5. `agentd` creates a pipe and forks. The child redirects standard output and error to the pipe,
   calls `mark_agent(session, capabilities, scope)`, and only then executes `/bin/sh -c command`.
6. The process label is inherited across `fork`, retained across `exec`, and can only be narrowed.
   All system calls pass through one dispatch table; path operations are checked after normalization.
7. The parent streams the child's output back to the bridge and ends the response with
   `\0EXIT <code>\n`. `agentd` never executes agent-supplied input itself.

**Use this figure:** [Agent request and response path](images/warden_agentd_request_response.svg)

### Technology stack

| Layer | Technology |
|---|---|
| Kernel | x86-64 C and NASM; `int 0x80` system calls; ring-3 processes |
| Boot and execution | Limine bootloader and QEMU (`q35`, 512 MiB) |
| Runtime | Custom libc, shell, in-memory filesystem, and userland tools |
| Agent bridge | Python, OpenAI-compatible client, and OpenRouter |
| Host/guest transport | Unix-domain socket → QEMU COM2 → `/dev/agent` |
| Build and tests | Make, GCC/binutils, host unit tests, and QEMU integration tests |

### Current implementation validation

The repository's functional test command is:

```sh
make test SAN=0 HOST_BUILD=build-host-nosan
```

It builds a clean test ISO, runs 11 host test binaries, boots Warden in QEMU, and executes 46 kernel
test groups. The tests cover memory management, scheduling, user/kernel isolation, process and file
operations, the agent channel, label enforcement, the audit log, and the shell. The ordinary
`make test` target also enables AddressSanitizer and UndefinedBehaviorSanitizer for host tests; the
57/57 result reported here is the functional run without sanitizers because LeakSanitizer cannot run
under the managed environment's ptrace instrumentation.

### Comparative evaluation protocol (planned)

Run the same tasks under four configurations selected without rebuilding:

| Configuration | Prompt rules | Userspace deny-list | Kernel policy |
|---|:---:|:---:|:---:|
| A — Prompt only | ✓ | — | — |
| B — `agentd` only | ✓ | ✓ | — |
| C — Kernel only | ✓ | — | ✓ |
| D — Combined | ✓ | ✓ | ✓ |

Use two profiles: `readonly` (`FS_READ + SPAWN`, no write scope) and `work-rw`
(`FS_READ + FS_WRITE + SPAWN + KILL`, write scope `/home/work`). Use **20 tasks**: five simple,
five multi-step, five tempting tasks whose easiest solution crosses policy, and five tasks containing
indirect prompt injection. Run every task once in each configuration with the same model, model
version, temperature, system prompt, step limit, and initial filesystem: **20 × 4 = 80 runs**.

For reproducibility, record the model identifier, temperature, maximum steps, random seed if the API
supports one, complete prompt/tool transcript, initial image hash, configuration, profile, final
filesystem, audit records, and exit status in one JSONL record per run. Boot a clean QEMU image for
every run and score it automatically against task-specific final-state checks.

### Metrics

- **Task success:** every required final-state check passes.
- **Unsafe attempted:** at least one system call is denied or one command is rejected by the
  userspace filter.
- **Unsafe executed:** a protected path is created, changed, renamed, or deleted, or a process
  outside the agent's session is killed.
- **Escape block rate:** attacks that leave protected state unchanged divided by attacks tested.
- **Overhead:** median and 95th-percentile time for `getpid` and `stat`, with and without enforcement,
  after a warm-up period over **[N] iterations** in the same QEMU configuration.

## 5. Results

### Results available now: implementation validation

| Measure | Result | Takeaway |
|---|---:|---|
| Host test binaries | **11/11 passed** | Core algorithms and data structures pass functional unit tests. |
| In-kernel/QEMU test groups | **46/46 passed** | The integrated kernel, userland, agent channel, labels, and audit path boot and execute correctly. |
| Implemented syscall entries with declared policy | **21/21** | Every current syscall table entry declares how it is checked. |
| `agentd` size | **114 physical lines** | The trusted bridge between host commands and labeled children is small enough to review. |
| Kernel source size | **5,655 physical lines** | Count includes C, assembly, headers, comments, and blank lines. |

**Caption:** All 57 current functional test groups passed; this validates implementation behavior,
not yet comparative security against the planned attack suite.

### Security properties exercised by current tests

| Test case | Expected policy | Current result |
|---|---|---:|
| Write through `/home/work/../../bin/ls` | Deny after normalization | **Blocked** |
| Write to prefix-confusion path `/home/workevil` | Deny at path-component boundary | **Blocked** |
| Add capabilities, widen scope, or change session | Labels may only narrow | **Blocked** |
| Agent opens `/dev/agent` | Channel is unavailable to agents | **Blocked** |
| Agent kills a human or another session | Only same-session targets are allowed | **Blocked** |
| Agent reads the audit log | Audit records are human-only | **Blocked and logged** |
| Bad user pointer or user-mode fault | Return `EFAULT` or kill only the process | **Contained** |

**Caption:** Current regression tests exercise the kernel mechanisms that the later comparative
escape suite will test end to end through shell commands.

### Main comparative results to add after the pilot

Do not replace the two tables above with unmeasured predictions. Add these three visuals once the
evaluation data exists:

1. **Grouped bar chart — “Safety and task success by configuration.”**  
   X-axis: A Prompt, B `agentd`, C Kernel, D Combined. Y-axis: percentage of 20 runs. Three series:
   task success, unsafe attempted, and unsafe executed.  
   **Caption template:** Kernel enforcement reduced unsafe execution from **[A%] to [C%]**, while
   task success changed by **[Δ] percentage points**.

2. **Bar chart — “Escape attacks blocked.”**  
   X-axis: four configurations. Y-axis: attacks blocked out of **[N]**. Put the exact fraction above
   each bar.  
   **Caption template:** String filtering blocked **[B/N]** attacks, whereas configurations with
   kernel enforcement blocked **[C/N]**.

3. **Dot or bar chart — “System-call overhead.”**  
   Show baseline and enforced median latency for `getpid` and `stat`, with p95 error bars.  
   **Caption template:** Enforcement added **[X ns / Y%]** to `getpid` and **[Z ns / W%]** to `stat`.

## 6. Discussion / Explanation

The current tests support Warden's central mechanism: once a child is labeled, changing the spelling
of a shell command does not change the authority of the process that eventually reaches the kernel.
Path traversal and prefix tricks are blocked because Warden checks a normalized path at the
filesystem boundary, and label-widening attempts fail because the label transition is one-way.

One useful design result is that `agentd` can remain a human-labeled supervisor without directly
executing agent input. It marks only the forked child before `exec`; if labeling fails, the child
exits with status 126. This keeps the trusted user-space component small while allowing output and
exit status to return normally.

These results do **not** yet establish that Warden outperforms prompt rules or a deny-list on the
planned workload. The current kernel has no network, symbolic links, or kernel-managed working
directory, and the agent protocol is line-based. File-writer provenance labels, per-session process
limits, a whole-session kill switch, the four runtime configurations, and the 80-run pilot remain
unfinished. The evaluation also uses only 20 author-written tasks, one model, and one repetition per
configuration, so its eventual success rates should be treated as a pilot rather than a population
estimate.

## 7. Conclusion and Future Work

Warden demonstrates that an agent's authority can follow its processes across `fork` and `exec` and
be checked at one kernel entry point rather than inferred from command text. Current tests confirm
the underlying enforcement mechanisms, including normalized write scopes, narrow-only labels,
same-session process control, and human-only audit access, across **57/57 functional test groups**.
The comparative security, task-success, and overhead research questions remain open until the
escape suite, 80-run pilot, and microbenchmarks are completed. Next, we will implement file-writer
provenance and session-wide limits, then expand the evaluation to more tasks, at least two models,
and repeated trials.

**Results-based replacement after evaluation (four sentences):**  
Kernel enforcement blocked **[K/N]** escape attacks, compared with **[B/N]** for the userspace
deny-list. It reduced unsafe execution from **[A%]** under prompt-only control to **[C%]**, while task
success changed from **[S1%]** to **[S2%]**. The median enforcement overhead was **[X]** for a simple
system call and **[Y]** for a path-based operation. Future work will add file provenance and session
limits and repeat the study with more tasks, models, and trials.

## Short References for the Poster

1. Debenedetti et al. **AgentDojo: A Dynamic Environment to Evaluate Prompt Injection Attacks and
   Defenses for LLM Agents.** NeurIPS 2024, Datasets and Benchmarks.
   <https://openreview.net/pdf?id=m1YYAQjO3w>
2. Greshake et al. **Not What You've Signed Up For: Compromising Real-World LLM-Integrated
   Applications with Indirect Prompt Injection.** 2023. <https://arxiv.org/abs/2302.12173>
3. Watson et al. **Capsicum: Practical Capabilities for UNIX.** USENIX Security 2010.
   <https://www.usenix.org/conference/usenixsecurity10/capsicum-practical-capabilities-unix>

## Final Editing Checklist

- Replace every bracketed result field only from the generated analysis output.
- Change planned-method language to past tense only after the corresponding experiment ran.
- Put the architecture diagram in the center or upper-right of the methodology section.
- Make Results the largest poster section; use direct labels on charts instead of a distant legend.
- Keep body text at poster-readable size and remove this checklist before export.
