#include "utils.h"
#include <iostream>
#include <sstream>
#include <cstdlib>
using namespace std;

vector<Job> jobs;
struct termios shell_tmodes;
vector<string> builtins = {"cd", "exit", "echo", "pwd", "type", "history", "jobs"};
deque<string> manual_history_list;
const size_t MAX_HISTORY = 500;
size_t history_count = 0;

bool peek(const string &s, int (*f)(int), size_t pos) {
  if (pos + 1 < s.size()) {
    return f(s[pos + 1]);
  }

  return false;
}

bool peek(const string &s, bool (*f)(char), size_t pos) {
  if (pos + 1 < s.size()) {
    return f(s[pos + 1]);
  }

  return false;
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

void chdir_logic(string dir) {
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