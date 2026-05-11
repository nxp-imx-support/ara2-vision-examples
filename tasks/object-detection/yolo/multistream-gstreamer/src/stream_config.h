/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: stream_config.h
 */

#ifndef STREAM_CONFIG_H
#define STREAM_CONFIG_H

#include <string>
#include <vector>
#include <memory>
#include <map>

/**
 * @brief Enum representing different stream source types
 */
enum class StreamSourceType {
  VIDEO_FILE,
  CAMERA_V4L2,
  RTSP,
  TEST_PATTERN
};

/**
 * @brief Base class for stream source configuration
 */
class StreamSourceConfig {
 public:
  virtual ~StreamSourceConfig() = default;
  virtual StreamSourceType get_type() const = 0;
  virtual std::string build_gstreamer_source(int stream_id) const = 0;
  virtual std::string to_string() const = 0;
};

/**
 * @brief Configuration for video file sources
 */
class VideoFileConfig : public StreamSourceConfig {
 public:
  explicit VideoFileConfig(const std::string& filepath)
      : filepath_(filepath) {}

  StreamSourceType get_type() const override {
    return StreamSourceType::VIDEO_FILE;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;

 private:
  std::string filepath_;
};

/**
 * @brief Configuration for V4L2 camera sources
 */
class CameraV4L2Config : public StreamSourceConfig {
 public:
  CameraV4L2Config(const std::string& device, int width = 640,
                   int height = 360, int fps = 30)
      : device_(device), width_(width), height_(height), fps_(fps) {}

  StreamSourceType get_type() const override {
    return StreamSourceType::CAMERA_V4L2;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;

 private:
  std::string device_;
  int width_;
  int height_;
  int fps_;
};

/**
 * @brief Configuration for RTSP sources
 */
class RTSPConfig : public StreamSourceConfig {
 public:
  RTSPConfig(const std::string& url, int latency_ms = 100,
             bool use_tcp = true)
      : url_(url), latency_ms_(latency_ms), use_tcp_(use_tcp) {}

  StreamSourceType get_type() const override {
    return StreamSourceType::RTSP;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;

 private:
  std::string url_;
  int latency_ms_;
  bool use_tcp_;
};

/**
 * @brief Configuration for test pattern sources (useful for debugging)
 */
class TestPatternConfig : public StreamSourceConfig {
 public:
  TestPatternConfig(int pattern = 0, int width = 640, int height = 360)
      : pattern_(pattern), width_(width), height_(height) {}

  StreamSourceType get_type() const override {
    return StreamSourceType::TEST_PATTERN;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;

 private:
  int pattern_;  // 0=smpte, 1=snow, 2=black, etc.
  int width_;
  int height_;
};

/**
 * @brief Manager class for stream configurations
 */
class StreamConfigManager {
 public:
  StreamConfigManager() = default;

  /**
   * @brief Add a stream source configuration
   */
  void add_stream(std::unique_ptr<StreamSourceConfig> config);

  /**
   * @brief Get number of configured streams
   */
  size_t get_stream_count() const { return streams_.size(); }

  /**
   * @brief Get stream configuration by index
   */
  const StreamSourceConfig* get_stream(size_t index) const;

  /**
   * @brief Load configuration from JSON file
   */
  bool load_from_json(const std::string& filepath);

  /**
   * @brief Save configuration to JSON file
   */
  bool save_to_json(const std::string& filepath) const;

  /**
   * @brief Clear all stream configurations
   */
  void clear() { streams_.clear(); }

  /**
   * @brief Print all configured streams
   */
  void print_summary() const;

 private:
  std::vector<std::unique_ptr<StreamSourceConfig>> streams_;
};

#endif  // STREAM_CONFIG_H