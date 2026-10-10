# MiniShell (msh)

MiniShell is a small Unix shell written in C. It reads a command, starts
programs, waits for foreground work, and returns to its prompt. The project is
intended to make a few important operating-system ideas visible in code:
process creation, signals, pipes, exit codes, and child-process cleanup.

This is a learning shell. Its parser handles ordinary words, single and double
quotes, a small set of `$` variables, pipelines, and a trailing `&`. It does
not implement every feature or every built-in found in Bash.

## Build and start

Build on Linux or another POSIX environment with a C compiler and `make`:

```sh
make
./msh
```

Remove the compiled program with `make clean`.

## Commands to try

```sh
pwd
cd /tmp
echo "The shell PID is $$"
echo "The shell executable is $SHELL"
false
echo "The previous status was $?"
ls | wc -l
sleep 20 &
jobs
```

The default prompt is `msh> `. Set another prompt by entering an assignment
with no spaces around `=`:

```sh
PS1=mini>
```

`PS1 = mini` is treated as a command, not a prompt assignment. The parser
supports a trailing `&` for background commands. Use Ctrl-C to interrupt a
foreground job and Ctrl-Z to stop it. `fg` resumes a job in the foreground;
`bg` resumes it in the background. Both commands use the most recent job when
no job number or process-group ID is supplied.

Implemented shell built-ins are `cd`, `pwd`, `echo`, `exit`, `jobs`, `fg`, and
`bg`. Other command names are searched for in `PATH` and run as external
programs.

## How the code is organized

The project keeps its implementation in one C file so the flow is easy to
follow, with constants and shared declarations in `main.h`.

### 1. The input loop

`main()` prepares the `SHELL` variable and prompt, then repeats these steps:

1. Reap any finished background children.
2. Print the prompt and read a line (`scan_input`).
3. Parse and execute that line (`process_line`).

An empty line is ignored. A line beginning with `PS1=` changes the prompt
inside msh; the `=` must immediately follow `PS1`.

### 2. Parsing a command

`tokenize()` turns the input into a list of words and operators. For example:

```text
cat notes.txt | wc -l &
```

becomes roughly `cat`, `notes.txt`, `|`, `wc`, `-l`, `&`. Quotes keep spaces in
one word. `$?`, `$$`, `$SHELL`, and environment variables are expanded while
words are read. `process_line()` groups the words around each `|` into pipeline
stages.

### 3. Built-ins and external programs

Some commands must change msh itself. For example, if `cd` ran only in a child
process, that child's directory would change while the shell stayed where it
was. `builtin()` therefore runs a standalone built-in in the shell process.
For a pipeline such as `pwd | wc -c`, the built-in runs in a child because its
output belongs in the pipe.

External commands use two system calls:

- `fork()` creates a child process.
- `execvp()` replaces that child with the requested program, searching `PATH`.

The parent shell uses `waitpid()` to wait for a foreground command and to
collect its exit status. A normal exit status is the program's return value; a
signal exit is represented as `128 + signal number`.

### 4. Pipelines

`run_pipeline()` creates a pipe between each pair of stages. A pipe has a read
end and a write end. The shell connects a stage's standard output to the next
stage's standard input with `dup2()`, then closes the extra file descriptors.
Each stage is a separate child process.

### 5. Signals and process groups

Interactive shells and their foreground jobs use different process groups.
The terminal sends Ctrl-C (SIGINT) or Ctrl-Z (SIGTSTP) to the foreground
process group. The shell gives the terminal to a foreground job while it runs,
then takes terminal control back after the job exits or stops. Background jobs
do not receive terminal input by default.

The shell ignores keyboard job-control signals so Ctrl-C does not kill msh.
Children restore the default signal behavior before running a command. The
SIGCHLD handler only sets a flag; `reap_children()` calls `waitpid()` later in
the main loop. This keeps non-signal-safe work out of the signal handler and
prevents completed background processes from remaining zombies.

### 6. Job table

Each job records its process-group ID, child PIDs, command text, and child
states (running, stopped, or complete). `jobs` displays the table. `fg` gives a
job the terminal and waits for it. `bg` sends SIGCONT and leaves the job in the
background. When every process in a background job finishes, msh prints its
exit status and removes the job entry.

## Files

- `main.c` — input loop, parser, built-ins, process launching, and job control.
- `main.h` — shared constants and function declarations.
- `Makefile` — build and clean targets.
- `README.md` — this guide.

## Current limits

This project is intentionally smaller than a production shell. It does not
support redirection (`>`, `<`), command separators (`;`), shell scripting,
globbing, command substitution, or the full Bash built-in list. Variable
expansion is deliberately limited and does not perform shell word splitting.
