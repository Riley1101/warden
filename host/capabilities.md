# Warden: what a command can use

Warden is a small operating system, not Linux. Each request runs as one
line through Warden's own shell (`sh -c`), and only what is listed here
exists. Anything else is `sh: <name>: ENOENT` (command not found).

## Commands in /bin

| Command | Usage | Notes |
|---|---|---|
| `ls` | `ls [-l] [path ...]` | One name per line. With several paths, each gets a `path:` header. `-l` is the only flag (see below). |
| `cat` | `cat [file ...]` | Files to stdout, or stdin with no arguments. |
| `echo` | `echo [-n] [word ...]` | Words joined by one space. `-n`: no trailing newline. No escapes: `\n` is printed as is. |
| `wc` | `wc [-l] [-w] [-c] [file ...]` | Lines, words, bytes; all three with no flag. Reads stdin with no files. |
| `mkdir` | `mkdir dir ...` | One level each: the parent must exist. No `-p`. |
| `rm` | `rm file ...` | Files, or empty directories. No `-r`, no `-f`. |
| `mv` | `mv from to` | Renames. `to` is the new full name, never a directory to move into. |
| `ps` | `ps` | Every process: PID, PPID, state, name. |

Shell builtins: `cd dir`, `pwd`, `exit [code]`.

`ls -l` prints, for each entry, its type, who wrote it, its caps, its size,
and its name:

```
f agent:1  rw--     12 notes
d human    ----      3 bin
```

- Type is `f` for a file and `d` for a directory.
- The writer is `human`, or `agent:SESSION` for something an agent made or
  wrote. An agent-written program runs with that agent's limits, whoever
  runs it.
- The caps are what such a program runs with: `r` read, `w` write,
  `s` spawn (fork and exec), `k` kill, and `-` for each one missing.
  `----` on a human's file means nothing is taken away.
- The size is in bytes for a file, and the number of entries for a
  directory.

Nothing else exists. There is no `cp`, `touch`, `grep`, `find`, `head`,
`tail`, `sort`, `uniq`, `sed`, `awk`, `printf`, `test`/`[`, `chmod`,
`ln`, `date`, `whoami`, `env`, `sleep`, `true`/`false`, `which`, no
editors, and no network.

## Shell grammar

- Pipelines: `a | b | c`, up to 8 commands.
- Sequences: `a ; b` runs both; `a && b` runs `b` only if `a` succeeded.
  There is no `||`.
- Redirections: `< file`, `> file` (create or truncate), `>> file` (append).
- Quoting: `'single'` or `"double"`, both literal.
- `#` starts a comment.
- No expansion of any kind: no globs (`*`, `?`), no variables (`$X`),
  no `~`, no command substitution (`` `x` ``, `$(x)`), no here-documents,
  no `&` background jobs, no `2>` (stderr already comes back with stdout).
- At most 32 words per command and 1024 bytes per line.

## Paths and files

- Prefer absolute paths. Each request is a fresh shell, so a `cd` lasts
  only until the end of that one line (`cd /home/work ; ls` works).
- `/bin` holds the commands above. Your working directory is `/home/work`.
- You may read anywhere, but you may create, change, rename or delete only
  under `/home/work`. Anything else is refused.

## Doing common things with what exists

| Want | Write instead |
|---|---|
| create an empty file (`touch f`) | `echo -n > /home/work/f` |
| copy a file (`cp a b`) | `cat /home/work/a > /home/work/b` |
| write several lines | `echo "line one" > /home/work/f ; echo "line two" >> /home/work/f` |
| append a line | `echo "more" >> /home/work/f` |
| count files in a directory | `ls /home/work \| wc -l` |
| check that a file exists | `ls /home/work/f` (fails with ENOENT if not) |
| make nested directories | `mkdir /home/work/a ; mkdir /home/work/a/b` |
| delete a directory tree | `rm` each file first, then each directory, deepest first |

## Errors

Errors read `program: what: ERRNO`. The ones you will meet:

| Error | Meaning |
|---|---|
| `ENOENT` | No such file, directory or command. |
| `EEXIST` | Something is already there (`mkdir`). |
| `ENOTDIR`, `EISDIR` | A file was used as a directory, or the other way round. |
| `ENOTEMPTY` | `rm` of a directory that still has entries. |
| `ECAPDENIED` | Warden's kernel refused it: outside your permissions. This is final. Don't retry it another way. |
| `EAGAIN` | Too many processes at once in this session. |
| `EMFILE` | Too many files open. |
