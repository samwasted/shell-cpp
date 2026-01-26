#ifndef PARSER_H
#define PARSER_H

#include <string>
#include <vector>
#include <deque>
#include <iostream>
#include <filesystem>
namespace fs = std::filesystem;

enum TokenT { PlainText, SingleQuoted, Pipe, Semicolon, WhitespaceTk, RedirectOut, Background };

typedef struct Token {
  TokenT type;
  std::string text;
} Token;
enum TreeT { Builtin, ExecutableFile, TextNode, Leaf, WhitespaceNode };

typedef struct Tree {
  TreeT type;
  std::string value;
  fs::path path;
  std::vector<Tree> children;
  bool is_background = false;
} Tree;


std::ostream &operator<<(std::ostream &os, const Token &tok);

std::ostream &operator<<(std::ostream &os, const Tree &t);

std::vector<Token> parse(std::string in);

Tree check(std::vector<Token> &tokens);

std::vector<Tree> build_pipeline_trees(std::vector<Token> &tokens);

#endif