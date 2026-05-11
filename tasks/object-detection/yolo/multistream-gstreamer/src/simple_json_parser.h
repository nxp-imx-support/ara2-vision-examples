/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: simple_json_parser.h
 * Simple JSON parser for stream configuration - no external dependencies
 */

#ifndef SIMPLE_JSON_PARSER_H
#define SIMPLE_JSON_PARSER_H

#include <string>
#include <map>
#include <vector>
#include <sstream>
#include <algorithm>

class SimpleJsonParser {
public:
  static std::vector<std::map<std::string, std::string>> parse_streams(const std::string& json_content);
  
private:
  static std::string trim(const std::string& str);
  static std::string extract_value(const std::string& line, const std::string& key);
  static std::string remove_quotes(const std::string& str);
};

#endif // SIMPLE_JSON_PARSER_H