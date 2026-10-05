## Roadmap 

[x] Repo skeleton

- Setup repository, minimal kernel file with `BOOT ok`.
- Test setup that support QEMU boot and unit tests on the host.
- Setup host unit tests with ASan UBsan and path normalization.


[ ] Kernel Core

- Physical memory management, virtual memory management, and paging.
- Slab allocator. 
- Timer and Scheduler, APIC timer interrupt, kernel threads, round-robin scheduling (for now). 
- Syscalls: and single checkpoint `dispatch()` table.
- Usercopy: copy_from_user, copy_to_user with exception table, so a bad pointer kills the
  process not the kernel itself.

[ ] User land

- ELF loader, fork, exec, wait, exit, using eager copy than copy-on-write.
- COM1 receiver, so read(0) reads serail console.
- pipes and dup2
- Minimal libc, string, ctype functions, snprintf, malloc over brk, syscallwrappers with errno

[ ] Writable fs and tools

- Writable filesystem, with a simple initramfs and a few tools (ls, cat, echo, etc).
- `getpid`, `kill`, `ps`
- Simple busybox utils, `ls, cat, echo, rm, mv, cp, mkdir, wc, grep, ps, kill`
- A terminal


[ ]  Agent channel

- COM2 receive and `/dev/agent`
- Support frame protocols `ACTION, RESULT, ERROR, APPROVAL`
- `host/bridge.py` QEMU socket to LLM API and JSONL logs
- `agend` in L1 mode `sh -c`

Basically, type `list files in /bin` on the host and the model's command run in warden.


[ ] Kernel enforcement (Finally, thesis starts!)

- Provenance label in each process (`origin`, `session`, `caps`); inherited, can't be remove 
 may be `SYS_mark_agent` to mark a process as an agent.
- `cap_check` inside `dispatch()` with component aware path scopes like  `/home/` should
  not match  milicious paths
- Audit ring buffer and `SYS_audit_read` syscall to read it from userland.
- Session limit and `SYS_session_limit` to set it, so a process can't spawn too many
  children.

[ ] agentd and evaluation

- L2 typed tools, three feedbacks levels, capability profiles
- agentd's own userspace policy, for agentd only configuration, 
- `host/harness.py` to run , collect audit logs, score
- Task suit (30 may be ) about 10 prompt-injection tasks
- Four configurations, prompt, agentd, kernel and combined

[ ] Further 

- Linux mapping: Landlock, seccomp, cgroups, overlayfs, and optionally a small BPF-LSM prototype
