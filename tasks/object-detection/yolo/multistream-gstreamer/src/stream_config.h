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
#include <cstdint>
#include <mutex>

/**
 * @brief Enum representing different stream source types
 */
enum class StreamSourceType {
  VIDEO_FILE,
  CAMERA_V4L2,
  RTSP,
  CAMERA_LIBCAMERA
};

/**
 * @brief Compute the number of recording segments to retain.
 *
 * Returns ceil(retention_minutes * 60 / segment_seconds), clamped to a minimum
 * of 1. Rounding up ensures non-divisible combinations do not silently
 * under-retain by up to one segment. A non-positive segment_seconds falls back
 * to 60. This single helper is shared by the splitmuxsink "max-files" property
 * and the application-side retention janitor so the two can never diverge.
 */
int compute_retention_max_files(int retention_minutes, int segment_seconds);

// Internal codec-detection helpers — implementation in stream_config.cpp.
// Declared here only because get_codec() inline bodies in this header call
// them; they are not part of the public API.
std::string detect_codec_gst_impl(const std::string& filepath);
std::string detect_codec_rtsp_sdp_impl(const std::string& url,
                                        int timeout_sec = 3);

/**
 * @brief Base class for stream source configuration
 */
class StreamSourceConfig {
 public:
  virtual ~StreamSourceConfig() = default;
  virtual StreamSourceType get_type() const = 0;
  virtual std::string build_gstreamer_source(int stream_id) const = 0;
  virtual std::string to_string() const = 0;
  // Returns a short codec label for display (e.g. "h264", "h265", "raw", "unknown")
  virtual std::string get_codec() const = 0;
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

  const std::string& get_filepath() const {
    return filepath_;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;
  // Returns cached codec. Thread-safe: detection runs exactly once via call_once.
  std::string get_codec() const override {
    std::call_once(codec_once_, [this]() {
      codec_cache_ = detect_codec_gst_impl(filepath_);
    });
    return codec_cache_;
  }

 private:
  std::string filepath_;
  mutable std::once_flag codec_once_;
  mutable std::string codec_cache_;
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
  std::string get_codec() const override { return "raw"; }

  int get_fps() const { return fps_; }
  // The source caps pin these exact dimensions, so unlike a file or an RTSP
  // camera there is nothing to probe: this IS the stream's initial
  // resolution. Exposed so the summary table can report it instead of
  // "unknown".
  int get_width() const { return width_; }
  int get_height() const { return height_; }

 private:
  std::string device_;
  int width_;
  int height_;
  int fps_;
};

class CameraLibcameraConfig : public StreamSourceConfig {
 public:
  CameraLibcameraConfig(const std::string& camera_id = "",
                        bool record = false,
                        const std::string& record_dir =
                            "/var/lib/ara2-vision-examples/recordings",
                        int segment_seconds = 60,
                        int max_files = 10,
                        bool rtsp_out = false,
                        int rtsp_port = 8554)
      : camera_id_(camera_id), record_(record), record_dir_(record_dir),
        segment_seconds_(segment_seconds), max_files_(max_files),
        rtsp_out_(rtsp_out), rtsp_port_(rtsp_port) {}

  StreamSourceType get_type() const override {
    return StreamSourceType::CAMERA_LIBCAMERA;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;
  std::string get_codec() const override { return "raw"; }

  bool is_recording_enabled() const { return record_; }
  const std::string& get_record_dir() const { return record_dir_; }
  int get_segment_seconds() const { return segment_seconds_ > 0 ? segment_seconds_ : 60; }
  int get_max_files() const { return max_files_ > 0 ? max_files_ : 10; }
  bool is_rtsp_out_enabled() const { return rtsp_out_; }
  int get_rtsp_port() const { return rtsp_port_ > 0 ? rtsp_port_ : 8554; }

 private:
  std::string camera_id_;
  bool record_;
  std::string record_dir_;
  int segment_seconds_;
  int max_files_;
  bool rtsp_out_;
  int rtsp_port_;
};

/**
 * @brief Configuration for RTSP sources
 */
class RTSPConfig : public StreamSourceConfig {
 public:
  // tcp_timeout_us: rtspsrc tcp-timeout and timeout in microseconds (default 5 s).
  RTSPConfig(const std::string& url, int latency_ms = 100,
             bool use_tcp = true, uint64_t tcp_timeout_us = 5000000,
             bool record = false,
             const std::string& record_url = "",
             const std::string& record_dir =
                 "/var/lib/ara2-vision-examples/recordings",
             int segment_seconds = 60, int retention_minutes = 30)
      : url_(url), latency_ms_(latency_ms), use_tcp_(use_tcp),
        tcp_timeout_us_(tcp_timeout_us),
        record_(record), record_url_(record_url), record_dir_(record_dir),
        segment_seconds_(segment_seconds),
        retention_minutes_(retention_minutes) {}

  StreamSourceType get_type() const override {
    return StreamSourceType::RTSP;
  }

  std::string build_gstreamer_source(int stream_id) const override;
  std::string to_string() const override;

  // Transport accessors. Needed so the pre-flight resolution probe can be
  // driven with the SAME transport settings the real pipeline uses: on a
  // TCP-only network a default-UDP probe simply times out, and the app would
  // then fall back to a placeholder resolution.
  const std::string& get_url() const { return url_; }
  int get_latency_ms() const { return latency_ms_; }
  bool uses_tcp() const { return use_tcp_; }
  uint64_t get_transport_timeout_us() const { return tcp_timeout_us_; }

  // Returns cached codec detected from the inference URL via RTSP DESCRIBE/SDP.
  // Thread-safe: detection runs exactly once via call_once.
  std::string get_codec() const override {
    std::call_once(codec_once_, [this]() {
      codec_cache_ = detect_codec_rtsp_sdp_impl(url_);
    });
    return codec_cache_;
  }

  // Recording configuration accessors. Recording is active only when
  // recording is requested AND a dedicated high-resolution record URL is
  // configured (recording never uses the low-resolution inference URL).
  bool is_recording_enabled() const {
    return record_ && !record_url_.empty();
  }

  const std::string& get_record_url() const { return record_url_; }
  const std::string& get_record_dir() const { return record_dir_; }
  int get_segment_seconds() const { return segment_seconds_; }
  int get_retention_minutes() const { return retention_minutes_; }

  // Build a standalone recording sub-pipeline that pulls the dedicated
  // high-resolution H.265 record URL directly (independent rtspsrc), depays
  // and parses it without decoding, and writes bounded, rolling MP4 segments.
  // Returns an empty string when recording is not enabled or the configured
  // URLs/paths fail validation.
  std::string build_recording_branch(int stream_id) const;

 private:
  std::string url_;
  int latency_ms_;
  bool use_tcp_;
  uint64_t tcp_timeout_us_;  ///< rtspsrc tcp-timeout / timeout (us)
  bool record_;
  std::string record_url_;
  std::string record_dir_;
  int segment_seconds_;
  int retention_minutes_;
  mutable std::once_flag codec_once_;
  mutable std::string codec_cache_;
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

  StreamSourceType get_stream_type(int stream_id) const;

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
