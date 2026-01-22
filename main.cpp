#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <string>
#include <vector>
#include <deque>
#define ALL(s) (s).begin(), (s).end()
using namespace std;
namespace fs = filesystem;

deque<string> manual_history_list; 
const size_t MAX_HISTORY = 500;
size_t history_count = 0;
enum TokenT { PlainText, SingleQuoted, Pipe, Semicolon, WhitespaceTk, RedirectOut };

typedef struct Token {
  TokenT type;
  string text;
} Token;

ostream &operator<<(ostream &os, const Token &tok) { // changing the way how cout<< works, to debug token
  os << "TOKEN -> type: ";

  switch (tok.type) {
  case PlainText:
    os << "Plaintext, ";
    break;
  case SingleQuoted:
    os << "SingleQuoted, ";
    break;
  case Pipe:
    os << "Pipe, ";
    break;
  case Semicolon: 
    os << "Semicolon, "; 
    break;
  case WhitespaceTk:
    os << "WhitespaceTk, ";
    break;
  case RedirectOut:
    os << "RedirectOut, ";
    break;
  }
  os << "text: " << tok.text;
  return os;
}

enum TreeT { Builtin, ExecutableFile, TextNode, Leaf, WhitespaceNode };

typedef struct Tree {
  TreeT type;
  string value;
  fs::path path;
  vector<Tree> children;
} Tree;

ostream &operator<<(ostream &os, const Tree &t) {
  os << "{ type: ";

  switch (t.type) {
  case Builtin:
    os << "Builtin, ";
    break;
  case ExecutableFile:
    os << "ExecutableFile, ";
    break;
  case TextNode:
    os << "TextNode, ";
    break;
  case WhitespaceNode:
    os << "WhitespaceNode, ";
    break;
  case Leaf:
    os << "Leaf, ";
    break;
  }

  os << "value: " << t.value << ", children: ";
  for (auto &child : t.children) {
    os << child << ',';
  }

  os << " }";

  return os;
}

vector<filesystem::path> exec_files;
vector<string> builtins = {"cd", "exit", "echo", "pwd", "type", "history"};

fs::path find_in_path(string s);
void execute_pipeline(const vector<Tree> &pipeline);

bool peek(const string &s, int (*f)(int), int pos) {
  if (pos + 1 < s.size()) {
    return f(s[pos + 1]);
  }

  return false;
}

bool peek(const string &s, bool (*f)(char), int pos) {
  if (pos + 1 < s.size()) {
    return f(s[pos + 1]);
  }

  return false;
}

vector<Token> parse(string in) {
  vector<Token> tokens;
  int i = 0;
  
  while (i < in.size()) {
    // skip leading whitespaces
    while (i < in.size() && isspace(in[i])) { 
      i++; 
    }
    
    if (i >= in.size()) break;

    if (in[i] == '|') {
      tokens.emplace_back(Token{Pipe, "|"});
      i++;
      continue;
    }
    else if (in[i] == ';') {
      tokens.emplace_back(Token{Semicolon, ";"});
      i++;
      continue;
    }
    else if (in[i] == '>') {
    if (i + 1 < in.size() && in[i+1] == '>') {
        tokens.emplace_back(Token{RedirectOut, ">>"});
        i += 2;
    } else {
        tokens.emplace_back(Token{RedirectOut, ">"});
        i++;
    }
    continue;
    }   
    else if (isdigit(in[i]) && i + 1 < in.size() && in[i+1] == '>') {
      if (i + 2 < in.size() && in[i+2] == '>') {
          // Handle 1>> or 2>>
          string redirectToken = "";
          redirectToken += in[i];   // '1' or '2'
          redirectToken += ">>";
          tokens.emplace_back(Token{RedirectOut, redirectToken});
          i += 3;
      } else {
          // Handle 1> or 2>
          string redirectToken = "";
          redirectToken += in[i];
          redirectToken += ">";
          tokens.emplace_back(Token{RedirectOut, redirectToken});
          i += 2;
      }
    continue;
}
    string current_argument = "";
    
    while (i < in.size() && !isspace(in[i]) && in[i] != '|' && in[i] != ';') {
      
      if (in[i] == '\\') {
          // backslash outside quotes: skip the '\' and take the next char literally
          i++;
          if (i < in.size()) {
              current_argument += in[i];
              i++;
          }
      } 
      else if (in[i] == '\'') {
        // single quotes: take everything literally until the closing '
        i++;
        while (i < in.size() && in[i] != '\'') {
          current_argument += in[i];
          i++;
        }
        if (i < in.size()) i++; 
      } 
      else if (in[i] == '\"') {
        // double quotes:handle specific escape rules inside "..."
        i++; 
        while (i < in.size() && in[i] != '\"') {
          if (in[i] == '\\' && i + 1 < in.size()) {
            char next = in[i + 1];
            // POSIX rule: only escape if next is ", \, $, or `
            if (next == '\"' || next == '\\' || next == '$' || next == '`') {
              current_argument += next;
              i += 2;
            } else {
              // otherwise, keep the backslash as literal text
              current_argument += in[i];
              i++;
            }
          } else {
            current_argument += in[i];
            i++;
          }
        }
        if (i < in.size()) i++; // skip closing "
      } 
      else {
        current_argument += in[i];
        i++;
      }
    }

    tokens.emplace_back(Token{PlainText, current_argument});
  }
  return tokens;
}
Tree check(vector<Token> &tokens) {
  if (tokens.empty()) return Tree{Leaf, "", {}};

  Tree tree{Leaf, "", {}};
  bool command_found = false;

  for (int i = 0; i < tokens.size(); i++) {
    const Token *cur = &tokens[i];

    switch (cur->type) {
    case PlainText:
    case SingleQuoted: {
      Tree node;

      if (!command_found) {
        if (find(ALL(builtins), cur->text) != builtins.end()) {
          node = {Builtin, cur->text, "", {}};
        } else {
          auto p = find_in_path(cur->text);
          if (!p.empty()) {
            node = {ExecutableFile, cur->text, p, {}};
          } else {
            // it's a command that doesn't exist
            node = {TextNode, cur->text, "", {}};
          }
        }
        tree = node;
        command_found = true;
      } else {
        node = {TextNode, cur->text, "", {}};
        tree.children.emplace_back(node);
      }
    } break;

    case WhitespaceTk:
      // parser handles this
      break;

    case Pipe:
      // handled by build_pipeline_trees()
      break;
    case RedirectOut:
      if (i + 1 < tokens.size()) {
            // use cur->text to capture ">", ">>", "1>>", "2>>"
            tree.children.emplace_back(Tree{TextNode, cur->text, {}}); 
            tree.children.emplace_back(Tree{TextNode, tokens[i+1].text, {}}); 
            i++; 
        }
      break;
    }
  }

  return tree;
}

vector<Tree> build_pipeline_trees(vector<Token> &tokens) {
    vector<Tree> pipeline;
    vector<Token> current_cmd_tokens;

    for (const auto &tok : tokens) {
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
    return pipeline;
}

fs::path find_in_path(string s) {
  if (!s.empty() && s[0] == '~') {
    const char* home = getenv("HOME");
    if (home) {
      // replace ~ with home dir path
      fs::path expanded_path = fs::path(home) / s.substr(1 == s.size() ? 1 : 2); 
      
      if (s == "~") expanded_path = fs::path(home);
      else expanded_path = fs::path(home) / s.substr(2);

      if (fs::exists(expanded_path) && fs::is_regular_file(expanded_path)) {
        return expanded_path;
      }
    }
  }
  // handle both absolute and relative paths
  if (s.find('/') != string::npos) {
      if (fs::exists(s) && fs::is_regular_file(s)) { // ensure its not a directory
          return fs::path(s);
      }
      return fs::path{};
  }

  // 2. Get the current PATH environment variable
  const char *path_env = getenv("PATH");
  if (!path_env) return fs::path{};

  string path_str(path_env);
  stringstream ss(path_str);
  string dir;

  // iterate through each dir in path
  while (getline(ss, dir, ':')) {
    if (dir.empty()) continue; // skip empty entries

    try {
      fs::path full_path = fs::path(dir) / s;

      // check if file exists and is executable
      if (fs::exists(full_path)) {
        auto perms = fs::status(full_path).permissions();
        if ((perms & fs::perms::owner_exec) != fs::perms::none ||
            (perms & fs::perms::group_exec) != fs::perms::none ||
            (perms & fs::perms::others_exec) != fs::perms::none) {
          return full_path;
        }
      }
    } catch (...) {
      // ignore directories we don't have permission to read
      continue;
    }
  }

  return fs::path{};
}
void chdir(string dir) {
  string expanded_dir = dir;

  // tilde expansion logic
  if (!dir.empty() && dir[0] == '~') {
    const char *home = getenv("HOME");
    if (home) {
      if (dir == "~") {
        expanded_dir = home;
      } else if (dir.size() > 1 && (dir[1] == '/' || dir[1] == '\\')) {
        expanded_dir = string(home) + dir.substr(1);
      }
    }
  }

  try {
    if (fs::exists(expanded_dir)) {
      if (fs::is_directory(expanded_dir)) {
        fs::current_path(expanded_dir);
      } else {
        cout << "cd: " << dir << ": Not a directory" << endl;
      }
    } else {
      cout << "cd: " << dir << ": No such file or directory" << endl;
    }
  } catch (const fs::filesystem_error& e) {
    // handle permission errors or other FS issues
    cout << "cd: " << dir << ": Permission denied" << endl;
  }
}

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
    if (out_fd != -1) {
      saved_stdout = dup(STDOUT_FILENO); 
      dup2(out_fd, STDOUT_FILENO);
      close(out_fd);
    }
    if (err_fd != -1) {
      saved_stderr = dup(STDERR_FILENO);
      dup2(err_fd, STDERR_FILENO);
      close(err_fd);
    }

    if (ast.value == "cd") {
      if (!filtered_children.empty()) {
          chdir(filtered_children[0].value);
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
      if (filtered_children.size() == 1) {
        size_t new_i = manual_history_list.size() - stoi(filtered_children[0].value);
        i = max((size_t)0, new_i);
      }
      for (; i < manual_history_list.size(); ++i) {
          cout << "  " << (start_index + i) << "  " << manual_history_list[i] << endl;
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
    pid_t pid = fork();
    if (pid == 0) {
      // no need to backup here because the child will die anyway
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
        
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status)) {
          int code = WEXITSTATUS(status);
          if (code != 0) { 
            cout << "Error code " << code << " encountered" << endl;
          }
        }
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

  // execution Switch
  switch (ast.type) {
  case Builtin: {
    if (ast.value == "cd") {
      if (!filtered_children.empty()) {
          chdir(filtered_children[0].value);
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
          size_t requested = stoi(filtered_children[0].value);
          if (requested < manual_history_list.size()) {
            i = manual_history_list.size() - requested;
          }
          i = max((size_t)0, manual_history_list.size() - requested);
      }

      for (; i < manual_history_list.size(); ++i) {
          cout << "  " << (start_index + i) << "  " << manual_history_list[i] << endl;
      }
    }
    exit(0);
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

  for (int i = 0; i < n; i++) {
    pid_t pid = fork();
    if (pid == 0) {
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
    }
  }

  // parent must close all its copies of the pipes
  for (int j = 0; j < 2 * (n - 1); j++) {
      close(pipefds[j]);
  }

  // then wait for the children
  for (int i = 0; i < n; i++) {
      wait(NULL);
  }
}

int main() {
  std::ios_base::sync_with_stdio(false);
  // flush after every cout / cerr
  cout << unitbuf;
  cerr << unitbuf;

  while (1) {
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
        if (tok.type == Semicolon) {
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

        for (const auto &tok : seq) { // iterate over tokens in the current sequence
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

        // instead of execute(ast), we now run the pipeline coordinator
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
