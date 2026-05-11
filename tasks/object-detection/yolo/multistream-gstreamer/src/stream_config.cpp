/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: stream_config.cpp
 */

#include "stream_config.h"
#include "simple_json_parser.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>

#include <cstdio>
#include <string>

std::string detect_codec_gst(const std::string& filepath) {
    std::string cmd = "gst-discoverer-1.0 " + filepath + " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    char buffer[256];
    std::string output = "";

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    pclose(pipe);

    if (output.find("H.264") != std::string::npos)
        return "h264";

    if (output.find("H.265") != std::string::npos ||
        output.find("HEVC") != std::string::npos)
        return "h265";

    return "unknown";
}

//==============================================================================
// VideoFileConfig Implementation
//==============================================================================

std::string VideoFileConfig::build_gstreamer_source(int stream_id) const {
    std::ostringstream pipeline;

    std::string codec = detect_codec_gst(filepath_);

    if (codec == "h264") {
        pipeline << "multifilesrc loop=true location=" << filepath_ << " ! ";
        pipeline << "h264parse ! v4l2h264dec ";

    } else if (codec == "h265") {

        pipeline << "multifilesrc loop=true location=" << filepath_ << " ! ";
        pipeline << "tsdemux name=demux_" << stream_id << " ";
        pipeline << "demux_" << stream_id << ". ! h265parse ! v4l2h265dec ";

    }

    pipeline << "! imxvideoconvert_g2d ";
    pipeline << "! video/x-raw,width=640,height=360,format=BGRx ";

    pipeline << "! tee name=t" << stream_id << " ";

    return pipeline.str();
}

std::string VideoFileConfig::to_string() const {
  return "VideoFile: " + filepath_;
}

//==============================================================================
// CameraV4L2Config Implementation
//==============================================================================

std::string CameraV4L2Config::build_gstreamer_source(int stream_id) const {
  std::ostringstream pipeline;
  
  pipeline << "v4l2src device=" << device_ 
           << " do-timestamp=true"
           << " name=src_" << stream_id << " ";
  pipeline << "! video/x-raw,width=" << width_ 
           << ",height=" << height_ 
           << ",framerate=" << fps_ << "/1 ";
  pipeline << "! queue max-size-buffers=1 leaky=downstream ";
  pipeline << "! imxvideoconvert_g2d ! video/x-raw,format=BGRx,width=640,height=360 ";
  pipeline << "! videorate ! video/x-raw,framerate=30/1 ";
  pipeline << "! tee name=t" << stream_id << " ";
  
  return pipeline.str();
}

std::string CameraV4L2Config::to_string() const {
  std::ostringstream ss;
  ss << "V4L2 Camera: " << device_ 
     << " (" << width_ << "x" << height_ << " @ " << fps_ << " fps)";
  return ss.str();
}

//==============================================================================
// RTSPConfig Implementation
//==============================================================================
std::string RTSPConfig::build_gstreamer_source(int stream_id) const {
  std::ostringstream pipeline;
  
  pipeline << "rtspsrc location=" << url_ 
            << " latency=" << latency_ms_;
  
  if (use_tcp_) {
    pipeline << " protocols=tcp";
  } else {
    pipeline << " protocols=udp";
  } 
  
  
  pipeline << " name=src_" << stream_id << " ";
  
  // Add caps filter to specify the media type
  pipeline << "! application/x-rtp,media=video,encoding-name=H264 ";
  
  pipeline << "! rtph264depay ! h264parse ! v4l2h264dec ";
  pipeline << "! imxvideoconvert_g2d ! video/x-raw,format=BGRx,width=640,height=360 ";
  pipeline << "! tee name=t" << stream_id << " ";
  
  return pipeline.str();
}
std::string RTSPConfig::to_string() const {
  std::ostringstream ss;
  ss << "RTSP: " << url_ 
     << " (latency=" << latency_ms_ << "ms, " 
     << (use_tcp_ ? "TCP" : "UDP") << ")";
  return ss.str();
}

//==============================================================================
// TestPatternConfig Implementation
//==============================================================================

std::string TestPatternConfig::build_gstreamer_source(int stream_id) const {
  std::ostringstream pipeline;
  
  const char* pattern_name = "smpte";
  switch (pattern_) {
    case 1: pattern_name = "snow"; break;
    case 2: pattern_name = "black"; break;
    case 3: pattern_name = "white"; break;
    case 4: pattern_name = "red"; break;
    case 5: pattern_name = "green"; break;
    case 6: pattern_name = "blue"; break;
    default: pattern_name = "smpte"; break;
  }
  
  pipeline << "videotestsrc pattern=" << pattern_name 
           << " name=src_" << stream_id << " ";
  pipeline << "! video/x-raw,width=" << width_ 
           << ",height=" << height_ << ",framerate=30/1 ";
  pipeline << "! videoconvert ! video/x-raw,format=BGRx ";
  pipeline << "! tee name=t" << stream_id << " ";
  
  return pipeline.str();
}

std::string TestPatternConfig::to_string() const {
  std::ostringstream ss;
  ss << "Test Pattern: pattern=" << pattern_ 
     << " (" << width_ << "x" << height_ << ")";
  return ss.str();
}

//==============================================================================
// StreamConfigManager Implementation
//==============================================================================

void StreamConfigManager::add_stream(std::unique_ptr<StreamSourceConfig> config) {
  streams_.push_back(std::move(config));
}

const StreamSourceConfig* StreamConfigManager::get_stream(size_t index) const {
  if (index < streams_.size()) {
    return streams_[index].get();
  }
  return nullptr;
}

void StreamConfigManager::print_summary() const {
  std::cout << "\n========================================\n"
            << "  Stream Configuration Summary\n"
            << "========================================\n"
            << "Total Streams: " << streams_.size() << "\n\n";
  
  for (size_t i = 0; i < streams_.size(); ++i) {
    std::cout << "[Stream " << i << "] " << streams_[i]->to_string() << "\n";
  }
  
  std::cout << "========================================\n" << std::endl;
}

bool StreamConfigManager::load_from_json(const std::string& filepath) {
  try {
    std::ifstream file(filepath);
    if (!file.is_open()) {
      std::cerr << "Error: Cannot open config file: " << filepath << std::endl;
      return false;
    }

    // Read entire file content
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_content = buffer.str();

    // Parse using simple parser
    auto stream_configs = SimpleJsonParser::parse_streams(json_content);

    clear();

    for (const auto& stream : stream_configs) {
      auto type_it = stream.find("type");
      if (type_it == stream.end()) continue;

      std::string type = type_it->second;

      if (type == "video_file") {
        auto filepath_it = stream.find("filepath");
        if (filepath_it != stream.end() && !filepath_it->second.empty()) {
          add_stream(std::make_unique<VideoFileConfig>(filepath_it->second));
        }
      } else if (type == "camera_v4l2") {
        std::string device = "/dev/video0";
        int width = 640;
        int height = 360;
        int fps = 30;

        auto device_it = stream.find("device");
        if (device_it != stream.end()) device = device_it->second;

        auto width_it = stream.find("width");
        if (width_it != stream.end()) width = std::stoi(width_it->second);

        auto height_it = stream.find("height");
        if (height_it != stream.end()) height = std::stoi(height_it->second);

        auto fps_it = stream.find("fps");
        if (fps_it != stream.end()) fps = std::stoi(fps_it->second);

        add_stream(std::make_unique<CameraV4L2Config>(device, width, height, fps));
      } else if (type == "rtsp") {
        auto url_it = stream.find("url");
        if (url_it != stream.end() && !url_it->second.empty()) {
          int latency = 200;
          bool use_tcp = false;

          auto latency_it = stream.find("latency_ms");
          if (latency_it != stream.end()) latency = std::stoi(latency_it->second);

          auto tcp_it = stream.find("use_tcp");
          if (tcp_it != stream.end()) {
            use_tcp = (tcp_it->second == "true" || tcp_it->second == "1");
          }

          add_stream(std::make_unique<RTSPConfig>(url_it->second, latency, use_tcp));
        }
      } else if (type == "test_pattern") {
        int pattern = 0;
        int width = 640;
        int height = 360;

        auto pattern_it = stream.find("pattern");
        if (pattern_it != stream.end()) pattern = std::stoi(pattern_it->second);

        auto width_it = stream.find("width");
        if (width_it != stream.end()) width = std::stoi(width_it->second);

        auto height_it = stream.find("height");
        if (height_it != stream.end()) height = std::stoi(height_it->second);

        add_stream(std::make_unique<TestPatternConfig>(pattern, width, height));
      }
    }

    return true;
  } catch (const std::exception& e) {
    std::cerr << "Error parsing JSON config: " << e.what() << std::endl;
    return false;
  }
}

bool StreamConfigManager::save_to_json(const std::string& filepath) const {
  std::cerr << "Error: JSON save not implemented in simple parser" << std::endl;
  return false;
}
