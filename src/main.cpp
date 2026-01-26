#include <iostream>
#include <string>
#include <vector>
#include <readline/readline.h>
#include <readline/history.h>

#include "parser.h"
#include "executor.h"
#include "utils.h"

using namespace std;

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
