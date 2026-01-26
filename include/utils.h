#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <deque>
#include <filesystem>
#include <sys/types.h>
#include <termios.h>
namespace fs = std::filesystem;

struct Job {
    pid_t pid;
    std::string command;
    bool is_running;
};

extern struct termios shell_tmodes;
extern std::vector<Job> jobs;
extern std::vector<std::string> builtins;
extern std::deque<std::string> manual_history_list;
extern const size_t MAX_HISTORY;
extern size_t history_count;

fs::path find_in_path(std::string s);

void chdir_logic(std::string dir);

bool peek(const std::string &s, int (*f)(int), int pos);
bool peek(const std::string &s, bool (*f)(char), int pos);

#endif