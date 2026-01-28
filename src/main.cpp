#include <iostream>
#include <string>
#include <vector>
#include <readline/readline.h>
#include <readline/history.h>
#include <signal.h>
#include <unistd.h>
#include <cstring>
#include <sys/wait.h>
#include "parser.h"
#include "executor.h"
#include "utils.h"

using namespace std;
// a flag to tell main loop something changed
volatile sig_atomic_t child_changed = 0;

void sigchld_handler(int sig) {
    // save errno because waitpid might overwrite it, 
    // and we want to restore it so the main loop doesn't get confused
    int saved_errno = errno;

    // reap all dead children, using loop cuz if multiple children die at once, kernel 
    // might send only one signal
    // WNOHANG: return immediately if no more dead children
    while (waitpid(-1, nullptr, WNOHANG) > 0) {
        child_changed = 1;
    }

    errno = saved_errno;
}

void setup_sigchld() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &sigchld_handler;
    sigemptyset(&sa.sa_mask);
    
    // SA_RESTART: automatically restart interrupted system calls (like readline)
    // SA_NOCLDSTOP: don't send signal when child is stopped (Ctrl+Z), only when dead.
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, nullptr) == -1) {
        perror("sigaction");
        exit(1);
    }
}

int main() {
  // save the terminal state of the shell itself at startup
  if (tcgetattr(STDIN_FILENO, &shell_tmodes) < 0) {
        perror("tcgetattr");
  }

  setup_sigchld();

  std::ios_base::sync_with_stdio(false);
  // flush after every cout / cerr
  cout << unitbuf;
  cerr << unitbuf;

  while (1) {
    // reap all: cleans up kernel's process table
    // we do this every loop iteration, even if child_changed == 0, to be safe against mixed signals
    int status;
    pid_t reaped_pid;
    while ((reaped_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // find the job in the list and mark it as finished
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].pid == reaped_pid) {
                cout << "\n[" << (i + 1) << "]  Done  " << jobs[i].command << endl;
                jobs.erase(jobs.begin() + i);
                break; 
            }
        }
    }
    child_changed = 0; // reset after reaping everything current
    char* input_ptr = readline("$ ");
    if (input_ptr == nullptr) {
      cout << endl;
      break;
    }
    string input(input_ptr);

    if(input.empty()){
      free(input_ptr);
      continue;
    }

    add_history(input_ptr);
    manual_history_list.push_back(input);
    history_count++;
    if (manual_history_list.size() > MAX_HISTORY) {
      manual_history_list.pop_front();
    }

    vector<Token> tokens = parse(input);

    /*for (auto &tok : tokens)*/
    /* cout << tok << endl;*/

    // split tokens into sequential command groups by ';'
    vector<vector<Token>> command_sequences;
    vector<Token> current_seq;
    for (const auto &tok : tokens) {
        if (tok.type == Semicolon || tok.type == Background) {
          if (tok.type == Background) {
                current_seq.push_back(tok);
          }
          if (!current_seq.empty()) command_sequences.push_back(current_seq);
          current_seq.clear();
        } else {
            current_seq.push_back(tok);
        }
    }
    if (!current_seq.empty()) command_sequences.push_back(current_seq);

    for (auto &seq : command_sequences) {
        vector<Tree> pipeline;
        vector<Token> current_cmd_tokens;
        bool is_bg = false;

        // scan this specific sequence for the Background token
        for (const auto &tok : seq) {
            if (tok.type == Background) {
                is_bg = true;
                break; 
            }
        }

        for (const auto &tok : seq) {
            if (tok.type == Background) continue; // not passing & to check() 
            if (tok.type == Pipe) {
                if (!current_cmd_tokens.empty()) {
                    pipeline.push_back(check(current_cmd_tokens));
                    current_cmd_tokens.clear();
                } 
            } else {
                current_cmd_tokens.push_back(tok);
            }
        }
        if (!current_cmd_tokens.empty()) {
            pipeline.push_back(check(current_cmd_tokens));
        }

        // transfer the flag to every command in pipe
        for (auto &ast : pipeline) {
            ast.is_background = is_bg;
        }

        execute_pipeline(pipeline);
    }
    /*Tree ast = check(tokens);*/ // old single-command check

    /*cout << ast << endl;*/
    /*execute(ast);*/ // old single-command execute

    /*fs::path p = find_in_path(words[0]);*/
    /**/
    /*if (!p.empty())*/
    /* system(input.c_str());*/
    /*else*/
    /* cout << words[0] << ": command not found" << endl;*/

    free(input_ptr);
  }
}
