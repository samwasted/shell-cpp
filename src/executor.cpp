#include "executor.h"
#include "utils.h"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <algorithm>
#include <signal.h>
#include <termios.h>
#define ALL(s) (s).begin(), (s).end()
using namespace std;

void execute(const Tree &ast) {
  int out_fd = -1;
  int err_fd = -1;
  int saved_stdout = -1;
  int saved_stderr = -1;
  vector<Tree> filtered_children;

 for (size_t i = 0; i < ast.children.size(); i++) {
    string val = ast.children[i].value;

    if ((val.find(">") != string::npos) && i + 1 < ast.children.size()) {
        string filename = ast.children[i + 1].value;
        
        int flags = O_WRONLY | O_CREAT;
        if (val.find(">>") != string::npos) {
            flags |= O_APPEND; // Keep existing content
        } else {
            flags |= O_TRUNC;  // Wipe existing content
        }

        int fd = open(filename.c_str(), flags, 0644);
        if (fd < 0) { perror("open"); return; }

        if (val.find("2") != string::npos) {
            if (err_fd != -1) close(err_fd);
            err_fd = fd;
        } else {
            if (out_fd != -1) close(out_fd);
            out_fd = fd;
        }
        i++;
    } else {
        filtered_children.push_back(ast.children[i]);
    }
  } 
  if (ast.type == Builtin) {
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (out_fd != -1) {
      dup2(out_fd, STDOUT_FILENO);
      close(out_fd);
    }
    if (err_fd != -1) {
      dup2(err_fd, STDERR_FILENO);
      close(err_fd);
    }

    if (ast.value == "cd") {
      if (!filtered_children.empty()) {
          chdir_logic(filtered_children[0].value);
      } else {
          cout << "specify a path to continue" << endl;
      }
    } else if (ast.value == "echo") {
      for (size_t i = 0; i < filtered_children.size(); i++) {
        cout << filtered_children[i].value << (i < filtered_children.size() - 1 ? " " : "");
      }
      cout << endl;
    } else if (ast.value == "exit") {
      exit(0);
    } else if (ast.value == "pwd") {
      cout << fs::current_path().c_str() << endl;
    } else if (ast.value == "type") {
      for (const auto &child : filtered_children) {
        if (find(ALL(builtins), child.value) != builtins.end()) {
            cout << child.value << " is a shell builtin" << endl;
        } else {
            fs::path p = find_in_path(child.value);
            if (!p.empty()) cout << child.value << " is " << p.string() << endl;
            else cout << child.value << ": not found" << endl;
        }
      }
    } else if (ast.value == "history") {
        size_t start_index = history_count - manual_history_list.size() + 1;
        size_t i = 0;

        if (!filtered_children.empty()) {
            const string& arg = filtered_children[0].value;
            
            bool is_numeric = !arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit);

            if (!is_numeric) {
                cerr << "history: " << arg << ": numeric argument required" << endl;
                return;
            }

            if (filtered_children.size() > 1) {
                cerr << "history: too many arguments" << endl;
                return;
            }

            try {
                long long requested = std::stoll(arg);
                if (requested < 0) {
                    // technically bash handles negative numbers differently,
                    // but for our shell, this is an error
                    cerr << "history: " << arg << ": invalid option" << endl;
                    return;
                }
                
                size_t n = static_cast<size_t>(requested);
                if (manual_history_list.size() > n) {
                    i = manual_history_list.size() - n;
                }
            } catch (...) {
                // handles numbers too large for long long
                cerr << "history: " << arg << ": numeric argument required" << endl;
                return;
            }
        }

        for (; i < manual_history_list.size(); ++i) {
            cout << "  " << (start_index + i) << "  " << manual_history_list[i] << endl;
        }
    } else if (ast.value == "jobs") {
        for (size_t i = 0; i < jobs.size(); ++i) {
            cout << "[" << i + 1 << "]  Running  " << jobs[i].command << " (" << jobs[i].pid << ")" << endl;
        }
    }

    // restore parent descriptors
    if (saved_stdout != -1) {   
      dup2(saved_stdout, STDOUT_FILENO);
      close(saved_stdout);
    }
    if (saved_stderr != -1) {
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
    }

  } else if (ast.type == ExecutableFile) {
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &oldmask); // block before fork

    pid_t pid = fork();

    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &oldmask, nullptr); 

        if (out_fd != -1) dup2(out_fd, STDOUT_FILENO);
        if (err_fd != -1) dup2(err_fd, STDERR_FILENO);

        vector<char*> argv;
        argv.push_back(const_cast<char*>(ast.value.c_str()));
        for (const auto& child : filtered_children) {
            argv.push_back(const_cast<char*>(child.value.c_str()));
        }
        argv.push_back(nullptr);

        execv(ast.path.c_str(), argv.data());
        perror("execv failed");
        exit(1);

    } else if (pid > 0) {
        if (out_fd != -1) close(out_fd);
        if (err_fd != -1) close(err_fd);

        if (!ast.is_background) {
            // FOREGROUND: The shell waits
            int status;
            if (waitpid(pid, &status, WUNTRACED) > 0) {
                // reclaim terminal
                tcsetpgrp(STDIN_FILENO, getpgrp());
                tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_tmodes);

                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    if (code != 0) cout << "Error code " << code << " encountered" << endl;
                } else if (WIFSIGNALED(status)) {
                    cout << "Terminated by signal " << WTERMSIG(status) << endl;
                }
            }
        } else {
            // BACKGROUND: The shell records and moves on immediately
            jobs.push_back({pid, ast.value, true});
            cout << "[" << jobs.size() << "] " << pid << endl;
            // no waitpid here
        }

        // unblock SIGCHLD so handler can work
        sigprocmask(SIG_SETMASK, &oldmask, nullptr);
    } else {
        perror("fork failed");
        sigprocmask(SIG_SETMASK, &oldmask, nullptr); // cleanup on error
    }
  } else {
    cout << ast.value << ": command not found" << endl;
    if (out_fd != -1) close(out_fd);
    if (err_fd != -1) close(err_fd);
  }
}
void execute_child_logic(const Tree &ast) {
  int out_fd = -1;
  int err_fd = -1;
  vector<Tree> filtered_children;

  // scan for redirection tokens: ">", "1>", or "2>"
  for (size_t i = 0; i < ast.children.size(); i++) {
    string val = ast.children[i].value;

    if ((val.find(">") != string::npos) && i + 1 < ast.children.size()) {
        string filename = ast.children[i + 1].value;
        
        int flags = O_WRONLY | O_CREAT;
        if (val.find(">>") != string::npos) {
            flags |= O_APPEND; // keep existing content
        } else {
            flags |= O_TRUNC;  // wipe existing content
        }

        int fd = open(filename.c_str(), flags, 0644);
        if (fd < 0) { perror("open"); return; }

        if (val.find("2") != string::npos) {
            if (err_fd != -1) close(err_fd);
            err_fd = fd;
        } else {
            if (out_fd != -1) close(out_fd);
            out_fd = fd;
        }
        i++;
    } else {
        filtered_children.push_back(ast.children[i]);
    }
  }

  // apply redirection
  // in a pipeline child, we just overwrite FD 1 or FD 2
  // no restore needed as this process image is temporary
  if (out_fd != -1) {
    if (dup2(out_fd, STDOUT_FILENO) < 0) { 
      perror("dup2 stdout"); 
      exit(1); 
    }
    close(out_fd);
  }

  if (err_fd != -1) {
    if (dup2(err_fd, STDERR_FILENO) < 0) { 
      perror("dup2 stderr"); 
      exit(1); 
    }
    close(err_fd);
  }
  // if process is background, it needs its own group id
  if (ast.is_background) {
      setpgid(0, 0); 
  }

  // execution Switch
  switch (ast.type) {
  case Builtin: {
    if (ast.value == "cd") {
      if (!filtered_children.empty()) {
          chdir_logic(filtered_children[0].value);
      }
    } else if (ast.value == "echo") {
      for (size_t i = 0; i < filtered_children.size(); i++) {
        cout << filtered_children[i].value;
        if (i < filtered_children.size() - 1) cout << " ";
      }
      cout << endl;
    } else if (ast.value == "exit") {
      exit(0);
    } else if (ast.value == "pwd") {
      cout << fs::current_path().c_str() << endl;
    } else if (ast.value == "type") {
      for (const auto &child : filtered_children) {
        if (find(ALL(builtins), child.value) != builtins.end()) {
            cout << child.value << " is a shell builtin" << endl;
        } else {
            fs::path p = find_in_path(child.value);
            if (!p.empty()) cout << child.value << " is " << p.string() << endl;
            else cout << child.value << ": not found" << endl;
        }
      }
    } else if (ast.value == "history") {
      size_t start_index = history_count - manual_history_list.size() + 1;
      size_t i = 0;

      if (!filtered_children.empty()) {
          const string& arg = filtered_children[0].value;
          
          bool is_numeric = !arg.empty() && std::all_of(arg.begin(), arg.end(), ::isdigit);

          if (!is_numeric) {
              cerr << "history: " << arg << ": numeric argument required" << endl;
              exit(1);
          }

          if (filtered_children.size() > 1) {
              cerr << "history: too many arguments" << endl;
              return;
          }

          try {
              long long requested = std::stoll(arg);
              if (requested < 0) {
                  // technically bash handles negative numbers differently,
                  // but for our shell, this is an error
                  cerr << "history: " << arg << ": invalid option" << endl;
                  return;
              }
              
              size_t n = static_cast<size_t>(requested);
              if (manual_history_list.size() > n) {
                  i = manual_history_list.size() - n;
              }
          } catch (...) {
              // handles numbers too large for long long
              cerr << "history: " << arg << ": numeric argument required" << endl;
              return;
          }
      }

      for (; i < manual_history_list.size(); ++i) {
          cout << "  " << (start_index + i) << "  " << manual_history_list[i] << endl;
      }
    } else if (ast.value == "jobs") {
        for (size_t i = 0; i < jobs.size(); ++i) {
            cout << "[" << i + 1 << "]  Running  " << jobs[i].command << " (" << jobs[i].pid << ")" << endl;
        }
    }
  } break;

  case ExecutableFile: {
    vector<char*> argv;
    argv.push_back(const_cast<char*>(ast.value.c_str()));

    for (const auto& child : filtered_children) {
      argv.push_back(const_cast<char*>(child.value.c_str()));
    }
    argv.push_back(nullptr);

    // replace the child process image with the program
    execv(ast.path.c_str(), argv.data());

    perror("execv failed");
    exit(1);
  } break;

  default:
    // ensure error messages go to current STDERR (which might be redirected)
    cerr << ast.value << ": command not found" << endl;
    exit(1);
    break;
  }
}
void execute_pipeline(const vector<Tree> &pipeline) {
  int n = pipeline.size();
  if (n == 0) return;

  // if only one command, we run it normally
  if (n == 1 && pipeline[0].type == Builtin) {
      execute(pipeline[0]); 
      return;
  }

  int pipefds[2 * (n - 1)];
  for (int i = 0; i < n - 1; i++) {
      if (pipe(pipefds + i * 2) < 0) {
          perror("pipe");
          return;
      }
  }
  sigset_t mask, oldmask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &mask, &oldmask);

  vector<pid_t> children_pids;

  for (int i = 0; i < n; i++) {
    pid_t pid = fork();
    if (pid == 0) {

      // unblock signals in the child
      sigprocmask(SIG_SETMASK, &oldmask, nullptr);

      // redirect input from previous pipe
      if (i > 0) {
        if (dup2(pipefds[(i - 1) * 2], STDIN_FILENO) < 0) perror("dup2 input");
      }
      // redirect output to current pipe
      if (i < n - 1) {
          if (dup2(pipefds[i * 2 + 1], STDOUT_FILENO) < 0) perror("dup2 output");        
      }

      // close all pipe FDs in the child
      for (int j = 0; j < 2 * (n - 1); j++) {
          close(pipefds[j]);
      }

      execute_child_logic(pipeline[i]);
      exit(0);
    } else if (pid < 0) {
      perror("fork failed");
    } else if (pid > 0) {
      children_pids.push_back(pid);
    }
  }

  // parent must close all its copies of the pipes
  for (int j = 0; j < 2 * (n - 1); j++) {
      close(pipefds[j]);
  }

 if (!pipeline.empty() && pipeline[0].is_background) {
      // reconstruct the full command string: "cmd arg | cmd arg"
      string cmd_str = "";
      for (size_t i = 0; i < pipeline.size(); ++i) {
          cmd_str += pipeline[i].value; // add command name 

          // add all arguments
          for (const auto& child : pipeline[i].children) {
              cmd_str += " " + child.value;
          }

          // add pipe separator if this isn't the last command
          if (i < pipeline.size() - 1) {
              cmd_str += " | ";
          }
      }

      // add the reconstructed string to the jobs list
      jobs.push_back({children_pids.back(), cmd_str, true});
      cout << "[" << jobs.size() << "] " << children_pids.back() << endl;
      
      sigprocmask(SIG_SETMASK, &oldmask, nullptr);
      return; 
  }
  // then wait for the children/foreground
  for (int i = 0; i < n; i++) {
    waitpid(children_pids[i], nullptr, 0);
  }

  // reclaim terminal
  tcsetpgrp(STDIN_FILENO, getpgrp());
  tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_tmodes);
  sigprocmask(SIG_SETMASK, &oldmask, nullptr);
}