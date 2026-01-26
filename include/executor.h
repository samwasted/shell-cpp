#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h"
#include <vector>

void execute(const Tree &ast);

void execute_child_logic(const Tree &ast);

void execute_pipeline(const std::vector<Tree> &pipeline);

#endif