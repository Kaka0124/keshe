#ifndef GPU_SCHEDULING_PARSER_H
#define GPU_SCHEDULING_PARSER_H

#include <istream>
#include <string>
#include <utility>
#include <vector>

#include "models.h"

// Parse from an input stream (stdin)
std::pair<std::vector<ServerSpec>, std::vector<Job>> readInstance(std::istream &input);

// Parse from a string (for file-based evaluation)
std::pair<std::vector<ServerSpec>, std::vector<Job>> parseInstance(const std::string &text);

#endif
