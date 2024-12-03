#include "process.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>  
#include <limits.h> 
#include <errno.h>  

#define BG_PROCESSES_MAX 1000
#define STACK_DIR_MAX 1000

char *dir_stack[STACK_DIR_MAX];
int dir_stack_top = -1; // stack initially empty

// array that keeps track of background process PIDs
pid_t bg_processes[BG_PROCESSES_MAX];

// counter for number of active background processes
int bg_count = 0;

int handle_redirection(const CMD *cmd) {
    int fd;

    // input redirection
    if (cmd->fromType == RED_IN) {
        fd = open(cmd->fromFile, O_RDONLY);
        if (fd == -1) {
            perror("open input file");
            return -1;
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2 input");
            close(fd);
            return -1;
        }
        close(fd);
    }
    // HERE doc input redirection
    else if (cmd->fromType == RED_IN_HERE) {
        // create a temp file
        char temp_filename[] = "HEREdocXXXXXX";
        fd = mkstemp(temp_filename);
        if (fd == -1) {
            perror("mkstemp");
            return -1;
        }

        // write HERE doc content to temp file
        if (write(fd, cmd->fromFile, strlen(cmd->fromFile)) == -1) {
            perror("write to temp file");
            close(fd);
            unlink(temp_filename);
            return -1;
        }

        // reset file offset to the beginning
        if (lseek(fd, 0, SEEK_SET) == -1) {
            perror("seek");
            close(fd);
            unlink(temp_filename);
            return -1;
        }

        // redirect stdin to the temp file
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2 HERE doc");
            close(fd);
            unlink(temp_filename);
            return -1;
        }
        close(fd); // stdin now points to closed file descriptor
        unlink(temp_filename);
    }

    // output redirection
    if (cmd->toType == RED_OUT) {
        fd = open(cmd->toFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            perror("open output file");
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2 output");
            close(fd);
            return -1;
        }
        close(fd);
    } else if (cmd->toType == RED_OUT_APP) {
        fd = open(cmd->toFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd == -1) {
            perror("open append output file");
            return -1;
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2 append output");
            close(fd);
            return -1;
        }
        close(fd);
    }
    return 0;
}

int set_local_vars(const CMD *cmd) {
    for (int i = 0; i < cmd->nLocal; i++) {
        // check for setting ? variable
        if (strcmp(cmd->locVar[i], "?") == 0) {
            // ignore attempts to set ?
            fprintf(stderr, "cannot set variable '?'\n");
            continue;
        }
        if (setenv(cmd->locVar[i], cmd->locVal[i], 1) == -1) {
            perror("setenv");
            return -1;
        }
    }
    return 0;
}

// add a background process to the queue
void add_to_background_queue(pid_t pid) {
    if (bg_count < BG_PROCESSES_MAX) {
        bg_processes[bg_count++] = pid;
    } else {
        fprintf(stderr, "background process queue full\n");
    }
}

// reap background processes
void reap_background_processes() {
    int status;
    for (int i = 0; i < bg_count; i++) {
        pid_t pid = waitpid(bg_processes[i], &status, WNOHANG);
        if (pid > 0) { // process completed
            fprintf(stderr, "Completed: %d (%d)\n", pid, WEXITSTATUS(status)); // WEXITSTATUS helps with pipeline fails
            // remove completed process from queue
            for (int j = i; j < bg_count - 1; j++) {
                bg_processes[j] = bg_processes[j + 1];
            }
            bg_count--;
            i--; // removed element
        }
    }
}

// handle background commands (recursively)
void end_background(const CMD *cmd) {
    if (cmd->type == SEP_BG) {
        end_background(cmd->left);  // process left command in background
        if (cmd->right) end_background(cmd->right); // left command
    } else if (cmd->type == SEP_END) {
        process(cmd->left);  // execute left command
        if (cmd->right) end_background(cmd->right);  // right command
    } else {
        // process as background command
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
        } else if (pid == 0) {
            if (set_local_vars(cmd) == -1) exit(EXIT_FAILURE);
            if (handle_redirection(cmd) == -1) exit(EXIT_FAILURE);
            if (execvp(cmd->argv[0], cmd->argv) == -1) {
                perror("execvp");
                exit(EXIT_FAILURE);
            }
        } else {
            // add backgrounded PID to queue (parent)
            fprintf(stderr, "Backgrounded: %d\n", pid);
            add_to_background_queue(pid);
        }
    }
}

// push a directory onto stack
int push_directory(const char *dir) {
    if (dir_stack_top >= STACK_DIR_MAX - 1) {
        fprintf(stderr, "directory stack full\n");
        return -1;
    }
    dir_stack[++dir_stack_top] = strdup(dir); // free when popping!!!
    return 0;
}

// pop a directory from stack
char *pop_directory() {
    if (dir_stack_top < 0) {
        fprintf(stderr, "directory stack empty\n");
        return NULL;
    }
    char *dir = dir_stack[dir_stack_top--];
    return dir;
}

// print current directory stack
void print_directory() {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return;
    }
    printf("%s", cwd);
    for (int i = dir_stack_top; i >= 0; i--) {
        printf(" %s", dir_stack[i]);
    }
    printf("\n");
}

int is_builtin(const CMD *cmd) {
    if (cmd->argc == 0) {
        return 0; // no command to process
    }
    const char *cmd_name = cmd->argv[0];
    if (strcmp(cmd_name, "cd") == 0 ||
        strcmp(cmd_name, "pushd") == 0 ||
        strcmp(cmd_name, "popd") == 0) {
        return 1;
    }
    return 0;
}

int execute_builtin(const CMD *cmd) {
    const char *cmd_name = cmd->argv[0];
    int status = 0;

    if (strcmp(cmd_name, "cd") == 0) {
        // cd
        const char *target_dir = cmd->argv[1];
        if (cmd->argc > 2) {
            fprintf(stderr, "cd: too many arguments\n");
            return 1; // not 0 for fail
        }
        if (target_dir == NULL) {
            // no arg -> change to HOME
            target_dir = getenv("HOME");
            if (target_dir == NULL) {
                fprintf(stderr, "cd: HOME not set\n");
                return 1;
            }
        }
        if (chdir(target_dir) != 0) {
            int saved_errno = errno; // store errno before calling perror (not really sure why, got from ed)
            perror("cd");
            return saved_errno;
        }
    } else if (strcmp(cmd_name, "pushd") == 0) {
        // pushd
        if (cmd->argc != 2) {
            fprintf(stderr, "pushd: usage: pushd <dir>\n");
            return 1;
        }
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            int saved_errno = errno; 
            perror("getcwd");
            return saved_errno;
        }
        if (push_directory(cwd) != 0) {
            return 1;
        }
        if (chdir(cmd->argv[1]) != 0) {
            int saved_errno = errno; 
            perror("pushd");
            // pop the directory that was just pushed
            free(pop_directory());
            return saved_errno;
        }
        print_directory();
    } else if (strcmp(cmd_name, "popd") == 0) {
        // popd
        if (cmd->argc != 1) {
            fprintf(stderr, "popd: too many arguments\n");
            return 1;
        }
        char *dir = pop_directory();
        if (dir == NULL) {
            fprintf(stderr, "popd: directory stack empty\n");
            return 1;
        }
        if (chdir(dir) != 0) {
            int saved_errno = errno;
            perror("popd");
            free(dir);
            return saved_errno;
        }
        free(dir);
        print_directory();
    } else {
        // should not get here
        fprintf(stderr, "unknown command: %s\n", cmd_name);
        return 1;
    }
    return status;
}

void clean_directory_stack() {
    while (dir_stack_top >= 0) {
        free(dir_stack[dir_stack_top]);
        dir_stack[dir_stack_top--] = NULL;
    }
}

void update_exit_status(int status) {
    char status_str[16];
    sprintf(status_str, "%d", status);
    if (setenv("?", status_str, 1) == -1) {
        perror("setenv");
    }
}

int process_pipeline(const CMD *cmd) {
    if (cmd == NULL) {
        return 0;
    }
    pid_t pid;
    int status = 0;
    int exit_status = 0;
    const CMD *commands[1024];
    int cmd_count = 0;
    const CMD *current = cmd;

    // create commands array
    while (current != NULL && cmd_count < 1024) {
        if (current->type == PIPE) {
            if (current->right == NULL) {
                // fprintf(stderr, "current->right NULL in PL\n");
                exit(EXIT_FAILURE);
            }
            commands[cmd_count++] = current->right;
            current = current->left;
        } else {
            commands[cmd_count++] = current;
            current = NULL;
        }
    }

    // reverse array to get commands in order
    for (int i = 0; i < cmd_count / 2; i++) {
        const CMD *temp = commands[i];
        commands[i] = commands[cmd_count - i - 1];
        commands[cmd_count - i - 1] = temp;
    }

    int pipefd[2];
    int prev_fd = -1;
    pid_t pids[1000];

    for (int i = 0; i < cmd_count; i++) {
        if (i < cmd_count - 1) {
            if (pipe(pipefd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
            // fprintf(stderr, "pipefd[0]=%d, pipefd[1]=%d\n", pipefd[0], pipefd[1]);
        }

        const CMD *cmd_to_execute = commands[i];

        if (cmd_to_execute == NULL || cmd_to_execute->argv == NULL || cmd_to_execute->argv[0] == NULL) {
            // fprintf(stderr, "command is invalid\n");
            exit(EXIT_FAILURE);
        }

        // fprintf(stderr, "forking for command: %s\n", cmd_to_execute->argv[0]);

        pid = fork();
        if (pid == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            // child process
            signal(SIGINT, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);

            // stdin setup
            if (prev_fd != -1) {
                if (dup2(prev_fd, STDIN_FILENO) == -1) {
                    perror("dup2 stdin");
                    exit(EXIT_FAILURE);
                }
                close(prev_fd);
            }

            // stdout setup
            if (i < cmd_count - 1) {
                if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
                    perror("dup2 stdout");
                    exit(EXIT_FAILURE);
                }
                close(pipefd[1]);
                close(pipefd[0]);
            }

            // close the unused file descriptors
            for (int fd = 3; fd < 100; fd++) {
                if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
                    close(fd);
                }
            }

            // hgandle redirection and local variables
            if (set_local_vars(cmd_to_execute) == -1) exit(EXIT_FAILURE);
            if (handle_redirection(cmd_to_execute) == -1) exit(EXIT_FAILURE);

            // execute command
            if (is_builtin(cmd_to_execute)) {
                int builtin_status = execute_builtin(cmd_to_execute);
                exit(builtin_status);
            } else {
                if (execvp(cmd_to_execute->argv[0], cmd_to_execute->argv) == -1) {
                    perror("execvp");
                    exit(EXIT_FAILURE);
                }
            }
        } else {
            // parent process
            pids[i] = pid;

            // close previous read
            if (prev_fd != -1) {
                close(prev_fd);
            }

            // update prev_fd
            if (i < cmd_count - 1) {
                prev_fd = pipefd[0];
                close(pipefd[1]);
            } else {
                prev_fd = -1;
            }
        }
    }

    // wait for child processes
    for (int i = 0; i < cmd_count; i++) {
        if (waitpid(pids[i], &status, 0) == -1) {
            perror("waitpid");
            continue;
        }
        int child_exit_status = STATUS(status);
        if (child_exit_status != 0) {
            exit_status = child_exit_status;
        }
    }

    update_exit_status(exit_status);
    return exit_status;
}

int process_subcommand(const CMD *cmd) {
    pid_t pid = fork();
    int status;

    if (pid == -1) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        // child process (subshell)
        signal(SIGINT, SIG_DFL); 
        if (set_local_vars(cmd) == -1) exit(EXIT_FAILURE);
        if (handle_redirection(cmd) == -1) exit(EXIT_FAILURE);
        exit(process(cmd->left)); // process the subcommand
    } else {
        // parent process
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return -1;
        }
        update_exit_status(WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
}

int process(const CMD *cmd) {
    reap_background_processes(); // reap completed background processes to start

    if (cmd == NULL) {
        return 0;
    }

    int status = 0;

    switch (cmd->type) {
        case PIPE:
            return process_pipeline(cmd);
        case SEP_END:
            process(cmd->left);  // execute left command
            return process(cmd->right);  // execute right
        case SEP_BG:
            // handle background commands
            end_background(cmd->left);
            if (cmd->right) return process(cmd->right);
            return 0;
        case SEP_AND:
            status = process(cmd->left);
            if (status == 0) {
                // left command success -> execute right command
                return process(cmd->right);
            } else {
                // left command fail -> skip right command
                update_exit_status(status);
                return status;
            }
        case SEP_OR:
            status = process(cmd->left);
            if (status != 0) {
                // left command fail -> execute right command
                return process(cmd->right);
            } else {
                // left command succss -> skip right command
                update_exit_status(status);
                return status;
            }
        case SUBCMD:
            // handle subcommands
            return process_subcommand(cmd);
        default:
            break;
    }

    // handle built in commands
    if (is_builtin(cmd)) {
        if (set_local_vars(cmd) == -1) return -1;
        if (handle_redirection(cmd) == -1) return -1;
        status = execute_builtin(cmd);
        update_exit_status(status);
        return status;
    }

    // handle normal foreground command
    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        return -1;
    } else if (pid == 0) {
        // child process foreground command
        signal(SIGINT, SIG_DFL); 
        if (set_local_vars(cmd) == -1) exit(EXIT_FAILURE);
        if (handle_redirection(cmd) == -1) exit(EXIT_FAILURE);
        if (execvp(cmd->argv[0], cmd->argv) == -1) {
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    } else {
        // parent process (wait for the child to complete)
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return -1;
        }
        update_exit_status(WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    return 0;
}
