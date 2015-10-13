/*
 * Project: TJ Shell, a small Linux shell
 * File: tj_shell.c
 * Author: Tobias Johansson
 * Version: 1.0, 18 May 2015
 *
 * Compilation: gcc -pedantic -ansi -Wall -Werror -O4 -D SIGDET=1 tj_shell.c
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* For detection of terminated background processes by signals sent from the
   child processes compile with SIGDET=1. If SIGDET is undefined or equals zero
   then termination will be detected by polling. */
#if SIGDET != 1
#define POLLING
#endif
#define STR_LEN		(1023)
#define MAX_CMDS	(63)
#define MAX_ARGS	(63) /* per command */
#define PIPING		(no_cmds > 1)
#define FIRST_CMD	(cmd == 1)
#define MIDDLE_CMD	(cmd > 1 && cmd < no_cmds)
#define LAST_CMD	(cmd == no_cmds)
#define READ_END	(0)
#define WRITE_END	(1)

/*------------------------------------------------------------------------------
 * PROTOTYPES
 */

/* Called from main */
void init();
void prompt();
/* Excecute commands */
int exec_cmdline();
int exec_cmd();
int fork_exec_wait();
void c_init();
int c_wait();
/* Built in commands */
void change_dir();
void check_env();
void term_all();
void put_fg();
/* Helper functions */
int child_process();
void get_env_cmd();
void malloc_strcpy();
void print_status();
void stdout_to_pipe();
void pipe_to_stdin();
void ten_ms_sleep();
void tokenize();

/*------------------------------------------------------------------------------
 * GLOBAL VARIABLES
 */

pid_t shell_pid;

/*------------------------------------------------------------------------------
 * SIGNAL HANDLERS
 */

/*
 * Handles SIGCHLD signals or polling if SIGDET!=1.
 */
void sigchld_handler() {
	int status;
	pid_t c_pid;

	/* WUNTRACED: also return if a child has stopped
	   WNOHANG: return immediately if no child has exited */
	while ((c_pid = waitpid(WAIT_ANY, &status, WUNTRACED | WNOHANG)) > 0) {
		print_status(c_pid, status);
	}
}

/*
 * Handles SIGINT signals, that is usually Ctrl+C.
 */
void sigint_handler() {
	fprintf(stdout, "\n[Ctrl+C]\n");
	term_all();
}

/*
 * Handles SIGTSTP signals, that is usually Ctrl+Z.
 */
void sigtstp_handler() {
	fprintf(stdout, "\n[Ctrl+Z]\n");
	if (kill(shell_pid, SIGSTOP) == -1) perror("kill");
}

/*------------------------------------------------------------------------------
 * MAIN
 */

/*
 * Drives the program.
 */
int main(int argc, char **argv) {
	init(argc);
	#ifdef POLLING
	fprintf(stdout, "\nWelcome to TJ Shell! (POLLING) \n\n");
	#else
	fprintf(stdout, "\nWelcome to TJ Shell! (SIGDET=1) \n\n");
	#endif
	while (1) {
		prompt();
		ten_ms_sleep(10);
		#ifdef POLLING
		sigchld_handler();
		#endif
	}
}

/*------------------------------------------------------------------------------
 * CALLED FROM MAIN
 */

/*
 * Init.
 */
void init(int argc) {
	if (argc > 1) {
		fprintf(stderr, "init: TJ Shell does not take arguments\n");
		exit(EXIT_FAILURE);
	}
	/* If not already:
	   Set PGID=PID, this makes the shell process group leader.
	   Take control of the terminal. */
	shell_pid = getpid();
	if (setpgid(shell_pid, shell_pid) == -1) {
		fprintf(stderr, "init: Could not set the shell process group leader\n");
		exit(EXIT_FAILURE);
	}
	signal(SIGINT, sigint_handler);   /* Ctrl+C: terminal interrupt signal */
	signal(SIGQUIT, SIG_DFL);         /* Ctrl+4: terminal quit signal */
	signal(SIGTSTP, sigtstp_handler); /* Ctrl+Z: terminal stop signal */
	signal(SIGTTIN, SIG_IGN);         /* background process attempting read */
	signal(SIGTTOU, SIG_IGN);         /* background process attempting write */
	#ifdef POLLING
	signal(SIGCHLD, SIG_DFL);         /* child process terminated, stopped */
	#else
	signal(SIGCHLD, sigchld_handler);
	#endif
}

/*
 * Prompt user, get command line and excecute commands.
 */
void prompt(void) {
	char c, cwd[STR_LEN+1], line[STR_LEN+1];
	int i;
	if (shell_pid != getpid()) {
		fprintf(stderr, "prompt: Permission for child denied\n");
		_exit(EXIT_FAILURE);
	}
	getcwd(cwd, sizeof(cwd));
	fprintf(stdout, "%s> ", cwd);
	for (i = 0; (c = fgetc(stdin)) != '\n'; i++) {
		line[i] = c;
	}
	line[i] = '\0'; /* null-terminate string */
	if (strlen(line) > 0) exec_cmdline(line);
}

/*------------------------------------------------------------------------------
 * EXECUTE COMMANDS
 */

/*
 * Takes an unformatted command line and excecutes it. The function tokenize the
 * line for piping and by arguments. If any foreground child fail return a
 * number representing the command that failed there 1 represents the first, 2
 * the second etc., otherwize return 0.
 */
int exec_cmdline(char const *line) {
	/* Allocates for arrays of MAX_X strings, last should point to NULL */
	char **cmds = malloc((MAX_CMDS+1) * sizeof(char *));
	char **args = malloc((MAX_ARGS+1) * sizeof(char *));
	char s[STR_LEN+1];
	int failed_cmd = 0, i, j, no_cmds, no_args;
	/* Get commands from the command line */
	tokenize(cmds, &no_cmds, line, "|");
	/* Execute commands one bye one */
	for (i = 0; i < no_cmds; i++) {
		tokenize(args, &no_args, cmds[i], " ");
		if (no_args == 0) {
			failed_cmd = i + 1;
			fprintf(stderr, "exec_cmdline: Empty command\n");
			break;
		}
		if (exec_cmd(args, no_args, i + 1, no_cmds) == -1) {
			failed_cmd = i + 1;
			sprintf(s, "%s", args[0]);
			for (j = 1; j < no_args; j++) sprintf(s, "%s %s", s, args[j]);
			fprintf(stderr, "exec_cmdline: Command '%s' failed\n", s);
			break;
		}
	}
	free(cmds);
	free(args);
	return failed_cmd;
}

/*
 * Takes a tokenized command, check for built in command and decides execution.
 * If foreground execution failed return -1, else 0.
 *
 * dtype const **var ⇒ var mutable, *var mutable, **var const
 * (var is a: pointer to pointer to const-dtype)
 */
int exec_cmd(char const **args, int no_args, int cmd, int no_cmds) {
	int background = 0;
	if (no_cmds == 1) {
		if (strcmp(args[0], "cd") == 0) {
			if (no_args == 1) {change_dir("~"); return 0;}
			if (no_args == 2) {change_dir(args[1]); return 0;}
			return -1;
		}
		if (strcmp(args[0], "checkEnv") == 0) {
			if (no_args <= 2) {check_env(args); return 0;}
			return -1;
		}
		if (strcmp(args[0], "exit") == 0) {
			if (no_args == 1) {term_all(); return 0;}
			return -1;
		}
		if (strcmp(args[0], "fg") == 0) {
			if (no_args == 2) {put_fg(args[1]); return 0;}
			return -1;
		}
		/* If background mode */
		if (strcmp(args[no_args-1], "&") == 0) {
			if (no_args >= 2) {
				args[no_args-1] = NULL; /* replace "&" with NULL */
				background = 1;
			} else {
				return -1;
			}
		}
	}
	/* All other commands: Fork-Exec-Wait (piping in background not allowed) */
	return fork_exec_wait(args, cmd, no_cmds, background);
}

/*
 * Forks the process, the child executes a command and the parent waits the
 * child if background is set to 0. Piping can be made for following commands
 * there cmd is 1 for the first command and set 2 for second, etc. The variable
 * no_cmds should be set to number of commands for piping or 1 for a single
 * command. If foreground execution failed return -1, else 0.
 *
 * dtype *const *var ⇒ var mutable, *var const, **var mutable
 * (var is a: pointer to const-pointer to dtype)
 */
int fork_exec_wait(char *const *args, int cmd, int no_cmds, int background) {
	static int **pipe_fds, n;
	struct timeval t0;
	int i, return_value, status = 0;
	pid_t c_pid;
	if (PIPING && FIRST_CMD) {
		/* Allocates for no_cmds-1 pipes */
		pipe_fds = malloc((no_cmds - 1) * sizeof(int *));
		for (i = 0; i < (no_cmds - 1); i++) {
			/* Allocates for two file descriptors, read and write end */
			pipe_fds[i] = malloc(2 * sizeof(int));
			/* Retrive file descriptors */
			if (pipe(pipe_fds[i]) == -1) {
				fprintf(stderr, "fork_exec_wait: Could not pipe\n");
				exit(EXIT_FAILURE);
			}
		}
		n = 0; /* pipe count */
	}
	if (PIPING && MIDDLE_CMD) n++;
	gettimeofday(&t0, NULL); /* start stopwatch */
	/* Fork */
	if ((c_pid = fork()) == -1) {
		fprintf(stderr, "fork_exec_wait: Could not fork\n");
		exit(EXIT_FAILURE);
	}
	/* Exec (child) */
	else if (c_pid == 0){
		c_init(!background);
		if (PIPING) {
			if (FIRST_CMD) {
				stdout_to_pipe(pipe_fds[n]);
			}
			if (MIDDLE_CMD) {
				pipe_to_stdin(pipe_fds[n-1]); /* from prev */
				stdout_to_pipe(pipe_fds[n]);  /* to   next */
			}
			if (LAST_CMD) {
				pipe_to_stdin(pipe_fds[n]);
			}
		}
		/* The arrary position after the last argument must be set to NULL */
		if (execvp(args[0], args) == -1) {
			_exit(EXIT_FAILURE);
		}
	}
	/* Wait (parent) */
	else if (c_pid > 0) {
		if (PIPING) close(pipe_fds[n][WRITE_END]); /* widowing pipe */
		if (!background && LAST_CMD) {
			fprintf(stdout, "[%d] Spawned in foreground\n", c_pid);
			status = c_wait(c_pid, &t0, 0);
		} else {
			fprintf(stdout, "[%d] Spawned in background\n", c_pid);
		}
	}
	if (!background && WEXITSTATUS(status) == EXIT_FAILURE) {
		return_value = -1;
	} else {
		return_value = 0;
	}
	if (PIPING && (LAST_CMD || (return_value == -1))) free(pipe_fds);
	return return_value;
}

/*
 * Init child.
 */
void c_init(int foreground) {
	/* Make the child leader of a new process group */
	pid_t c_pid = getpid();
	setpgid(c_pid, c_pid);
	if (foreground) {
		/* The child takes the terminal */
		if (tcsetpgrp(STDIN_FILENO, getpgid(c_pid)) == -1) perror("tcsetpgrp");
	}
	/* Signal handling to default */
	signal(SIGINT , SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTSTP, SIG_DFL);
	signal(SIGTTIN, SIG_DFL);
	signal(SIGTTOU, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
}

/*
 * Waits for a child in the foreground. If process is in background set cont=1
 * for give over the terminal and continue the process, otherwise set 0. Returns
 * status from waitpid.
 */
int c_wait(pid_t c_pid, struct timeval const *t0, int cont) {
	int status;
	struct timeval t1, diff;
	if (cont) {
		if (tcsetpgrp(STDIN_FILENO, getpgid(c_pid)) == -1) perror("tcsetpgrp");
		if (kill(c_pid, SIGCONT) == -1) perror("kill");
	}
	/* Wait for childs death, WUNTRACED: also return if a child has stopped */
	if (waitpid(c_pid, &status, WUNTRACED) > 0) {
		print_status(c_pid, status);
		if (t0 != NULL) {
			gettimeofday(&t1, NULL); /* stop stopwatch */
			diff.tv_sec = t1.tv_sec - t0->tv_sec;
			diff.tv_usec = t1.tv_usec - t0->tv_usec;
			fprintf(stdout, "Run time was %.0f ms\n",
				diff.tv_sec * 1000.0 + diff.tv_usec / 1000.0);
		}
	}
	/* Reclaim the terminal */
	if (tcsetpgrp(STDIN_FILENO, getpgid(shell_pid)) == -1) perror("tcsetpgrp");
	return status;
}

/*------------------------------------------------------------------------------
 * BUILT IN COMMANDS
 */

/*
 * Change directory.
 */
void change_dir(char const *path) {
	int i, path_len = strlen(path);
	char exec_path[STR_LEN+1], s[STR_LEN+1];
	if (path[0] == '~') {
		/* Replace tilde by HOME environment */
		for (i = 1; i < path_len + 1; i++) s[i-1] = path[i];
			sprintf(exec_path, "%s%s", getenv("HOME"), s);
	} else {
		sprintf(exec_path, "%s", path);
	}
	if (chdir(exec_path) == -1) {
		fprintf(stderr, "change_dir: No such directory\n");
	}
}

/* A built-in command "checkEnv" which executes "printenv | sort | pager" if no
 * arguments are given to the command. If arguments are passed to the command
 * then "printenv | grep <arguments> | sort | pager" executes. The pager
 * executed is selected primarily based on the value of the users "PAGER"
 * environment variable. If no such variable is set then first try to execute
 * "less" and if that fails "more".
 *
 * dtype const *const *var ⇒ var mutable, *var const, **var const
 * (var is a: pointer to const-pointer to const-dtype)
 */
void check_env(char const *const *args) {
	char line[STR_LEN+1], pager[STR_LEN+1];
	int no_cmds = (args[1] == NULL ? 3 : 4);
	if (getenv("PAGER") != NULL) {
		sprintf(pager, "%s", getenv("PAGER"));
	} else {
		sprintf(pager, "less");
	}
	get_env_cmd(line, args, pager);
	fprintf(stdout, "Actual command line: %s\n", line);
	if (exec_cmdline(line) == no_cmds && strcmp(pager, "less") == 0) {
		/* If less failed */
		get_env_cmd(line, args, "more");
		fprintf(stdout, "Actual command line: %s\n", line);
		exec_cmdline(line);
	}
}

/*
 * Terminates all children in an orderly manner.
 */
void term_all(void) {
	char path[STR_LEN+1];
	DIR *dirp;
	FILE *fp;
	pid_t pid, ppid;
	struct dirent* dent;
	fprintf(stdout, "\nTJ Shell closing...\n\n");
	if ((dirp = opendir("/proc")) == NULL) {
		fprintf(stderr, "term_all: Could not open '/proc'\n");
		exit(EXIT_FAILURE);
	}
	/* Searches through all directories in /proc */
	while((dent = readdir(dirp)) != NULL) {
		/* If numerical */
		if (dent->d_name[0] >= '0' && dent->d_name[0] <= '9') {
			/* Take data from /proc/[pid]/stat, see URL below for more info.
			   http://man7.org/linux/man-pages/man5/proc.5.html */
			sprintf(path, "/proc/%s/stat", dent->d_name);
			fp = fopen(path,"r");
			fscanf(fp, "%d %*s %*c %d", &pid, &ppid);
			fclose(fp);
			/* Kill if shell is parent to process */
			if (shell_pid == ppid) if (kill(pid, SIGKILL) == -1) perror("kill");
		}
	}
	closedir(dirp);
	ten_ms_sleep(10); /* wait for SIGKILL signals to terminate bg processes */
	#ifdef POLLING
	sigchld_handler();
	#endif
	exit(EXIT_SUCCESS);
}

/*
 * Put background process in the foreground.
 */
void put_fg(char const *s) {
	pid_t c_pid;
	sscanf(s, "%d", &c_pid);
	if (child_process(c_pid)) {
		c_wait(c_pid, NULL, 1);
	} else {
		fprintf(stderr, "put_fg: No such child\n");
	}
}

/*------------------------------------------------------------------------------
 * HELPER FUNCTIONS
 */

/*
 * Returns 1 if given pid is a child process, else 0.
 */
int child_process(pid_t c_pid) {
	char path[STR_LEN+1];
	FILE *fp;
	pid_t ppid;
	sprintf(path, "/proc/%d/stat", c_pid);
	if ((fp = fopen(path,"r")) == NULL) return 0;
	fscanf(fp, "%*d %*s %*c %d", &ppid);
	fclose(fp);
	if (ppid == shell_pid) {
		return 1;
	} else {
		return 0;
	}
}

/*
 * Get command line for check of enviroment.
 *
 * dtype const *const *var ⇒ var mutable, var* const, var** const
 * (var is a: pointer to const-pointer to const-dtype)
 */
void get_env_cmd(char *line, char const *const *args, char const *pager) {
	if (args[1] == NULL) {
		sprintf(line, "printenv | sort | %s", pager);
	} else {
		sprintf(line, "printenv | sort | grep %s | %s", args[1], pager);
	}
}

/*
 * Takes dest a pointer to a string and src the source string. The function
 * allocates memory for dest and copies src to it.
 */
void malloc_strcpy(char **dest, char const *src) {
	if (src == NULL) {*dest = NULL; return;}
	*dest = malloc((strlen(src)+1) * sizeof(char)); /* +1 for null-terminated */
	sprintf(*dest, "%s", src);
}

/*
 * Print childs wait status.
 */
void print_status(pid_t c_pid, int status) {
	if (WIFEXITED(status)) {
		fprintf(stdout, "[%d] Terminated normally\n", c_pid);
	} else if (WIFSIGNALED(status)) {
		fprintf(stdout, "[%d] Terminated by a signal\n", c_pid);
	} else if (WIFSTOPPED(status)) {
		fprintf(stdout, "[%d] Stopped\n", c_pid);
	}
}

/*
 * Pipes stdout to a pipe.
 */
void stdout_to_pipe(int const *pipe_fd) {
	close(pipe_fd[READ_END]);
	close(STDOUT_FILENO);
	if (dup2(pipe_fd[WRITE_END], STDOUT_FILENO) == -1) perror("dup2");
}

/*
 * Pipes a pipe to stdin.
 */
void pipe_to_stdin(int const *pipe_fd) {
	close(pipe_fd[WRITE_END]);
	close(STDIN_FILENO);
	if (dup2(pipe_fd[READ_END], STDIN_FILENO) == -1) perror("dup2");
}

/*
 * Sleeps ten milliseconds using nanosleep for so many times that is given.
 */
void ten_ms_sleep(int times) {
	int i;
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 10000000; /* 10 ms */
	/* Nanosleep gets interupted by signals */
	for (i = 0; i < times; i++) {
		nanosleep(&ts, NULL);
	}
}

/*
 * Tokenizes a string for specified delimiters. The variable strs is an array of
 * unallocated strings and no_strs gives the number of output strings. The
 * allocation after the last string in strs is set to NULL.
 */
void tokenize(char **strs, int *no_strs, char const *input, char const *delim) {
	char *s;
	int i;
	malloc_strcpy(&s, input);
	malloc_strcpy(&strs[i = 0], strtok(s, delim));
	while(strs[i] != NULL) {
		malloc_strcpy(&strs[++i], strtok(NULL, delim));
	}
	*no_strs = i;
	free(s);
}
