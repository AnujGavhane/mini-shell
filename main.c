#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "main.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { TOK_WORD, TOK_PIPE, TOK_AMP } TokenType;
typedef struct { TokenType type; char *word; } Token;
typedef struct { Token *items; size_t length, capacity; } Tokens;

/*
 * A job is one command line running in its own process group. A pipeline may
 * contain several child processes, so each child PID and its state is stored.
 */
typedef struct {
    int used;
    unsigned id;
    pid_t pgid;
    pid_t pids[MAX_PROCESSES];
    int states[MAX_PROCESSES]; /* 0 running, 1 stopped, 2 complete */
    int statuses[MAX_PROCESSES];
    size_t count;
    char *command;
} Job;

static Job jobs[MAX_JOBS];
static volatile sig_atomic_t child_changed;
static pid_t shell_pgid;
static int interactive;
static int last_status;
static char *shell_path;
static char *old_directory;
static unsigned next_job_id = 1;

/* Stop cleanly if the shell cannot allocate memory. */
static void oom(void) { perror("msh: allocation"); exit(1); }

/* SIGCHLD can interrupt input(), but the handler must not call waitpid().
 * It only sets a flag; the main loop reaps children at a safe time. */
void signal_handler(int sig_num) {
    if (sig_num == SIGCHLD) child_changed = 1;
}

/* Save an absolute path so `echo $SHELL` reports this msh executable. */
static char *resolve_shell_path(const char *arg0) {
    char resolved[PATH_MAX];
    if (realpath(arg0, resolved)) return strdup(resolved);
    if (strchr(arg0, '/')) return strdup(arg0);
    const char *path = getenv("PATH");
    if (!path) return strdup(arg0);
    char *copy = strdup(path);
    if (!copy) oom();
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (!*dir) dir = ".";
        if (snprintf(resolved, sizeof resolved, "%s/%s", dir, arg0) < (int)sizeof resolved &&
            access(resolved, X_OK) == 0) {
            char *answer = realpath(resolved, NULL);
            free(copy);
            return answer ? answer : strdup(resolved);
        }
    }
    free(copy);
    return strdup(arg0);
}

/*
 * Put an interactive shell in its own process group and give it the terminal.
 * The shell ignores keyboard job-control signals; children restore defaults.
 */
static void setup_shell(void) {
    interactive = isatty(STDIN_FILENO);
    shell_pgid = getpid();
    if (interactive) {
        while (tcgetpgrp(STDIN_FILENO) != (shell_pgid = getpgrp()))
            kill(-shell_pgid, SIGTTIN);
        if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EACCES && errno != EPERM) {
            perror("msh: setpgid");
            exit(1);
        }
        shell_pgid = getpgrp();
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_handler;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

/* Print the current prompt and read one whole line from standard input. */
void scan_input(char *prompt, char **input_string) {
    size_t capacity = 0;
    fputs(prompt, stdout);
    fflush(stdout);
    ssize_t n = getline(input_string, &capacity, stdin);
    if (n < 0) {
        int saved_errno = errno;
        free(*input_string);
        *input_string = NULL;
        errno = saved_errno;
    }
}

/* Return a trimmed copy of the line, or NULL when it contains only spaces. */
char *get_command(char *input_string) {
    if (!input_string) return NULL;
    while (isspace((unsigned char)*input_string)) input_string++;
    if (!*input_string) return NULL;
    char *end = input_string + strlen(input_string);
    while (end > input_string && isspace((unsigned char)end[-1])) --end;
    char *command = strndup(input_string, (size_t)(end - input_string));
    if (!command) oom();
    return command;
}

/* Tell the executor which commands need shell-side handling. */
int check_command_type(char *command) {
    if (!command || !*command) return NO_COMMAND;
    /* Keep this list in sync with the commands implemented in builtin(). */
    static const char *const names[] = {"echo", "cd", "pwd", "exit", "jobs", NULL};
    for (size_t i = 0; names[i]; ++i) if (strcmp(command, names[i]) == 0) return BUILTIN;
    return EXTERNAL;
}

/* Token arrays grow as needed, so command length is not fixed. */
static void tokens_push(Tokens *t, TokenType type, char *word) {
    if (t->length == t->capacity) {
        size_t n = t->capacity ? t->capacity * 2 : 16;
        Token *p = realloc(t->items, n * sizeof *p);
        if (!p) oom();
        t->items = p; t->capacity = n;
    }
    t->items[t->length++] = (Token){type, word};
}

static void append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 1 >= *cap) {
        size_t n = *cap ? *cap * 2 : 32;
        char *p = realloc(*buf, n);
        if (!p) oom();
        *buf = p; *cap = n;
    }
    (*buf)[(*len)++] = c;
}

static void append_text(char **buf, size_t *len, size_t *cap, const char *s) {
    while (*s) append_char(buf, len, cap, *s++);
}

/* Expand the shell variables required by this project and environment vars. */
static char *expand_variable(const char *name, size_t n) {
    if (n == 1 && name[0] == '?') {
        char value[32]; snprintf(value, sizeof value, "%d", last_status); return strdup(value);
    }
    if (n == 1 && name[0] == '$') {
        char value[32]; snprintf(value, sizeof value, "%ld", (long)getpid()); return strdup(value);
    }
    if (n == 5 && strncmp(name, "SHELL", n) == 0) return strdup(shell_path ? shell_path : "msh");
    char *key = strndup(name, n);
    if (!key) oom();
    const char *value = getenv(key);
    free(key);
    return strdup(value ? value : "");
}

/*
 * Split a line into words and the | / & operators. Quotes keep spaces inside
 * a word; single quotes suppress variable expansion. This is intentionally a
 * small shell parser, not a complete POSIX shell grammar.
 */
static int tokenize(const char *line, Tokens *out) {
    size_t i = 0, n = strlen(line);
    while (i < n) {
        while (i < n && isspace((unsigned char)line[i])) ++i;
        if (i == n) break;
        if (line[i] == '|' || line[i] == '&') {
            tokens_push(out, line[i++] == '|' ? TOK_PIPE : TOK_AMP, NULL);
            continue;
        }
        char *word = NULL; size_t len = 0, cap = 0;
        int quote = 0, started = 0;
        while (i < n) {
            char c = line[i];
            if (!quote && (isspace((unsigned char)c) || c == '|' || c == '&')) break;
            if (c == '\\' && quote != '\'') {
                started = 1;
                if (++i < n) append_char(&word, &len, &cap, line[i++]);
                else append_char(&word, &len, &cap, '\\');
                continue;
            }
            if (c == '\'' || c == '"') {
                if (!quote) { quote = c; started = 1; ++i; continue; }
                if (quote == c) { quote = 0; ++i; continue; }
            }
            if (c == '$' && quote != '\'') {
                size_t start = ++i, end;
                if (i < n && line[i] == '{') {
                    start = ++i;
                    while (i < n && line[i] != '}') ++i;
                    end = i;
                    if (i < n) ++i;
                } else if (i < n && (line[i] == '?' || line[i] == '$')) end = ++i;
                else {
                    while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
                    end = i;
                }
                if (end > start) {
                    char *value = expand_variable(line + start, end - start);
                    if (!value) oom();
                    append_text(&word, &len, &cap, value); free(value);
                } else append_char(&word, &len, &cap, '$');
                started = 1; continue;
            }
            append_char(&word, &len, &cap, c); started = 1; ++i;
        }
        if (quote) { fprintf(stderr, "msh: unmatched quote\n"); free(word); return -1; }
        if (started) {
            append_char(&word, &len, &cap, '\0');
            tokens_push(out, TOK_WORD, word);
        } else free(word);
    }
    return 0;
}

/* Free the temporary parser representation after command execution. */
static void free_tokens(Tokens *t) {
    for (size_t i = 0; i < t->length; ++i) free(t->items[i].word);
    free(t->items); memset(t, 0, sizeof *t);
}

/* --------------------------- Background jobs --------------------------- */
static Job *find_job_by_id(unsigned id) {
    for (size_t i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used && jobs[i].id == id) return &jobs[i];
    }
    return NULL;
}

static Job *new_job(pid_t pgid, pid_t *pids, size_t count, const char *command) {
    for (size_t i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used) continue;

        Job *job = &jobs[i];
        memset(job, 0, sizeof *job);
        job->used = 1;
        job->id = next_job_id++;
        job->pgid = pgid;
        job->count = count;
        job->command = strdup(command ? command : "");
        if (!job->command) oom();

        for (size_t child = 0; child < count; ++child) {
            job->pids[child] = pids[child];
        }
        return job;
    }
    fprintf(stderr, "msh: job table is full\n");
    return NULL;
}

static void remove_job(Job *job) {
    if (!job) return;
    free(job->command);
    memset(job, 0, sizeof *job);
}

/* Return 0 for running, 1 for stopped, or 2 when every process has exited. */
static int job_state(Job *job) {
    int all_done = 1, all_stopped = 1;
    for (size_t i = 0; i < job->count; ++i) {
        if (job->states[i] != 2) all_done = 0;
        if (job->states[i] == 0) all_stopped = 0;
    }
    if (all_done) return 2;
    if (all_stopped) return 1;
    return 0;
}

static int process_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status)) return 128 + WSTOPSIG(status);
    return 0;
}

/* Record one waitpid() result in the job that owns this child. */
static void update_process(pid_t pid, int status) {
    for (size_t i = 0; i < MAX_JOBS; ++i) {
        if (!jobs[i].used) continue;

        for (size_t child = 0; child < jobs[i].count; ++child) {
            if (jobs[i].pids[child] != pid) continue;

            jobs[i].statuses[child] = status;
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                jobs[i].states[child] = 2;
            } else if (WIFSTOPPED(status)) {
                jobs[i].states[child] = 1;
            } else if (WIFCONTINUED(status)) {
                jobs[i].states[child] = 0;
            }
            return;
        }
    }
}

/*
 * Collect children that finished or changed state without blocking. This is
 * called in the main loop, never from the signal handler.
 */
static void reap_children(void) {
    int status;
    pid_t pid;
    child_changed = 0;

    /* WNOHANG makes this safe to call while the prompt is ready for input. */
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        update_process(pid, status);
    }

    for (size_t i = 0; i < MAX_JOBS; ++i) {
        if (!jobs[i].used || job_state(&jobs[i]) != 2) continue;

        int result = process_status(jobs[i].statuses[jobs[i].count - 1]);
        fprintf(stderr, "[%u] done (exit status %d) %s\n", jobs[i].id, result, jobs[i].command);
        remove_job(&jobs[i]);
    }
}

/*
 * Run a command implemented by msh itself. State-changing built-ins such as
 * cd must run in the shell process; in a pipeline they run in a child copy.
 */
static int builtin(char **argv, int in_child) {
    if (!argv[0]) return 0;

    if (!strcmp(argv[0], "echo")) {
        int first_argument = 1;
        int print_newline = 1;
        if (argv[first_argument] && !strcmp(argv[first_argument], "-n")) {
            print_newline = 0;
            ++first_argument;
        }

        for (int i = first_argument; argv[i]; ++i) {
            if (i > first_argument) putchar(' ');
            fputs(argv[i], stdout);
        }
        if (print_newline) putchar('\n');
        return 0;
    }

    if (!strcmp(argv[0], "pwd")) {
        char *current_directory = getcwd(NULL, 0);
        if (!current_directory) {
            perror("pwd");
            return 1;
        }
        puts(current_directory);
        free(current_directory);
        return 0;
    }

    if (!strcmp(argv[0], "cd")) {
        int go_to_previous = argv[1] && !strcmp(argv[1], "-");
        const char *destination = go_to_previous ? old_directory :
                                  argv[1] ? argv[1] : getenv("HOME");

        if (!destination) {
            fprintf(stderr, "cd: HOME or OLDPWD is not set\n");
            return 1;
        }

        char *previous_directory = getcwd(NULL, 0);
        if (chdir(destination) < 0) {
            perror("cd");
            free(previous_directory);
            return 1;
        }

        /* Keep the directory variables current for later commands. */
        if (previous_directory) {
            free(old_directory);
            old_directory = previous_directory;
            setenv("OLDPWD", old_directory, 1);
        }
        char *current_directory = getcwd(NULL, 0);
        if (current_directory) {
            setenv("PWD", current_directory, 1);
            if (go_to_previous) puts(current_directory);
            free(current_directory);
        }
        return 0;
    }

    if (!strcmp(argv[0], "exit")) {
        int exit_status = last_status;
        if (argv[1]) {
            char *end;
            long value = strtol(argv[1], &end, 10);
            if (*end || value < 0 || value > 255) {
                fprintf(stderr, "exit: expected a number from 0 to 255\n");
                exit_status = 2;
            } else {
                exit_status = (int)value;
            }
        }

        if (in_child) _exit(exit_status);
        exit(exit_status);
    }

    if (!strcmp(argv[0], "jobs")) {
        for (size_t i = 0; i < MAX_JOBS; ++i) {
            if (!jobs[i].used) continue;
            const char *state = job_state(&jobs[i]) == 1 ? "Stopped" : "Running";
            printf("[%u] %s\t%s\n", jobs[i].id, state, jobs[i].command);
        }
        return 0;
    }

    return 1;
}

static Job *select_job(char **argv) {
    Job *chosen = NULL;
    if (argv[1]) {
        char *end;
        long value = strtol(argv[1], &end, 10);
        if (*end || value <= 0) {
            fprintf(stderr, "%s: invalid job number or process ID: %s\n", argv[0], argv[1]);
            return NULL;
        }

        chosen = find_job_by_id((unsigned)value);
        if (!chosen) {
            for (size_t i = 0; i < MAX_JOBS; ++i) {
                if (jobs[i].used && jobs[i].pgid == (pid_t)value) {
                    chosen = &jobs[i];
                    break;
                }
            }
        }
    } else {
        /* Job IDs increase, so the highest active ID is the newest job. */
        for (size_t i = 0; i < MAX_JOBS; ++i) {
            if (jobs[i].used && (!chosen || jobs[i].id > chosen->id)) chosen = &jobs[i];
        }
    }

    if (!chosen) fprintf(stderr, "%s: no such job\n", argv[0]);
    return chosen;
}

/* Give a job the terminal, wait for it, then return terminal control to msh. */
static void foreground_job(Job *job) {
    if (!job) return;

    /* Only the foreground process group may read from the terminal. */
    if (interactive) tcsetpgrp(STDIN_FILENO, job->pgid);
    if (job_state(job) == 1) kill(-job->pgid, SIGCONT);
    for (size_t i = 0; i < job->count; ++i) {
        if (job->states[i] == 1) job->states[i] = 0;
    }

    int status;
    while (job_state(job) == 0) {
        pid_t pid = waitpid(-job->pgid, &status, WUNTRACED | WCONTINUED);
        if (pid > 0) update_process(pid, status);
        else if (pid < 0 && errno != EINTR) break;
    }

    if (interactive) tcsetpgrp(STDIN_FILENO, shell_pgid);
    if (job_state(job) == 1) {
        fprintf(stderr, "[%u] Stopped %ld %s\n", job->id, (long)job->pgid, job->command);
    } else if (job_state(job) == 2) {
        last_status = process_status(job->statuses[job->count - 1]);
        remove_job(job);
    }
}

/*
 * Create one child per pipeline stage. Adjacent children share a pipe, and all
 * children share a process group so Ctrl-C / Ctrl-Z reach the entire pipeline.
 */
static int run_pipeline(char ***stages, size_t stage_count, int background, const char *source) {
    int previous_read_end = -1;
    int pipe_fds[2];
    pid_t child_pids[MAX_PROCESSES];
    pid_t process_group = 0;
    size_t children_started = 0;

    if (stage_count > MAX_PROCESSES) {
        fprintf(stderr, "msh: pipeline has too many commands\n");
        return 2;
    }

    for (size_t i = 0; i < stage_count; ++i) {
        int has_next_stage = i + 1 < stage_count;
        if (has_next_stage && pipe(pipe_fds) < 0) {
            perror("pipe");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (has_next_stage) {
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            }
            break;
        }

        if (pid == 0) {
            /* Put every pipeline child in the same process group. */
            pid_t child_group = process_group ? process_group : getpid();
            setpgid(0, child_group);

            /* The child should receive Ctrl-C and Ctrl-Z normally. */
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            /* Read from the previous pipe stage, if there is one. */
            if (previous_read_end >= 0) {
                dup2(previous_read_end, STDIN_FILENO);
                close(previous_read_end);
            }

            /* Send output to the next stage, if there is one. */
            if (has_next_stage) {
                close(pipe_fds[0]);
                dup2(pipe_fds[1], STDOUT_FILENO);
                close(pipe_fds[1]);
            }

            if (check_command_type(stages[i][0]) == BUILTIN) {
                _exit(builtin(stages[i], 1));
            }

            execvp(stages[i][0], stages[i]);
            fprintf(stderr, "msh: %s: %s\n", stages[i][0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        if (!process_group) process_group = pid;
        setpgid(pid, process_group);
        child_pids[children_started++] = pid;

        /* The parent keeps only the read end needed by the next stage. */
        if (previous_read_end >= 0) close(previous_read_end);
        if (has_next_stage) {
            close(pipe_fds[1]);
            previous_read_end = pipe_fds[0];
        } else {
            previous_read_end = -1;
        }
    }

    if (previous_read_end >= 0) close(previous_read_end);
    if (!children_started) return 1;

    Job *job = new_job(process_group, child_pids, children_started, source);
    if (!job) {
        for (size_t i = 0; i < children_started; ++i) kill(child_pids[i], SIGTERM);
        return 1;
    }

    if (background) {
        printf("[%u] %ld\n", job->id, (long)process_group);
        fflush(stdout);
        return 0;
    }

    foreground_job(job);
    return last_status;
}

/* Implement fg and bg after parsing an optional job number or process-group ID. */
static int handle_job_builtin(char **argv) {
    Job *job = select_job(argv);
    if (!job) return 1;
    if (!strcmp(argv[0], "bg")) {
        if (kill(-job->pgid, SIGCONT) < 0) { perror("bg"); return 1; }
        for (size_t i = 0; i < job->count; ++i) if (job->states[i] == 1) job->states[i] = 0;
        printf("[%u] %s &\n", job->id, job->command); return 0;
    }
    foreground_job(job);
    return last_status;
}

/* Parse a line, recognize PS1 assignments, then choose builtin or pipeline. */
static int process_line(char *line, char *prompt) {
    char *command = get_command(line);
    if (!command) {
        free(command);
        return last_status;
    }

    /* PS1 assignment is recognized only when the equals sign touches PS1. */
    if (!strncmp(command, "PS1=", 4)) {
        if (setenv("PS1", command + 4, 1) < 0) perror("PS1");
        else snprintf(prompt, 1024, "%s", command + 4);
        free(command);
        last_status = 0;
        return last_status;
    }

    Tokens tokens = {0};
    if (tokenize(command, &tokens) < 0) {
        free_tokens(&tokens);
        free(command);
        last_status = 2;
        return last_status;
    }
    if (!tokens.length) {
        free_tokens(&tokens);
        free(command);
        return last_status;
    }

    int background = tokens.items[tokens.length - 1].type == TOK_AMP;
    if (background) --tokens.length;

    /* Count pipeline stages, then count each stage's arguments. */
    size_t stages_n = 1;
    for (size_t i = 0; i < tokens.length; ++i) {
        if (tokens.items[i].type == TOK_PIPE) ++stages_n;
    }

    char ***stages = calloc(stages_n, sizeof *stages);
    size_t *counts = calloc(stages_n, sizeof *counts);
    if (!stages || !counts) oom();

    size_t stage = 0;
    for (size_t i = 0; i <= tokens.length; ++i) {
        int end_of_stage = i == tokens.length || tokens.items[i].type == TOK_PIPE;
        if (end_of_stage) {
            if (!counts[stage]) {
                fprintf(stderr, "msh: syntax error near pipe\n");
                last_status = 2;
                goto done;
            }
            stages[stage] = calloc(counts[stage] + 1, sizeof(char *));
            if (!stages[stage]) oom();
            ++stage;
        } else if (tokens.items[i].type == TOK_WORD) {
            ++counts[stage];
        } else {
            fprintf(stderr, "msh: unexpected '&'\n");
            last_status = 2;
            goto done;
        }
    }

    /* Point each stage's argv entries at the strings owned by tokens. */
    stage = 0;
    size_t argument = 0;
    for (size_t i = 0; i < tokens.length; ++i) {
        if (tokens.items[i].type == TOK_PIPE) {
            ++stage;
            argument = 0;
        } else {
            stages[stage][argument++] = tokens.items[i].word;
        }
    }

    if (stages_n == 1 && !background && (!strcmp(stages[0][0], "fg") || !strcmp(stages[0][0], "bg")))
        last_status = handle_job_builtin(stages[0]);
    else if (stages_n == 1 && !background && check_command_type(stages[0][0]) == BUILTIN) {
        last_status = builtin(stages[0], 0);
    } else {
        last_status = run_pipeline(stages, stages_n, background, command);
    }

done:
    for (size_t i = 0; i < stages_n; ++i) free(stages[i]);
    free(stages);
    free(counts);
    free_tokens(&tokens);
    free(command);
    return last_status;
}

/* Main loop: reap completed background jobs, prompt, read, and execute. */
int main(int argc, char **argv) {
    (void)argc;
    shell_path = resolve_shell_path(argv[0]);
    if (!shell_path) oom();
    setenv("SHELL", shell_path, 1);

    char prompt[1024];
    const char *initial = getenv("PS1");
    snprintf(prompt, sizeof prompt, "%s", initial && *initial ? initial : "msh> ");
    setup_shell();

    char *line = NULL;
    for (;;) {
        if (child_changed) reap_children();

        /* getline() is interrupted when a background child changes state. */
        errno = 0;
        scan_input(prompt, &line);
        if (!line && errno == EINTR) {
            clearerr(stdin);
            reap_children();
            continue;
        }
        if (!line) {
            if (interactive) putchar('\n');
            break;
        }

        process_line(line, prompt);
        free(line);
        line = NULL;
    }

    free(old_directory);
    free(shell_path);
    return last_status;
}
