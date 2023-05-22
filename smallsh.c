#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>

#ifndef MAX_WORDS
#define MAX_WORDS 512
#endif

#ifndef MAX_JOBS
#define MAX_JOBS 500
#endif

char *words[MAX_WORDS];
size_t wordsplit(char const *line);
char * expand(char const *word);
int exitStatus = 0;
pid_t foregroundProcess = -1;

// array for bacground process management
int jobs_array[MAX_JOBS] = {0};

int background_flag = 0;
pid_t backgroundPID = -1;

void sigint_handler(int sig) {};
void sigtstp_handler(int sig) {};

int main(int argc, char *argv[])
{

  struct sigaction new_sigint, old_sigint;
  new_sigint.sa_handler = sigint_handler;
  sigfillset(&new_sigint.sa_mask);
  new_sigint.sa_flags = 0;
  sigaction(SIGINT, &new_sigint, &old_sigint);

  struct sigaction new_sigtstp, old_sigtstp;
  new_sigtstp.sa_handler = SIG_IGN;
  sigfillset(&new_sigtstp.sa_mask);
  new_sigtstp.sa_flags = 0;
  sigaction(SIGTSTP, &new_sigtstp, &old_sigtstp);


  FILE *input = stdin;
  char *input_fn = "(stdin)";
  if (argc == 2) {
    input_fn = argv[1];
    input = fopen(input_fn, "re");
    if (!input) err(1, "%s", input_fn);
  } else if (argc > 2) {
    errx(1, "too many arguments");
  }

  char *line = NULL;
  size_t n = 0;
  char *exp_words[MAX_WORDS] = {0};
 

  for (;;) {
prompt:;

    errno = 0;
    /* TODO: Manage background processes */
    // check for terminated background processes
    int i;

    for (i = 0; i < MAX_JOBS; i++) {
      int childPid;
      int childStatus;

      if (jobs_array[i] != 0) {
        childPid = waitpid(jobs_array[i], &childStatus, WNOHANG | WUNTRACED);

        if (childPid > 0) {
          jobs_array[i] = 0;

          if (WIFEXITED(childStatus)) {
            exitStatus = WEXITSTATUS(childStatus);
            fprintf(stderr, "Child process %d done. Exit status %d.\n", childPid, exitStatus);
            fflush(stdout);
          }
          if (WIFSTOPPED(childStatus)) {
            kill(childPid, SIGCONT);
            fprintf(stderr, "Child process %d stopped. Continuing.\n", childPid);
          }
          if (WIFSIGNALED(childStatus)) {
            int signal_num = WTERMSIG(childStatus);
            fprintf(stderr, "Child process %d done. Signaled %d.\n", childPid, signal_num);
            fflush(stdout);
          }
          errno = 0;
         } else if (childPid == -1) {
            perror("Issue in bg job check\n");
        }

      }
      errno = 0;
    }

    background_flag = 0;


    /* TODO: prompt */
    if (input == stdin) {
        char* prompt = getenv("PS1");
        if (prompt != NULL) {
          fprintf(stderr, "%s", prompt);
        }
    }

    ssize_t line_len = getline(&line, &n, input);

    if (line_len == -1) {
      if (errno == EINTR) {
        clearerr(input);
        fprintf(stderr, "\n");
        continue;
      } else if (errno == 0) {
        break;
      } else {
        perror("getline error");
        break;
      }
    }
    
    size_t nwords = wordsplit(line);
    if (nwords == 0) goto prompt;

    signal(SIGINT, SIG_IGN);
 
    // expand words
    for (size_t i = 0; i < nwords; ++i) {
      char *exp_word = expand(words[i]);
      if (exp_word == NULL) {
        fprintf(stderr, "Error expanding word: %s\n", words[i]);
      }
      free(words[i]);
      exp_words[i] = malloc(strlen(exp_word) + 1);
      if (exp_words[i] == NULL) {
        fprintf(stderr, "Error allocating memory for expanded word: %s\n", exp_words[i]);
        exit(1);
      }
      if ((strcmp(exp_word, "&") == 0) && (i + 1 == nwords)) {
        background_flag = 1;
      }
      strcpy(exp_words[i], exp_word);
      free(exp_word);
    } 
    
    
    // built-in command functionality
    if (nwords > 0) {
      if (strcmp(exp_words[0], "exit") == 0) {
        if (nwords > 1) {
          exitStatus = atoi(exp_words[1]);
        }
        return exitStatus;
      } else if (strcmp(exp_words[0], "cd") == 0) {
        if (nwords > 1) {
          if (chdir(exp_words[1]) != 0) {
            perror("cd");
          }
        } else {
          const char *home_dir = getenv("HOME");
          if (home_dir == NULL) {
            fprintf(stderr, "cd: no home directory found.\n");
          } else {
            if (chdir(home_dir) != 0) {
              perror("cd");
            }
          }
        }
      }
    }
    
    if ((strcmp(exp_words[0], "exit") == 0) || (strcmp(exp_words[0], "cd") == 0)) {
      goto prompt;
    }

    pid_t pid = fork();

    if (pid < 0) {
      fprintf(stderr, "fork() failed!");
      // perror("fork() failed!");
      exit(1);
      break;
    } else if (pid == 0) {
      
      sigaction(SIGINT, &old_sigint, NULL);
      sigaction(SIGTSTP, &old_sigtstp, NULL);

      int input_fd = STDIN_FILENO;
      int output_fd = STDOUT_FILENO;
      int append_fd = STDOUT_FILENO;
      int exec_arg_counter = 0;
      int input_redirect_flag = 0;
      int output_redirect_flag = 0;
      int append_redirect_flag = 0;
      // create a null-termianted array of pointers
      char *exec_args[MAX_WORDS + 1];
      for (size_t i = 0; i < nwords; i++) {

        if (strcmp(exp_words[i], "<") == 0) {
          if (i + 1 < nwords) {
            char *input_file = exp_words[i + 1];
            input_fd = open(input_file, O_RDONLY);
            if (input_fd == -1) {
              perror("Failed to open input file\n");
              exit(1);
            }
            i++;
            input_redirect_flag = 1;
          }
          // continue;
        } else if (strcmp(exp_words[i], ">") == 0) {
          if (i + 1 < nwords) {
            char *output_file = exp_words[i + 1];
            output_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0777);
            if (output_fd == -1) {
              perror("Failed to open output file\n");
              exit(1);
            }
            i++;
            output_redirect_flag = 1;
            append_redirect_flag = 0;
          }
          // continue;
        } else if (strcmp(exp_words[i], ">>") == 0) {
          if (i + 1 < nwords) {
            char *append_file = exp_words[i + 1];
            append_fd = open(append_file, O_WRONLY | O_CREAT | O_APPEND, 0777);
            if (append_fd == -1) {
              perror("Failed to open output file\n");
              exit(1);
            }
            i++;
            append_redirect_flag = 1;
            output_redirect_flag = 0;
          }
        } else if (strcmp(exp_words[i], "&") == 0) {
          continue;          
        } else {
          exec_args[exec_arg_counter] = exp_words[i];
          exec_arg_counter++;
        }
       

      }
      exec_args[exec_arg_counter] = NULL;

      if (input_redirect_flag) {
        if (dup2(input_fd, STDIN_FILENO) == -1) {
          perror("Failed to redirect input\n");
          close(input_fd);
          exit(1);
        }
        close(input_fd);
      }
      if (output_redirect_flag || append_redirect_flag) {
        int redirect_fd;
        if (output_redirect_flag) 
          redirect_fd = output_fd;
        else
          redirect_fd = append_fd;

        if (dup2(redirect_fd, STDOUT_FILENO) == -1) {
          perror("Failed to redirect output\n");
          close(output_fd);
          exit(1);
        } 
        close(output_fd);
      }
      if (append_redirect_flag) {
        if (dup2(append_fd, STDOUT_FILENO) == -1) {
          perror("Failed to redirect output\n");
          close(append_fd);
          exit(1);
        } 
        close(append_fd);
      }
      execvp(exec_args[0], exec_args);
      perror("Error executing command");
      exit(2);
    } else {
      int waitpid_flag = 0;
      if (background_flag) {
        backgroundPID = pid;

        int i;
        for (i = 0; i < MAX_JOBS; i++) {
          if (jobs_array[i] == 0) {
            jobs_array[i] = backgroundPID;
            break;
          }
        }
        waitpid_flag = WNOHANG;
      }

      if (!background_flag) {
        int childExitMethod;
        pid_t terminatedChildPID = waitpid(pid, &childExitMethod, waitpid_flag | WUNTRACED);

       if (terminatedChildPID > 0) {
          if (WIFEXITED(childExitMethod)) {
            exitStatus = WEXITSTATUS(childExitMethod);
          } 
          if (WIFSIGNALED(childExitMethod)) {
            exitStatus = WTERMSIG(childExitMethod) + 128;
          }
          if (WIFSTOPPED(childExitMethod)) {
            kill(terminatedChildPID, SIGCONT);
            backgroundPID = terminatedChildPID;
            fprintf(stderr, "Child process %d stopped. Continuing.\n", terminatedChildPID);
          }
        }
      }

      fflush(stdout);
      goto prompt;
    }
  }
  free(line);
  return 0;
}

char *words[MAX_WORDS] = {0};

/* Splits a string into words delimited by whitespace. Recognizes
 * comments as '#' at the beginning of a word, and backslash escapes.
 *
 * Returns number of words parsed, and updates the words[] array
 * with pointers to the words, each as an allocated string.
 */
size_t wordsplit(char const *line) {
  size_t wlen = 0;
  size_t wind = 0;

  char const *c = line;
  for (;*c && isspace(*c); ++c); /* discard leading space */

  for (; *c;) {
    if (wind == MAX_WORDS) break;
    /* read a word */
    if (*c == '#') break;
    for (;*c && !isspace(*c); ++c) {
      if (*c == '\\') ++c;
      void *tmp = realloc(words[wind], sizeof **words * (wlen + 2));
      if (!tmp) err(1, "realloc");
      words[wind] = tmp;
      words[wind][wlen++] = *c; 
      words[wind][wlen] = '\0';
    }
    ++wind;
    wlen = 0;
    for (;*c && isspace(*c); ++c);
  }
  return wind;
}


/* Find next instance of a parameter within a word. Sets
 * start and end pointers to the start and end of the parameter
 * token.
 */

char
param_scan(char const *word, char const **start, char const **end) {
  static char const *prev;
  if (!word) word = prev;

  char ret = 0;
  *start = 0;
  *end = 0;
  for (char const *s = word; *s && !ret; ++s) {
    // check if the '$' character is in the string and get the pointer to its first occurence
    s = strchr(s, '$');
    if (!s) break;
    switch (s[1]) {
      case '$':
      case '!':
      case '?':
        ret = s[1];
        *start = s;
        *end = s + 2;
        break;
      case '{':
        char *e = strchr(s + 2, '}');
        if (e) {
          ret = s[1];
          *start = s;
          *end = e + 1;
        }
        break;
    }
  }
  prev = *end;
  return ret;
}


/* Simple string-builder function. Builds up a base
 * string by appending supplied strings/character ranges
 * to it.
 */
char *
build_str(char const *start, char const *end) {
  static size_t base_len = 0;
  static char *base = 0;

  if (!start) {
    /* Reset; new base string, return old one */
    char *ret = base;
    base = NULL;
    base_len = 0;
    return ret;
  }
  /* Append [start, end) to base string 
   * If end is NULL, append whole start string to base string.
   * Returns a newly allocated string that the caller must free.
   */
  size_t n = end ? end - start : strlen(start);
  size_t newsize = sizeof *base *(base_len + n + 1);
  void *tmp = realloc(base, newsize);
  if (!tmp) err(1, "realloc");
  base = tmp;
  memcpy(base + base_len, start, n);
  base_len += n;
  base[base_len] = '\0';

  return base;
}

/* Expands all instances of $! $$ $? and ${param} in a string 
 * Returns a newly allocated string that the caller must free
 */
char *
expand(char const *word) 
{
  char const *pos = word;
  char const *start, *end;
  char c = param_scan(pos, &start, &end);
  build_str(NULL, NULL);
  build_str(pos, start);
  while (c) {
    if (c == '!') { 
      if (backgroundPID == -1) {
        build_str("", NULL);
      } else {
        char bgPID_str[10];
        sprintf(bgPID_str, "%d", (int)backgroundPID);
        build_str(bgPID_str, NULL);
      }
    } else if (c == '$') {
      pid_t pid = getpid();
      char pid_str[10];
      sprintf(pid_str, "%d", (int)pid);
      build_str(pid_str, NULL);
    } else if (c == '?') {
      char exit_status_str[16];
      sprintf(exit_status_str, "%d", exitStatus);
      build_str(exit_status_str, NULL);
    } else if (c == '{') {
      const char *param_start = start + 2;
      const char *param_end = end - 1;
      size_t param_length = param_end - param_start;
      char parameter[param_length + 1];
      strncpy(parameter, param_start, param_length);
      parameter[param_length] = '\0';
      
      char *value = getenv(parameter);
      if (value != NULL) {
        build_str(value, NULL);
      } else {
        build_str("", NULL);
      }
    }
    pos = end;
    c = param_scan(pos, &start, &end);
    build_str(pos, start);
  }
  return build_str(start, NULL);
}

