#include "parser.h"
#include "utils.h" // Needed because check() calls find_in_path()
#include <algorithm>
#include <cctype>
#include <sstream>
#define ALL(s) (s).begin(), (s).end()
using namespace std;


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

vector<Token> parse(string in) {
  vector<Token> tokens;
  size_t i = 0;
  
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
    } else if (in[i] == '&') {
    tokens.emplace_back(Token{Background, "&"});
    i++;
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

  for (size_t i = 0; i < tokens.size(); i++) {
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
    case Semicolon:
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
    bool background_pipeline = false;

    for (const auto &tok : tokens) {
        if (tok.type == Background) {
            background_pipeline = true;
            break; 
        }
    }

    for (const auto &tok : tokens) {
        if (tok.type == Background || tok.type == Semicolon) {
            break; 
        }

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

    // apply the background flag to every command in this pipeline.
    // eg if we run 'ls | grep .cpp &', both 'ls' and 'grep' belong to the background process group.
    for (auto &ast : pipeline) {
        ast.is_background = background_pipeline;
    }

    return pipeline;
}