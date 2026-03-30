# myshell

A Unix shell written in C++17 from scratch. Handles lexing, parsing (into an AST), pipelines, I/O redirection, background jobs, command history via readline, and has a `jail` mode that sandboxes untrusted programs using seccomp + Linux namespaces.

Built this to understand how shells actually work under the hood — signal handling, process groups, file descriptor plumbing, all of it.

## What it does

- Readline-based REPL with history (arrow keys, `history N`, etc.)
- Builtins: `cd`, `echo`, `pwd`, `exit`, `type`, `history`, `jobs`
- External commands resolved through `$PATH`, absolute/relative paths, tilde expansion
- Tokenizer + AST parser with proper single/double quote handling and backslash escapes
- Multi-stage pipelines (`cmd1 | cmd2 | cmd3`)
- Output/error redirection: `>`, `>>`, `1>`, `1>>`, `2>`, `2>>`
- Sequential commands with `;`
- Background execution with `&`, plus async `SIGCHLD` reaping
- `jail` prefix to run commands in a seccomp sandbox (network isolation, syscall whitelist, resource limits)

## Structure

```
include/
    executor.h, parser.h, security.h, utils.h
src/
    main.cpp        — REPL loop, SIGCHLD setup
    parser.cpp      — tokenizer, AST construction
    executor.cpp    — execution engine, pipelines, redirection
    security.cpp    — seccomp sandbox policy
    utils.cpp       — PATH resolution, cd, globals
test_security.cpp   — test harness for the sandbox
Makefile
```

`parser.cpp` turns raw input into tokens, `check()` builds an AST from those tokens, and the executor walks the tree. Builtins run in-process; external commands get `fork()`/`execv()`. Pipelines spin up N children connected with `pipe()`.

## Build

Needs g++ (C++17), libreadline-dev, and libseccomp-dev.

```bash
sudo apt install build-essential libreadline-dev libseccomp-dev
make
./myshell
```

## Sandbox (`jail`)

Prefix any command with `jail` to lock it down:

```
$ jail ./sketchy_binary
```

Show available jail options:

```
$ jail --help
```

What happens under the hood:
- `unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID [+ CLONE_NEWNET with --no-net])` — isolates user/mount/pid namespaces, and optionally network.
- `prctl(PR_SET_NO_NEW_PRIVS)` — can't escalate privileges.
- seccomp filter (default-kill policy) — only whitelisted syscalls go through. Everything else terminates the process immediately (`SCMP_ACT_KILL`). `openat` is only allowed read-only. `write` is restricted to fd 1 and 2.
- `setrlimit` — configurable CPU and memory limits (`--cpu`, `--mem`), max 3 child processes.
- All FDs above 2 are closed before `execv`.

### Interactive testing (recommended)

Run these directly inside `./myshell`:

```
$ jail --help

$ jail --cpu 5 --mem 256M echo hello

$ jail --mem bad /bin/echo should-fail
# expected: jail: invalid --mem value 'bad'

$ jail --cpu nope /bin/echo should-fail
# expected: jail: invalid --cpu value 'nope'

$ jail --wat /bin/echo should-fail
# expected: jail: unknown option '--wat'

$ jail --cpu 2 --mem 128M --no-net ping -c 1 1.1.1.1
# expected: network operation fails inside jail
```

### Scripted testing (optional)

If you want repeatable non-interactive tests, piping commands is fine too:

```bash
printf "jail --help\njail --cpu 5 --mem 256M echo hello\nexit\n" | ./myshell
```

There's a `test_security.cpp` you can compile separately to verify each constraint:

```bash
g++ -std=c++17 test_security.cpp -o test_security
./myshell
$ jail ./test_security net     # network blocked
$ jail ./test_security fork    # fork bomb capped
$ jail ./test_security mem     # allocation fails at limit
$ jail ./test_security cpu     # killed after ~2s
```

## Technical deep-dive

This section documents the non-obvious problems I ran into and the decisions behind the current implementation. Most of these are concurrency issues around signal handling and process management.

### The fork/SIGCHLD race

This was the nastiest bug. The scenario:

1. Shell calls `fork()`, gets back a PID.
2. Child exits immediately (like `true &` — runs in microseconds).
3. Kernel delivers `SIGCHLD` before `execute_pipeline()` even gets to `jobs.push_back()`.
4. Signal handler reaps the child via `waitpid()`.
5. Shell then adds the (already dead) PID to the jobs list as "Running."

Now you've got a ghost job that shows up in `jobs` forever because nobody will ever reap it again.

The fix: block `SIGCHLD` before forking, do all the bookkeeping, then unblock:

```cpp
sigprocmask(SIG_BLOCK, &mask, &oldmask);  // hold signals
pid_t pid = fork();
// ... push to jobs ...
sigprocmask(SIG_SETMASK, &oldmask, nullptr);  // release
```

Simple in hindsight, but this class of bug only shows up with fast-exiting background processes — so it's easy to miss during casual testing.

### Why `waitpid(-1)` instead of per-PID polling

The first version iterated through the entire `jobs` vector each prompt and called `waitpid()` on every PID individually. That's N syscalls per Enter press regardless of whether anything died. With 50 background jobs and 1 dead child, you're making 50 kernel calls for no reason.

Current approach:

```cpp
while ((reaped_pid = waitpid(-1, &status, WNOHANG)) > 0) {
    // look up reaped_pid in jobs, remove it
}
```

This asks the kernel "give me anyone who's dead" — it returns K+1 times (K dead children + 1 to say "nobody left"). Way fewer syscalls.

The other reason this matters: **Unix signals are lossy.** If three children die at the same time, the kernel might coalesce them into a single `SIGCHLD`. If you only reap once per signal, you leave zombies. The `while` loop drains the entire queue.

### `ECHILD` handling

`waitpid()` can return -1 with `errno == ECHILD`, which means "you have no children left to wait for." This happens legitimately when the signal handler already reaped a child before the main loop got to it. Rather than treating it as an error, the code takes it to mean "this job is done" — otherwise you'd accumulate stale entries.

### `wait(NULL)` is a trap

An earlier version used `wait(NULL)` to collect foreground pipeline children. Problem: `wait()` picks up *any* dead child. If a background job dies while you're waiting on a foreground pipeline, `wait()` grabs it, and the background job's entry in `jobs` never gets cleaned up.

Fixed by waiting on specific PIDs:

```cpp
for (int i = 0; i < n; i++) {
    waitpid(children_pids[i], &status, 0);
}
```

### Signal handler constraints

`SIGCHLD` can arrive literally anywhere — including inside `malloc()`. If the handler also calls `malloc()` (or `printf`, or `cout`, which call `malloc`), you deadlock on the heap lock. So the handler only uses async-signal-safe functions:

```cpp
void sigchld_handler(int sig) {
    int saved_errno = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0)
        child_changed = 1;
    errno = saved_errno;
}
```

`child_changed` is `volatile sig_atomic_t` — the only type that's safe to share between a signal handler and normal code without synchronization.

`errno` gets saved/restored because `waitpid()` can modify it, and if the main thread was halfway through a syscall that also checks `errno`, you'd corrupt its error state.

### Deferred job notifications

Background job completion ("Done") is printed at the top of the REPL loop, not inside the signal handler. This is intentional: if you print from the handler, you'll corrupt whatever the user is currently typing into readline. The trade-off is that you only see the notification after hitting Enter.

The "real" fix would be to call `rl_redisplay()` from the handler to refresh the prompt, but that introduces a lot of complexity around making readline cooperate with async output. Not worth it for this project.

### `dup2` over `dup3`

I went with `dup2()` for redirection. `dup3()` is nicer on Linux because it sets `O_CLOEXEC` atomically (no race window between dup and fcntl in multithreaded code), but it's Linux-only. Since the shell is single-threaded, the race doesn't apply here. For extra safety, all FDs above 2 are force-closed before `execv()` anyway.

For a production/multithreaded codebase, you'd want:
```cpp
#ifdef __linux__
    dup3(oldfd, newfd, O_CLOEXEC);
#else
    dup2(oldfd, newfd);
    fcntl(newfd, F_SETFD, FD_CLOEXEC);
#endif
```

POSIX portability won out here.

### Vector erasure during iteration

Removing from `std::vector` while iterating — the classic C++ footgun. `erase()` invalidates the iterator. The safe pattern is `it = jobs.erase(it)` which returns the next valid position.

Worth noting: `vector::erase` from the middle is O(N) due to element shifting. If the jobs list got large, switching to `std::list` or `std::unordered_map<pid_t, Job>` would give O(1) removal at the cost of cache locality. Not a problem at shell scale though.

### Pipeline PID tracking

For a background pipeline like `ls | grep foo | sort &`, only the last PID (`sort`) is stored in the jobs list. This matches how bash does it — pipeline status is defined by the final stage. The job is "Done" when the last command exits.

### Terminal state recovery

After a foreground process exits, the shell runs:

```cpp
tcsetpgrp(STDIN_FILENO, getpgrp());
tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_tmodes);
```

`shell_tmodes` is captured at startup. If a child process messes with terminal settings (raw mode, disabling echo, etc.) and then crashes without restoring them, the shell would inherit a broken terminal. This restores sanity.

---
