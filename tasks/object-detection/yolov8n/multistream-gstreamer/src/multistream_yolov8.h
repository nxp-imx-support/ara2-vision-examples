/*
 * Copyright (c) 2025, Kinara, Inc. All rights reserved.
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: multistream_yolov8.h
 */

#ifndef MULTISTREAM_YOLOV8_H
#define MULTISTREAM_YOLOV8_H

// System headers
#include <cairo.h>
#include <glib.h>
#include <gst/gst.h>

// Standard C++ headers
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Constants
namespace {
constexpr guint64 TIMESTAMP_TOLERANCE = 100000000;  // ~100ms in nanoseconds
}  // namespace

// COCO class names (80 classes)
const std::vector<std::string> COCO_CLASSES = {
    "person",        "bicycle",      "car",
    "motorcycle",    "airplane",     "bus",
    "train",         "truck",        "boat",
    "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench",        "bird",
    "cat",           "dog",          "horse",
    "sheep",         "cow",          "elephant",
    "bear",          "zebra",        "giraffe",
    "backpack",      "umbrella",     "handbag",
    "tie",           "suitcase",     "frisbee",
    "skis",          "snowboard",    "sports ball",
    "kite",          "baseball bat", "baseball glove",
    "skateboard",    "surfboard",    "tennis racket",
    "bottle",        "wine glass",   "cup",
    "fork",          "knife",        "spoon",
    "bowl",          "banana",       "apple",
    "sandwich",      "orange",       "broccoli",
    "carrot",        "hot dog",      "pizza",
    "donut",         "cake",         "chair",
    "couch",         "potted plant", "bed",
    "dining table",  "toilet",       "tv",
    "laptop",        "mouse",        "remote",
    "keyboard",      "cell phone",   "microwave",
    "oven",          "toaster",      "sink",
    "refrigerator",  "book",         "clock",
    "vase",          "scissors",     "teddy bear",
    "hair drier",    "toothbrush"};

// Helper function to get class name
inline std::string get_class_name(int class_id) {
  if (class_id >= 0 && class_id < static_cast<int>(COCO_CLASSES.size())) {
    return COCO_CLASSES[class_id];
  }
  return "unknown";
}

// Structure to hold detection box data for rendering
struct DetectionBox {
  float x1;
  float y1;
  float x2;
  float y2;
  float score;
  int class_id;
  int track_id;
};

// Structure matching ara_detection_t from ara_inference_api.h
// NOTE: class_name pointer is not valid after serialization, so we ignore it
struct AraDetection {
  float xmin;               // offset 0, 4 bytes
  float ymin;               // offset 4, 4 bytes
  float xmax;               // offset 8, 4 bytes
  float ymax;               // offset 12, 4 bytes
  float confidence;         // offset 16, 4 bytes
  int32_t class_id;         // offset 20, 4 bytes
  void* class_name_ptr;     // offset 24, 8 bytes (ignored - pointer not valid)
} __attribute__((packed));  // Total: 32 bytes on 64-bit systems

/**
 * @brief Handles detection visualization for a video stream
 */
class Detection {
 public:
  explicit Detection(int stream_id);
  ~Detection();

  // Non-copyable
  Detection(const Detection&) = delete;
  Detection& operator=(const Detection&) = delete;

  // Movable
  Detection(Detection&&) = default;
  Detection& operator=(Detection&&) = default;

  /**
   * @brief Start detection processing for the given pipeline
   * @param pipeline GStreamer pipeline element
   */
  void start(GstElement* pipeline);

 private:
  // Stream identification
  int stream_id_;

  // GStreamer elements
  GstElement* dv_post_sink_;
  GstElement* draw_detections_;

  // Rendering parameters
  int line_width_;
  int text_size_;

  // FPS tracking
  double curr_fps_;
  double avg_fps_;
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_fps_update_;
  uint64_t frame_count_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>>
      fps_samples_;
  std::mutex fps_mutex_;

  // IPS (Inferences Per Second) tracking
  double curr_ips_;
  double avg_ips_;
  uint64_t inference_count_;
  std::chrono::steady_clock::time_point last_ips_update_;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>>
      ips_samples_;
  std::mutex ips_mutex_;

  // Detection data
  std::vector<std::vector<DetectionBox>> online_targets_;
  std::map<guint64, std::vector<DetectionBox>> timestamped_detections_;
  std::mutex targets_mutex_;

  // Video capabilities
  GstCaps* video_caps_;

  // Static members
  static bool display_warning_;
  static std::mutex warning_mutex_;

  // Private methods
  void calculate_fps();
  void calculate_ips();
  GstFlowReturn new_detection(GstElement* sink);
  void prepare_overlay(GstCaps* caps);
  void draw_overlay(cairo_t* cr, guint64 timestamp);
  static int get_terminal_columns();

  // GStreamer callback functions
  static GstFlowReturn new_detection_callback(GstElement* sink,
                                              gpointer user_data);
  static void prepare_overlay_callback(GstElement* overlay, GstCaps* caps,
                                       gpointer user_data);
  static void draw_overlay_callback(GstElement* overlay, cairo_t* cr,
                                    guint64 timestamp, guint64 duration,
                                    gpointer user_data);
};

/**
 * @brief Main demo application class managing multiple video streams
 */
class Demo {
 public:
  Demo(int streams, int endpoint, const std::string& group, bool enable_perf_stats = false, bool sync = false, const std::string& model = "/usr/share/cnn/detection/yolov8n/model.dvm");
  ~Demo();

  // Non-copyable
  Demo(const Demo&) = delete;
  Demo& operator=(const Demo&) = delete;

  // Movable
  Demo(Demo&&) = default;
  Demo& operator=(Demo&&) = default;

  /**
   * @brief Start the demo application
   * @return true if started successfully, false otherwise
   */
  bool start();

 private:
  // Configuration
  int streams_;
  int endpoint_;
  std::string group_;
  bool sync_;
  std::string model_;
  std::string sock_;
  int video_width_;
  int video_height_;
  bool enable_perf_stats_;

  // GStreamer components
  GstElement* pipeline_;
  GMainLoop* main_loop_;

  // Detection handlers
  std::vector<std::unique_ptr<Detection>> detections_;

  // Private methods
  std::string build_compositor(int number_of_streams);
  void on_bus_message(GstMessage* message);
  void set_fullscreen_when_ready();
  void cleanup();

  // GStreamer callback function
  static gboolean on_bus_message_callback(GstBus* bus, GstMessage* message,
                                          gpointer user_data);
};

/**
 * @brief Print usage information
 * @param prog_name Program name
 */
void print_usage(const char* prog_name);

#endif  // MULTISTREAM_YOLOV8_H
