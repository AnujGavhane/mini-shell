#ifndef MAIN_H
#define MAIN_H

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

/* Values returned by check_command_type(). */
#define BUILTIN 1
#define EXTERNAL 2
#define NO_COMMAND 3

/* Fixed limits keep the job table easy to inspect for this learning project. */
#define MAX_JOBS 128
#define MAX_PROCESSES 128

/* Input, parsing, command classification, and child-signal notification. */
void scan_input(char *prompt, char **input_string);
char *get_command(char *input_string);
int check_command_type(char *command);
void signal_handler(int sig_num);

#endif
