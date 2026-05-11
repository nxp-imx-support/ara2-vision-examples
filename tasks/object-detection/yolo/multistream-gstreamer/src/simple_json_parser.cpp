/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: simple_json_parser.cpp
 */

#include "simple_json_parser.h"
#include <fstream>
#include <iostream>

std::string SimpleJsonParser::trim(const std::string& str) {
  size_t first = str.find_first_not_of(" \t\n\r");
  if (first == std::string::npos) return "";
  size_t last = str.find_last_not_of(" \t\n\r,");
  return str.substr(first, (last - first + 1));
}

std::string SimpleJsonParser::remove_quotes(const std::string& str) {
  std::string result = trim(str);
  if (result.length() >= 2 && result.front() == '"' && result.back() == '"') {
    return result.substr(1, result.length() - 2);
  }
  return result;
}

std::string SimpleJsonParser::extract_value(const std::string& line, const std::string& key) {
  size_t pos = line.find("\"" + key + "\"");
  if (pos == std::string::npos) return "";
  
  size_t colon = line.find(':', pos);
  if (colon == std::string::npos) return "";
  
  size_t value_start = colon + 1;
  size_t value_end = line.find_first_of(",}", value_start);
  if (value_end == std::string::npos) value_end = line.length();
  
  return remove_quotes(line.substr(value_start, value_end - value_start));
}

std::vector<std::map<std::string, std::string>> SimpleJsonParser::parse_streams(const std::string& json_content) {
  std::vector<std::map<std::string, std::string>> streams;
  std::istringstream iss(json_content);
  std::string line;
  
  bool in_streams_array = false;
  bool in_stream_object = false;
  std::map<std::string, std::string> current_stream;
  
  while (std::getline(iss, line)) {
    std::string trimmed = trim(line);
    
    // Check for "streams" array start
    if (trimmed.find("\"streams\"") != std::string::npos) {
      in_streams_array = true;
      continue;
    }
    
    // Check for stream object start
    if (in_streams_array && trimmed == "{") {
      in_stream_object = true;
      current_stream.clear();
      continue;
    }
    
    // Check for stream object end
    if (in_stream_object && (trimmed == "}" || trimmed == "},")) {
      if (!current_stream.empty()) {
        streams.push_back(current_stream);
      }
      in_stream_object = false;
      continue;
    }
    
    // Parse key-value pairs
    if (in_stream_object && trimmed.find(':') != std::string::npos) {
      size_t colon = trimmed.find(':');
      std::string key = remove_quotes(trimmed.substr(0, colon));
      std::string value = remove_quotes(trimmed.substr(colon + 1));
      
      if (!key.empty()) {
        current_stream[key] = value;
      }
    }
  }
  
  return streams;
}