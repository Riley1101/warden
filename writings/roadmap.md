## Roadmap

Four weeks. If something slips, protect in this order: escape suite > agent demo > pilot results.

## Week One (Foundations, minimum to run `sh -c`)

- [x] Setup repository, minimal kernel file with `BOOT ok`.
- [x] Test setup that support QEMU boot and unit tests on the host.
- [x] Setup host unit tests with ASan UBSan and path normalization.
- [x] Physical memory management, virtual memory management, and paging.
- [x] Slab allocator.
- [x] GDT, IDT
- [x] APIC timer and round-robin scheduler.
- [x] User mode: TSS, ring 3 entry, per-process address spaces.
- [x] Syscalls: single checkpoint `dispatch()` table. Every entry declares a capability;
      `dispatch` refuses entries without one.
- [x] Usercopy: copy_from_user, copy_to_user with exception table. Any ring 3 fault kills the process, not the kernel.
- [x] ELF loader, fork (eager copy), exec, wait, exit, getpid, kill.
      (Programs load from the filesystem.)
- [x] pipes and dup2
- [x] Minimal libc: string functions, snprintf, malloc over brk, syscall wrappers with errno
- [x] Writable in-memory fs loaded from initramfs: create, write, `unlink`, `rename`, `mkdir`, `stat`.
- [ ] Tools: `ls, cat, echo, rm, mv, mkdir, wc, ps`
- [ ] `sh` with `sh -c "..."` and simple pipelines

**Done when:** `sh -c "ls /bin | wc -l"` works, and a bad pointer or fault in ring 3 kills only that process.


## Week Two (Agent channel, labels, caps, audit)

- [ ] COM2 receive with an interrupt, a ring buffer, and a blocking `/dev/agent`
- [ ] Framed protocol `[u32 len][u8 type][payload]` with `ACTION, RESULT, ERROR`
- [ ] `host/bridge.py` QEMU socket to LLM API, one JSONL log line per step
- [ ] `agentd` in L1 mode: read an action, run `sh -c`, return stdout and exit code
- [ ] QEMU: `-serial stdio -serial unix:/tmp/warden-agent.sock,server,nowait`. Debug output only on COM1.
- [ ] Provenance label in each process (`origin`, `session`, `capset`); inherited by `fork`, kept by `exec`
- [ ] `SYS_mark_agent(session, caps)`: one-way, can only narrow; agentd marks the child before running the command
- [ ] `cap.c`: `cap_check()` and component-aware `in_scope()` with host tests (`/home` vs `/homeevil`, `..`)
- [ ] `cap_check` inside `dispatch()`, denied calls return `-ECAPDENIED`
- [ ] `audit.c` ring buffer and `SYS_audit_read` (human-only), host test for wraparound

**Done when:** the model's command runs in warden from a host prompt (record it), and an agent
scoped to `/home/work` can't delete `/bin/ls` through `sh -c`, with the denial in the audit log.


## Week Three (Escape suite and agentd configurations)

- [ ] Session process-count limit, set through `mark_agent`
- [ ] `SYS_kill_session` kill switch
- [ ] File labels: `writer_origin`/`writer_session`; `exec` of an agent-written file gets the agent label
- [ ] Escape suite in `make test`, every one blocked: path traversal, prefix confusion, label clearing,
      shell escape, fork bomb, executing an agent-written script, bad pointers, reading the audit log, kill switch
- [ ] agentd userspace policy: string-matching deny-lists of commands and paths
- [ ] Two capability profiles in a config file: `readonly`, `work-rw`
- [ ] Four configurations A to D (prompt only, agentd only, kernel only, combined), chosen by one flag
- [ ] Feedback: errno name plus the audit record attached to RESULT

**Done when:** every escape test is blocked in `make test`, and one task runs under all four configurations without rebuilding.


## Week Four (Harness and pilot evaluation)

- [ ] `host/harness.py`: boot clean, create setup files, run the task, collect audit log and final fs,
      score, append to `results.jsonl`
- [ ] Task file format (YAML): prompt, setup, protected paths, profile, success checks
- [ ] 20 tasks: 5 simple, 5 multi-step, 5 tempting, 5 prompt-injection
- [ ] Pilot: 20 tasks x 4 configurations x 1 cheap model = 80 runs (low temperature, log for replay)
- [ ] `analyze.py`: success and unsafe-executed rates per configuration

**Done when:** one command produces the first results table.


## Further (after the four weeks)

- [ ] Memory limits, more profiles, COM3 audit mirror
- [ ] L2 typed tools (`list_dir`, `read_file`, `write_file`, `delete`, `move`, `spawn`, `ps`)
- [ ] Feedback levels 0/1/2 as an experimental variable
- [ ] Expand to ~100 tasks, two models, three repetitions; ablations (file labels, limits)
- [ ] Optional extensions: approval blocking (`APPROVAL` frame), untrusted-input taint
- [ ] Linux mapping: Landlock, seccomp, cgroups, overlayfs, and optionally a small BPF-LSM prototype
- [ ] Writing
