/*
 * Copyright (c) 2025, Kinara, Inc. All rights reserved.
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: multistream_yolo.cpp
 */

#include "multistream_yolo.h"
// System headers
#include <gst/video/video.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <dirent.h>

// Standard C++ headers
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <gst/gst.h>
#include <dlfcn.h>
#include <cstring>
#include <dvapi.h>

//==============================================================================
// Constants
//==============================================================================

static const std::string SAMPLE_VIDEOS_DIR =
    "/usr/share/ara2-vision-examples/sample_videos";

//==============================================================================
// Helper Functions
//==============================================================================

/**
 * @brief Check if a file exists
 * @param filepath Path to the file to check
 * @return true if file exists, false otherwise
 */
bool file_exists(const std::string& filepath) {
  struct stat buffer;
  return (stat(filepath.c_str(), &buffer) == 0);
}

/**
 * @brief Validate that the model file exists
 * @param model_path Path to the model file
 * @return true if model exists, false otherwise
 */
bool validate_model_exists(const std::string& model_path) {
  if (!file_exists(model_path)) {
    std::cerr << "\n========================================\n"
              << "      ERROR: Model Not Found\n"
              << "========================================\n"
              << "The specified model file does not exist:\n"
              << "  " << model_path << "\n\n"
              << "  To download the required models:\n\n"
              << "1. Download all YOLOv8 models:\n"
              << "   fetch_models --repo-id nxp/YOLOv8\n\n"
              << "2. Download all YOLOX models:\n"
              << "   fetch_models --repo-id nxp/YOLOX\n\n"
              << "3. Verify installation:\n"
              << "   ls -lh /usr/share/cnn/detection/yolov8*/\n"
              << "   ls -lh /usr/share/cnn/detection/yolox*/\n\n"
              << "  Available YOLOv8 model variants:\n"
              << "   - yolov8n (nano)        - Fastest, lowest accuracy\n"
              << "   - yolov8s (small)       - Balanced speed and accuracy\n"
              << "   - yolov8m (medium)      - Good accuracy, moderate speed\n"
              << "   - yolov8l (large)       - High accuracy, slower\n"
              << "   - yolov8x (extra-large) - Highest accuracy, slowest\n\n"
              << "  Available YOLOX model variants:\n"
              << "   - yoloxs (small)        - Balanced speed and accuracy\n"
              << "   - yoloxm (medium)       - Good accuracy, moderate speed\n"
              << "   - yoloxl (large)        - High accuracy, slower\n"
              << "   - yoloxx (extra-large)  - Highest accuracy, slowest\n\n"
              << "========================================\n"
              << std::endl;
    return false;
  }
  return true;
}

/**
 * @brief Discover MP4 video files in a directory
 * @param directory Path to scan for .mp4 files
 * @return Sorted vector of full file paths (up to 8 files)
 */
std::vector<std::string> discover_video_files(const std::string& directory) {
  std::vector<std::string> video_files;

  DIR* dir = opendir(directory.c_str());
  if (!dir) {
    std::cerr << "Warning: Cannot open directory: " << directory << "\n";
    return video_files;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string filename(entry->d_name);

    // Skip hidden files
    if (filename.empty() || filename[0] == '.') {
      continue;
    }

    // Check for .mp4 extension (case-insensitive)
    if (filename.length() >= 4) {
      std::string ext = filename.substr(filename.length() - 4);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".mp4") {
        std::string full_path = directory + "/" + filename;
        // Verify it's a regular file
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
          video_files.push_back(full_path);
        }
      }
    }
  }
  closedir(dir);

  // Sort for deterministic, reproducible ordering
  std::sort(video_files.begin(), video_files.end());

  // Cap at 8 files maximum
  if (video_files.size() > 8) {
    video_files.resize(8);
  }

  return video_files;
}

//==============================================================================
// Static Member Initialization
//==============================================================================

bool Detection::display_warning_ = true;
std::mutex Detection::warning_mutex_;

//==============================================================================
// Detection Class Implementation
//==============================================================================

Detection::Detection(int stream_id)
    : stream_id_(stream_id),
      dv_post_sink_(nullptr),
      draw_detections_(nullptr),
      line_width_(5),
      text_size_(25),
      curr_fps_(0.0),
      avg_fps_(0.0),
      curr_ips_(0.0),
      avg_ips_(0.0),
      frame_count_(0),
      inference_count_(0),
      video_caps_(nullptr),
      no_bbox_(false),
      no_osd_stats_(false),
      only_bbox_(false),
      console_interval_(2) {
  const auto now = std::chrono::steady_clock::now();
  start_time_ = now;
  last_fps_update_ = now;
  last_ips_update_ = now;
  last_console_print_ = now;
}

void Detection::print_stats_to_console() {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - last_console_print_).count();

  if (console_interval_ == 0) {
      return;
  }
  if (elapsed >= console_interval_) {
      double current_ips;
    {
      std::lock_guard<std::mutex> lock(ips_mutex_);
      current_ips = avg_ips_;
    }

    if (stats_callback_) {
      stats_callback_();
    }

    last_console_print_ = now;
  }
}

void Detection::set_no_bbox(bool no_bbox) {
  no_bbox_ = no_bbox;
}

void Detection::set_no_osd_stats(bool no_osd_stats) {
  no_osd_stats_ = no_osd_stats;
}

void Detection::set_only_bbox(bool only_bbox) {
  only_bbox_ = only_bbox;
}

void Detection::set_stats_callback(std::function<void()> callback) {
  stats_callback_ = callback;
}

void Detection::set_console_interval(int interval) {
 console_interval_ = interval;
}

Detection::~Detection() {
  if (video_caps_) {
    gst_caps_unref(video_caps_);
    video_caps_ = nullptr;
  }
  if (dv_post_sink_) {
    gst_object_unref(dv_post_sink_);
    dv_post_sink_ = nullptr;
  }
  if (draw_detections_) {
    gst_object_unref(draw_detections_);
    draw_detections_ = nullptr;
  }
}

void Detection::start(GstElement* pipeline) {
  if (!pipeline) {
    g_printerr("Pipeline is null\n");
    return;
  }

  // Get dvPost sink (appsink)
  const std::string sink_name = "dv_post_sink_" + std::to_string(stream_id_);
  dv_post_sink_ = gst_bin_get_by_name(GST_BIN(pipeline), sink_name.c_str());
  if (!dv_post_sink_) {
    g_printerr("Failed to get element: %s\n", sink_name.c_str());
    return;
  }

  g_signal_connect(dv_post_sink_, "new-sample",
                   G_CALLBACK(new_detection_callback), this);

  // Get cairo overlay element
  const std::string draw_name = "draw_detections_" + std::to_string(stream_id_);
  draw_detections_ = gst_bin_get_by_name(GST_BIN(pipeline), draw_name.c_str());
  if (!draw_detections_) {
    g_printerr("Failed to get element: %s\n", draw_name.c_str());
    return;
  }

  g_signal_connect(draw_detections_, "draw", G_CALLBACK(draw_overlay_callback),
                   this);
  g_signal_connect(draw_detections_, "caps-changed",
                   G_CALLBACK(prepare_overlay_callback), this);
}

//==============================================================================
// FPS and IPS Calculation
//==============================================================================

void Detection::calculate_fps() {
  std::lock_guard<std::mutex> lock(fps_mutex_);

  const auto now = std::chrono::steady_clock::now();
  ++frame_count_;

  // Calculate instantaneous FPS
  const std::chrono::duration<double> frame_time = now - last_fps_update_;
  const double frame_seconds = frame_time.count();

  if (frame_seconds > 0.0) {
    curr_fps_ = 1.0 / frame_seconds;
  }

  last_fps_update_ = now;

  // Add current sample to the window
  fps_samples_.push_back({now, frame_count_});

  // Keep only samples from the last second
  while (!fps_samples_.empty()) {
    const std::chrono::duration<double> age = now - fps_samples_.front().first;
    if (age.count() > 1.0) {
      fps_samples_.pop_front();
    } else {
      break;
    }
  }

  // Calculate average FPS over the last second
  if (fps_samples_.size() >= 2) {
    const auto& oldest = fps_samples_.front();
    const auto& newest = fps_samples_.back();

    const std::chrono::duration<double> time_span = newest.first - oldest.first;
    const uint64_t frame_span = newest.second - oldest.second;

    if (time_span.count() > 0.0) {
      avg_fps_ = std::round(frame_span / time_span.count() * 10.0) / 10.0;
    }
  } else {
    // Not enough samples yet, use instantaneous FPS
    avg_fps_ = std::round(curr_fps_ * 10.0) / 10.0;
  }
}

void Detection::calculate_ips() {
  std::lock_guard<std::mutex> lock(ips_mutex_);

  const auto now = std::chrono::steady_clock::now();
  ++inference_count_;

  // Calculate instantaneous IPS
  const std::chrono::duration<double> inference_time = now - last_ips_update_;
  const double inference_seconds = inference_time.count();

  if (inference_seconds > 0.0) {
    curr_ips_ = 1.0 / inference_seconds;
  }

  last_ips_update_ = now;

  // Add current sample to the window
  ips_samples_.push_back({now, inference_count_});

  // Keep only samples from the last second
  while (!ips_samples_.empty()) {
    const std::chrono::duration<double> age = now - ips_samples_.front().first;
    if (age.count() > 1.0) {
      ips_samples_.pop_front();
    } else {
      break;
    }
  }

  // Calculate average IPS over the last second
  if (ips_samples_.size() >= 2) {
    const auto& oldest = ips_samples_.front();
    const auto& newest = ips_samples_.back();

    const std::chrono::duration<double> time_span = newest.first - oldest.first;
    const uint64_t inference_span = newest.second - oldest.second;

    if (time_span.count() > 0.0) {
      avg_ips_ = std::round(inference_span / time_span.count() * 10.0) / 10.0;
    }
  } else {
    // Not enough samples yet, use instantaneous IPS
    avg_ips_ = std::round(curr_ips_ * 10.0) / 10.0;
  }
}

//==============================================================================
// Detection Processing
//==============================================================================

GstFlowReturn Detection::new_detection_callback(GstElement* sink,
                                                gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  return self->new_detection(sink);
}

GstFlowReturn Detection::new_detection(GstElement* sink) {
  GstSample* sample = nullptr;
  g_signal_emit_by_name(sink, "pull-sample", &sample);
  if (!sample) {
    return GST_FLOW_OK;
  }

  calculate_ips();

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  // Get buffer timestamp for synchronization
  const guint64 timestamp = GST_BUFFER_PTS(buffer);

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  if (map.size < sizeof(uint32_t)) {
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  uint32_t num_boxes = 0;
  std::memcpy(&num_boxes, map.data, sizeof(uint32_t));

  std::vector<DetectionBox> boxes;
  boxes.reserve(num_boxes);

  constexpr size_t detection_size = sizeof(AraDetection);
  const uint8_t* raw_data = map.data + sizeof(uint32_t);

  const size_t expected_size = sizeof(uint32_t) + num_boxes * detection_size;
  if (map.size < expected_size) {
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  for (uint32_t i = 0; i < num_boxes; ++i) {
    AraDetection det;
    std::memcpy(&det, raw_data + i * detection_size, detection_size);

    DetectionBox box;
    box.x1 = det.xmin;
    box.y1 = det.ymin;
    box.x2 = det.xmax;
    box.y2 = det.ymax;
    box.score = det.confidence;
    box.class_id = det.class_id;
    box.track_id = 1;

    // Validate box coordinates
    if (box.x1 >= 0.0f && box.y1 >= 0.0f && box.x2 > box.x1 &&
        box.y2 > box.y1) {
      boxes.push_back(box);
    }
  }

  gst_buffer_unmap(buffer, &map);

  // Store detections with timestamp for synchronization
  {
    std::lock_guard<std::mutex> lock(targets_mutex_);
    timestamped_detections_[timestamp] = std::move(boxes);

    // Clean up old timestamps (keep last 2 seconds worth)
    constexpr guint64 MAX_TIMESTAMP_AGE = 2000000000;  // 2 seconds in ns
    auto it = timestamped_detections_.begin();
    while (it != timestamped_detections_.end()) {
      if (timestamp > it->first &&
          (timestamp - it->first) > MAX_TIMESTAMP_AGE) {
        it = timestamped_detections_.erase(it);
      } else {
        ++it;
      }
    }
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

//==============================================================================
// Overlay Rendering
//==============================================================================

void Detection::prepare_overlay_callback(GstElement* overlay, GstCaps* caps,
                                         gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  self->prepare_overlay(caps);
}

void Detection::prepare_overlay(GstCaps* caps) {
  if (video_caps_) {
    gst_caps_unref(video_caps_);
  }
  video_caps_ = gst_caps_copy(caps);
}

void Detection::draw_overlay_callback(GstElement* overlay, cairo_t* cr,
                                      guint64 timestamp, guint64 duration,
                                      gpointer user_data) {
  auto* self = static_cast<Detection*>(user_data);
  self->draw_overlay(cr, timestamp);
}

void Detection::draw_overlay(cairo_t* cr, guint64 timestamp) {
  if (!video_caps_) {
    return;
  }

  calculate_fps();
  print_stats_to_console();

  double current_ips;
  {
    std::lock_guard<std::mutex> lock(ips_mutex_);
    current_ips = avg_ips_;
  }

  //----------------------------------------------------------------------------
  // Draw FPS/IPS overlay
  //----------------------------------------------------------------------------
  if (!no_osd_stats_) {
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 24.0);

  char fps_text[32];
  char ips_text[32];
  snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", avg_fps_);
  snprintf(ips_text, sizeof(ips_text), "IPS: %.1f", current_ips);

  cairo_text_extents_t fps_extents, ips_extents;
  cairo_text_extents(cr, fps_text, &fps_extents);
  cairo_text_extents(cr, ips_text, &ips_extents);

  const double max_text_width = std::max(fps_extents.width, ips_extents.width);
  constexpr double padding = 8.0;
  constexpr double line_spacing = 6.0;
  const double total_text_height =
      fps_extents.height + ips_extents.height + line_spacing;

  const double x = padding;
  const double y = padding + fps_extents.height;

  const double bg_width = max_text_width + 2.0 * padding;
  const double bg_height = total_text_height + 2.0 * padding;
  constexpr double corner_radius = 6.0;

  // Draw rounded rectangle background
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + bg_width - corner_radius,
            y - fps_extents.height - padding + corner_radius, corner_radius,
            -M_PI / 2.0, 0.0);
  cairo_arc(
      cr, x + bg_width - corner_radius,
      y + padding + total_text_height - fps_extents.height - corner_radius,
      corner_radius, 0.0, M_PI / 2.0);
  cairo_arc(
      cr, x + corner_radius,
      y + padding + total_text_height - fps_extents.height - corner_radius,
      corner_radius, M_PI / 2.0, M_PI);
  cairo_arc(cr, x + corner_radius,
            y - fps_extents.height - padding + corner_radius, corner_radius,
            M_PI, 3.0 * M_PI / 2.0);
  cairo_close_path(cr);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
  cairo_fill_preserve(cr);

  cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.9);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);

  // Draw FPS text
  cairo_move_to(cr, x + padding, y);
  cairo_set_source_rgb(cr, 0.0, 1.0, 0.8);
  cairo_show_text(cr, fps_text);

  // Draw IPS text
  cairo_move_to(cr, x + padding, y + fps_extents.height + line_spacing);
  cairo_set_source_rgb(cr, 1.0, 0.7, 0.0);
  cairo_show_text(cr, ips_text);
}
  //----------------------------------------------------------------------------
  // Find detections matching this timestamp
  //----------------------------------------------------------------------------
  std::vector<DetectionBox> current_targets;
  {
    std::lock_guard<std::mutex> lock(targets_mutex_);

    if (timestamped_detections_.empty()) {
      return;
    }

    // Find closest timestamp (prefer earlier detections for less latency)
    guint64 closest_diff = UINT64_MAX;
    auto closest_it = timestamped_detections_.end();

    for (auto iter = timestamped_detections_.begin();
         iter != timestamped_detections_.end(); ++iter) {
      // Only consider detections at or before current timestamp
      if (iter->first <= timestamp) {
        const guint64 diff = timestamp - iter->first;

        if (diff < closest_diff) {
          closest_diff = diff;
          closest_it = iter;
        }
      }
    }

    // If no past detection found, use the earliest future one
    if (closest_it == timestamped_detections_.end()) {
      closest_it = timestamped_detections_.begin();
      closest_diff =
          (closest_it->first > timestamp) ? (closest_it->first - timestamp) : 0;
    }

    // Only use detection if within tolerance
    if (closest_it != timestamped_detections_.end() &&
        closest_diff < TIMESTAMP_TOLERANCE) {
      current_targets = closest_it->second;
    }
  }

  if (current_targets.empty()) {
    return;
  }

  //----------------------------------------------------------------------------
  // Draw detections
  //----------------------------------------------------------------------------

  if (!no_bbox_) {
    cairo_set_font_size(cr, 14.0);
    cairo_set_line_width(cr, 2.5);

    for (const auto& box : current_targets) {
     // Generate color based on class ID
     const double hue = (box.class_id * 137.5) / 360.0;
     double r, g, b;

     const int h_i = static_cast<int>(hue * 6.0);
     const double f = hue * 6.0 - h_i;
     const double q = 1.0 - f;

     switch (h_i % 6) {
       case 0:
         r = 1.0;
         g = f;
         b = 0.0;
         break;
       case 1:
         r = q;
         g = 1.0;
         b = 0.0;
         break;
       case 2:
         r = 0.0;
         g = 1.0;
         b = f;
         break;
       case 3:
         r = 0.0;
         g = q;
         b = 1.0;
         break;
       case 4:
         r = f;
         g = 0.0;
         b = 1.0;
         break;
       case 5:
         r = 1.0;
         g = 0.0;
         b = q;
         break;
       default:
         r = 1.0;
         g = 0.0;
         b = 0.0;
         break;
     }

     // Draw bounding box
     cairo_set_source_rgb(cr, r, g, b);
     cairo_rectangle(cr, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1);
     cairo_stroke(cr);
     if (!only_bbox_) {
       // Get class name and create label
       const std::string class_name = get_class_name(box.class_id);
       char label_text[64];
       snprintf(label_text, sizeof(label_text), "%s: %.0f%%", class_name.c_str(),
               box.score * 100.0f);

       cairo_text_extents_t label_extents;
       cairo_text_extents(cr, label_text, &label_extents);

       const double label_x = box.x1;
       const double label_y = box.y1 - 4.0;

       // Draw label background
       cairo_rectangle(cr, label_x, label_y - label_extents.height - 4.0,
			    label_extents.width + 8.0, label_extents.height + 6.0);
       cairo_fill(cr);

       // Draw label text
       cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
       cairo_move_to(cr, label_x + 4.0, label_y - 2.0);
       cairo_show_text(cr, label_text);
      }
    }
  }
}

int Detection::get_terminal_columns() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    return w.ws_col;
  }
  return 80;  // Default fallback
}

ModelResolution Demo::get_model_resolution(const std::string& model_path) {
  // Default fallback resolution
  ModelResolution result{640, 360, false};

  // Library is always loaded, so call directly
  dv_model_t* model_info = nullptr;
  int status = dv_model_get_parameters_from_file(model_path.c_str(), &model_info);

  if (status != 0) {  // DV_SUCCESS = 0
    g_printerr("Warning: Could not retrieve model parameters\n");
    return result;
  }

  if (!model_info || model_info->num_inputs == 0) {
    if (model_info) {
      dv_model_free_parameters(model_info);
    }
    return result;
  }

  // Extract width and height from first input layer
  const dv_model_input_param_t* input_param = &model_info->input_param[0];

  if (input_param->width > 0 && input_param->height > 0) {
    result.width = input_param->width;
    result.height = input_param->height;
    result.success = true;

    g_print("Model Resolution (from file): %dx%d\n", result.width, result.height);
    if (input_param->layer_name) {
      g_print("Input Layer: %s\n", input_param->layer_name);
    }
    g_print("Channels: %d, BPP: %d\n", input_param->nch, input_param->bpp);

    // Free model info
    dv_model_free_parameters(model_info);
    return result;
  }

  g_printerr("Warning: Invalid input dimensions in model\n");
  dv_model_free_parameters(model_info);
  return result;
}

//==============================================================================
// Demo Class Implementation
//==============================================================================
 Demo::Demo(int streams, int endpoint, const std::string& group,
           bool enable_perf_stats, bool sync, const std::string& model,
           int console_interval)
    : streams_(streams),
      endpoint_(endpoint),
      group_(group),
      enable_perf_stats_(enable_perf_stats),
      sync_(sync),
      pipeline_(nullptr),
      main_loop_(nullptr),
      model_(model),
      sock_("/var/run/proxy.sock"),
      video_width_(640),
      video_height_(360),
      no_bbox_(false),
      no_osd_stats_(false),
      only_bbox_(false),
      console_interval_(console_interval),
      stream_config_(std::make_unique<StreamConfigManager>()) {
  main_loop_ = g_main_loop_new(nullptr, FALSE);

  const auto [model_width, model_height, success] = get_model_resolution(model);

  if (!success) {
    g_print("Using default resolution: %dx%d\n", model_width, model_height);
  }

  // Source resolution (input source - camera, video, RTSP, etc.)
  int src_width = 1920;
  int src_height = 1080;

  // Calculate letterbox dimensions
  float gain = std::min((float)model_width / src_width, (float)model_height / src_height);
  int scaled_width = (int)(src_width * gain);
  int scaled_height = (int)(src_height * gain);

  // Set the calculated resolution for pipeline
  video_width_ = scaled_width;
  video_height_ = scaled_height;
}
Demo::~Demo() { cleanup(); }

void Demo::set_stream_config(std::unique_ptr<StreamConfigManager> config) {
  stream_config_ = std::move(config);
}

void Demo::add_stream_source(std::unique_ptr<StreamSourceConfig> config) {
  stream_config_->add_stream(std::move(config));
}

bool Demo::load_stream_config(const std::string& filepath) {
  return stream_config_->load_from_json(filepath);
}

void Demo::list_cameras() {
  std::cout << "\n----------------------------------------\n"
            << "  Available Camera Devices\n"
            << "----------------------------------------\n";

  DIR* dir = opendir("/dev");
  if (!dir) {
    std::cerr << "Error: Cannot open /dev directory\n";
    return;
  }

  struct dirent* entry;
  bool found = false;

  while ((entry = readdir(dir)) != nullptr) {
    if (strncmp(entry->d_name, "video", 5) == 0) {
      std::string device = "/dev/" + std::string(entry->d_name);
      int fd = open(device.c_str(), O_RDONLY);
      if (fd >= 0) {
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
          if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
            std::cout << device << " - " << cap.card << "\n";
            found = true;
          }
        }
        close(fd);
      }
    }
  }
  closedir(dir);

  if (!found) {
    std::cout << "No camera devices found\n";
  }
  std::cout << "----------------------------------------\n" << std::endl;
}

void Demo::set_no_bbox(bool no_bbox) {
  no_bbox_ = no_bbox;
  for (auto& detection : detections_) {
    detection->set_no_bbox(no_bbox);
  }
}

void Demo::set_no_osd_stats(bool no_osd_stats) {
  no_osd_stats_ = no_osd_stats;
  for (auto& detection : detections_) {
    detection->set_no_osd_stats(no_osd_stats);
  }
}

void Demo::set_only_bbox(bool only_bbox) {
  only_bbox_ = only_bbox;
  for (auto& detection : detections_) {
     detection->set_only_bbox(only_bbox);
  }
}

void Demo::print_total_stats() {
  double total_fps = 0.0;
  double total_ips = 0.0;

  for (int i = 0; i < streams_; ++i) {
    double fps = detections_[i]->get_avg_fps();
    double ips = detections_[i]->get_avg_ips();
    std::cout << "[Stream " << i << "] "
              << "FPS: " << std::fixed << std::setprecision(1) << fps
              << " | IPS: " << std::fixed << std::setprecision(1) << ips
              << std::endl;
  }

  for (const auto& detection : detections_) {
    total_fps += detection->get_avg_fps();
    total_ips += detection->get_avg_ips();
  }

  std::cout << "----------------------------------------\n"
            << "TOTAL: FPS: " << std::fixed << std::setprecision(1) << total_fps
            << " | IPS: " << std::fixed << std::setprecision(1) << total_ips << "\n"
            << "----------------------------------------\n"
            << std::endl;
}

bool Demo::start() {
  const std::string pipeline_str = build_compositor(streams_);

  std::string p = pipeline_str;
  size_t pos = 0;
  while ((pos = p.find("! ", pos)) != std::string::npos) {
      p.insert(pos + 2, "\n");
      pos += 3;
  }

  std::cout << "\n----------------------------------------" << std::endl;
  std::cout << "LAUNCHING GSTREAMER PIPELINE:" << std::endl;
  std::cout << "----------------------------------------" << std::endl;

  GError* error = nullptr;
  pipeline_ = gst_parse_launch(pipeline_str.c_str(), &error);

  if (error) {
    g_printerr("Failed to parse pipeline: %s\n", error->message);
    g_error_free(error);
    return false;
  }

  if (!pipeline_) {
    g_printerr("Failed to create pipeline\n");
    return false;
  }

  // Create Detection instances
  detections_.reserve(streams_);
  for (int i = 0; i < streams_; ++i) {
    detections_.push_back(std::make_unique<Detection>(i));
    detections_[i]->set_no_bbox(no_bbox_);
    detections_[i]->set_no_osd_stats(no_osd_stats_);
    detections_[i]->set_only_bbox(only_bbox_);
    detections_[i]->set_console_interval(console_interval_);
    detections_[i]->start(pipeline_);
  }

  if (!detections_.empty()) {
    detections_[streams_ - 1]->set_stats_callback([this]() {
    this->print_total_stats();
    });
  }

  // Setup bus
  GstBus* bus = gst_element_get_bus(pipeline_);
  gst_bus_add_signal_watch(bus);
  g_signal_connect(bus, "message", G_CALLBACK(on_bus_message_callback), this);
  gst_object_unref(bus);

  // Start playing
  const GstStateChangeReturn ret =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set pipeline to playing state\n");
    return false;
  }

  g_print("Pipeline started with %d stream(s)\n", streams_);
  if (enable_perf_stats_) {
    g_print("Preprocessing performance statistics: ENABLED\n");
  }
  g_print("Press Ctrl+C to stop\n");

  // Run main loop
  g_main_loop_run(main_loop_);

  return true;
}

std::string Demo::build_compositor(int number_of_streams) {
  int rows, cols;
  // Optimize grid layout for different stream counts
  switch (number_of_streams) {
    case 1:
      rows = 1;
      cols = 1;
      break;
    case 2:
      rows = 1;
      cols = 2;
      break;
    case 3:
    case 4:
      rows = 2;
      cols = 2;
      break;
    case 5:
    case 6:
      rows = 2;
      cols = 3;
      break;
    case 7:
    case 8:
      rows = 2;
      cols = 4;
      break;
    default:
      rows = 2;
      cols = static_cast<int>(
          std::ceil(static_cast<double>(number_of_streams) / rows));
  }

  constexpr int width = 1920;
  constexpr int height = 1080;
  const int stream_width = width / cols;
  const int stream_height = height / rows;

  std::string detection_pipeline;
  std::string compositor_pipeline;

  int group_id = 0;
  if (group_ == "pcie") {
    group_id = 1;
  } else if (group_ == "usb") {
    group_id = 2;
  }

  // Build source pipelines using new configuration system
  for (int num = 0; num < number_of_streams; ++num) {
    detection_pipeline += build_source_pipeline(num);
  }

  // Build processing branches (SAME FOR ALL SOURCE TYPES)
  for (int num = 0; num < number_of_streams; ++num) {
    const int xpos = (num % cols) * stream_width;
    const int ypos = (num / cols) * stream_height;

    //--------------------------------------------------------------------------
    // Inference branch
    //--------------------------------------------------------------------------
    detection_pipeline += "t" + std::to_string(num) + ". ";
    detection_pipeline +=
        "! queue name=q_inf" + std::to_string(num) + " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";

    // Build dvInf element
    detection_pipeline += "! dvInf model=" + model_ + " sock=" + sock_ +
                          " endpoint=" + std::to_string(endpoint_) +
                          " group=" + std::to_string(group_id) +
                          " stream=" + std::to_string(num) +
                          " orig-width=" + std::to_string(video_width_) +
                          " orig-height=" + std::to_string(video_height_) +
                          " iou=" + std::to_string(0.45) +
			                    " nms=" + std::to_string(0.25) +
                          " use-shm=true shm-path=/dev/shm/ara_inf_" +
                          std::to_string(getpid()) + "_" + std::to_string(num) +
                          " shm-size=24 skip=1 offset=0 silent=true ";

    // Only enable perf stats for the first stream to avoid cluttered output
    if (enable_perf_stats_ && num == 0) {
      detection_pipeline += " enable-perf-stats=true stats-interval=5";
    }

    detection_pipeline += " ";

    detection_pipeline +=
        "! queue name=q_det" + std::to_string(num) + " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
    detection_pipeline += "! appsink name=dv_post_sink_" + std::to_string(num) +
                          " emit-signals=true sync=false drop=false "
                          "max-buffers=1 ";

    //--------------------------------------------------------------------------
    // Display branch
    //--------------------------------------------------------------------------
    detection_pipeline += "t" + std::to_string(num) + ". ";
    detection_pipeline +=
        "! queue name=q_disp" + std::to_string(num) + " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
    detection_pipeline +=
        "! cairooverlay name=draw_detections_" + std::to_string(num) + " ";
    detection_pipeline +=
        "! queue name=q_af_c" + std::to_string(num) + " max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";

    detection_pipeline += "! imxvideoconvert_g2d ! video/x-raw,format=RGBA ";
    detection_pipeline += "! comp.sink_" + std::to_string(num) + " ";

    //--------------------------------------------------------------------------
    // Compositor configuration with keep-ratio
    //--------------------------------------------------------------------------
    compositor_pipeline +=
        "sink_" + std::to_string(num) + "::xpos=" + std::to_string(xpos) + " ";
    compositor_pipeline +=
        "sink_" + std::to_string(num) + "::ypos=" + std::to_string(ypos) + " ";
    compositor_pipeline += "sink_" + std::to_string(num) +
                           "::width=" + std::to_string(stream_width) + " ";
    compositor_pipeline += "sink_" + std::to_string(num) +
                           "::height=" + std::to_string(stream_height) + " ";
    compositor_pipeline += "sink_" + std::to_string(num) + "::keep-ratio=true ";
  }

  const std::string entire_pipeline =
      "imxcompositor_g2d name=comp background=0xFF000000 " + compositor_pipeline +
      "! waylandsink name=wlsink sync=" + std::string(sync_ ? "true" : "false") +
      " " + detection_pipeline;

  return entire_pipeline;
}

//==============================================================================
// Message Handling
//==============================================================================

gboolean Demo::on_bus_message_callback(GstBus* bus, GstMessage* message,
                                       gpointer user_data) {
  auto* self = static_cast<Demo*>(user_data);
  self->on_bus_message(message);
  return TRUE;
}

void Demo::set_fullscreen_when_ready() {
  GstElement* waylandsink = gst_bin_get_by_name(GST_BIN(pipeline_), "wlsink");
  if (waylandsink) {
    // Set fullscreen property to FALSE to allow window drag
    g_object_set(waylandsink, "fullscreen", FALSE, NULL);
    gst_object_unref(waylandsink);
  }
}

void Demo::on_bus_message(GstMessage* message) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ASYNC_DONE:
      // Pipeline is fully prerolled, now safe to set fullscreen
      set_fullscreen_when_ready();
      break;
    case GST_MESSAGE_EOS:
      g_print("\nReceived EOS - End of Stream\n");
      g_main_loop_quit(main_loop_);
      break;

    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_error(message, &err, &debug_info);
      g_printerr("\nError from element %s: %s\n", GST_OBJECT_NAME(message->src),
                 err->message);
      g_printerr("Debugging info: %s\n", debug_info ? debug_info : "none");
      g_clear_error(&err);
      g_free(debug_info);
      g_main_loop_quit(main_loop_);
      break;
    }

    case GST_MESSAGE_WARNING: {
      GError* err = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_warning(message, &err, &debug_info);
      g_clear_error(&err);
      g_free(debug_info);
      break;
    }

    case GST_MESSAGE_STREAM_START:
    case GST_MESSAGE_QOS:
      // Silently ignore these messages
      break;

    default:
      break;
  }
}

void Demo::cleanup() {
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }

  if (main_loop_) {
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
  }

  detections_.clear();
}

//==============================================================================
// Source Pipeline Building
//==============================================================================

std::string Demo::build_source_pipeline(int stream_id) {
  // Check if we have a configured source for this stream
  if (stream_id < static_cast<int>(stream_config_->get_stream_count())) {
    const StreamSourceConfig* config = stream_config_->get_stream(stream_id);
    if (config) {
      return config->build_gstreamer_source(stream_id);
    }
  }

  // Fallback: dynamically discover videos in sample directory
  auto video_files = discover_video_files(SAMPLE_VIDEOS_DIR);

  std::string video_path;
  if (!video_files.empty()) {
    // Cycle through available videos if stream_id exceeds count
    int video_idx = stream_id % static_cast<int>(video_files.size());
    video_path = video_files[video_idx];
    g_print("Stream %d fallback: using discovered video %s\n",
            stream_id, video_path.c_str());
  } else {
    // Last resort: try legacy naming convention
    video_path = SAMPLE_VIDEOS_DIR + "/video_" +
                 std::to_string(stream_id) + ".mp4";
    g_printerr("Warning: No videos discovered, trying legacy path: %s\n",
               video_path.c_str());
  }

  std::string pipeline;
  pipeline += "multifilesrc location=" + video_path +
              " loop=true name=src_" + std::to_string(stream_id) + " ";
  pipeline += "! h264parse ! v4l2h264dec ";
  pipeline += "! imxvideoconvert_g2d ! video/x-raw,format=BGRx,width=" +
              std::to_string(video_width_) + ",height=" +
              std::to_string(video_height_) + " ";
  pipeline += "! tee name=t" + std::to_string(stream_id) + " ";
  return pipeline;
}

//==============================================================================
// Main Function
//==============================================================================
void print_usage(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
            << "Options:\n"
            << "  -s, --stream <1-8>          Number of streams to process\n"
            << "                              (default: auto-detected from available videos)\n"
            << "                              Note: Ignored when using -f/--config\n"
            << "  -e, --endpoint <0-10>       ARA2 Endpoint to use (default: 0)\n"
            << "  -g, --group <pcie|all>      Device group (default: all)\n"
            << "  -y, --sync <true|false>     Enable/disable Waylandsink synchronization "
                "(default: false)\n"
            << "  -m, --model <model-name>    Select model. Currently supported models:\n"
            << "                              YOLOv8: yolov8n, yolov8s, yolov8m, yolov8l, yolov8x\n"
            << "                              YOLOx: yoloxs, yoloxs_512, yoloxm, yoloxl, yoloxx\n"
            << "  -t                          Console stats will be refreshed each t seconds, To disable set t to 0\n"
            << "  -p, --perf-stats            Enable detailed performance stats every 5 seconds\n"
            << "\nStream Source Options:\n"
            << "  -c, --camera <device>       Camera device (e.g., /dev/video0)\n"
            << "  -r, --rtsp <url>            RTSP stream URL\n"
            << "  -v, --video <file>          Video file path\n"
            << "  --test-pattern              Use test pattern source\n"
            << "  -f, --config <file>         Load stream configuration from JSON file\n"
            << "                              (automatically sets stream count from file)\n"
            << "  --list-cameras              List available V4L2 cameras and exit\n"
            << "\nDisplay Options:\n"
            << "  --no-bbox                   Draw no bounding boxes and labels\n"
            << "  --no-osd-stats              No on screen stats display\n"
            << "  --only-bbox                 Draw only bounding boxes, no labels\n"
            << "  -h, --help                  Show this help message\n"
            << "\nDefault Behavior:\n"
            << "  When no source flags (-c, -v, -r, --test-pattern, -f) are used,\n"
            << "  the application auto-discovers .mp4 files in:\n"
            << "    " << SAMPLE_VIDEOS_DIR << "\n"
            << "  and sets the stream count to the number of videos found (max 8).\n";
}

int main(int argc, char* argv[]) {
  int streams = 0;  // 0 means "auto-detect"
  int endpoint = 0;
  std::string group = "all";
  bool enable_perf_stats = false;
  bool sync = false;
  std::string model = "/usr/share/cnn/detection/yolov8n/model.dvm";
  bool no_bbox = false;
  bool no_osd_stats = false;
  int console_interval = 2;
  bool only_bbox = false;

  std::string config_file;
  auto stream_config = std::make_unique<StreamConfigManager>();
  bool config_file_loaded = false;
  bool streams_explicitly_set = false;
  bool source_explicitly_set = false;  // Track if any source flag was used

  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--list-cameras") {
      gst_init(&argc, &argv);
      Demo::list_cameras();
      gst_deinit();
      return 0;
    } else if (arg == "-f" || arg == "--config") {
      if (i + 1 < argc) {
        config_file = argv[++i];
        source_explicitly_set = true;
      }
    } else if (arg == "-c" || arg == "--camera") {
      if (i + 1 < argc) {
        stream_config->add_stream(
          std::make_unique<CameraV4L2Config>(argv[++i], 640, 360, 30));
        source_explicitly_set = true;
      }
    } else if (arg == "-r" || arg == "--rtsp") {
      if (i + 1 < argc) {
        bool use_tcp = true;
        stream_config->add_stream(
          std::make_unique<RTSPConfig>(argv[++i], 100, use_tcp));
        source_explicitly_set = true;
      }
    } else if (arg == "-v" || arg == "--video") {
      if (i + 1 < argc) {
        stream_config->add_stream(
          std::make_unique<VideoFileConfig>(argv[++i]));
        source_explicitly_set = true;
      }
    } else if (arg == "--test-pattern") {
      stream_config->add_stream(
        std::make_unique<TestPatternConfig>());
      source_explicitly_set = true;
    } else if (arg == "--no-bbox") {
      no_bbox = true;
    } else if (arg == "--no-osd-stats") {
      no_osd_stats = true;
    } else if (arg == "--only-bbox") {
      only_bbox = true;
    } else if (arg == "-s" || arg == "--stream") {
      if (i + 1 < argc) {
        streams = std::atoi(argv[++i]);
        streams_explicitly_set = true;
        if (streams < 1 || streams > 8) {
          std::cerr << "Error: stream count must be between 1 and 8\n";
          return 1;
        }
      }
    } else if (arg == "-e" || arg == "--endpoint") {
      if (i + 1 < argc) {
        endpoint = std::atoi(argv[++i]);
        if (endpoint < 0 || endpoint > 10) {
          std::cerr << "Error: endpoint must be between 0 and 10\n";
          return 1;
        }
      }
    } else if (arg == "-g" || arg == "--group") {
      if (i + 1 < argc) {
        std::string group_val = argv[++i];
        if (group_val == "pcie" || group_val == "all") {
          group = group_val;
        } else {
          std::cerr << "Error: group must be 'pcie' or 'all'\n";
          return 1;
        }
      }
    } else if (arg == "-y" || arg == "--sync") {
      if (i + 1 < argc) {
        std::string sync_val = argv[++i];
        if (sync_val == "true" || sync_val == "1") {
          sync = true;
        } else if (sync_val == "false" || sync_val == "0") {
          sync = false;
        } else {
          std::cerr << "Error: sync must be 'true' or 'false'\n";
          return 1;
        }
      }
    } else if (arg == "-m" || arg == "--model") {
      if (i + 1 < argc) {
        std::string val = argv[++i];
        if (val == "yolov8n")
          model = "/usr/share/cnn/detection/yolov8n/model.dvm";
        else if (val == "yolov8s")
          model = "/usr/share/cnn/detection/yolov8s/model.dvm";
        else if (val == "yolov8m")
          model = "/usr/share/cnn/detection/yolov8m/model.dvm";
        else if (val == "yolov8l")
          model = "/usr/share/cnn/detection/yolov8l/model.dvm";
        else if (val == "yolov8x")
          model = "/usr/share/cnn/detection/yolov8x/model.dvm";
        else if (val == "yoloxs")
          model = "/usr/share/cnn/detection/yoloxs/model.dvm";
        else if (val == "yoloxs_512")
          model = "/usr/share/cnn/detection/yoloxs_512/model.dvm";
        else if (val == "yoloxm")
          model = "/usr/share/cnn/detection/yoloxm/model.dvm";
        else if (val == "yoloxl")
          model = "/usr/share/cnn/detection/yoloxl/model.dvm";
        else if (val == "yoloxx")
          model = "/usr/share/cnn/detection/yoloxx/model.dvm";
        else {
          std::cerr << "Error: model not found\n";
          return 1;
        }
      }
    } else if (arg == "-t") {
      if (i + 1 < argc) {
        console_interval = std::atoi(argv[++i]);
      } else {
        std::cerr << "Error: -t requires a time value in seconds\n";
        return 1;
      }
    } else if (arg == "-p" || arg == "--perf-stats") {
      enable_perf_stats = true;
    }
  }

  // Load config file if specified
  if (!config_file.empty()) {
    if (!stream_config->load_from_json(config_file)) {
      std::cerr << "Failed to load configuration file: " << config_file << std::endl;
      return 1;
    }
    config_file_loaded = true;
    streams = static_cast<int>(stream_config->get_stream_count());

    if (streams < 1 || streams > 8) {
      std::cerr << "Error: JSON file must define between 1 and 8 streams (found "
                << streams << ")\n";
      return 1;
    }
    if (streams_explicitly_set) {
      std::cout << "Note: -s parameter ignored when using -f (config file takes priority)\n";
    }

    std::cout << "Loaded " << streams << " stream(s) from configuration file: "
              << config_file << std::endl;
  }
  // Auto-discover video files when no source is explicitly configured
  else if (!source_explicitly_set) {
    auto video_files = discover_video_files(SAMPLE_VIDEOS_DIR);

    if (!video_files.empty()) {
      int available_videos = static_cast<int>(video_files.size());

      if (!streams_explicitly_set) {
        // Auto-set stream count to match number of available videos
        streams = available_videos;
      }
      // else: -s was set, use that value; cycle through videos if needed

      // Add a VideoFileConfig for each stream
      for (int i = 0; i < streams; ++i) {
        int video_idx = i % available_videos;
        stream_config->add_stream(
            std::make_unique<VideoFileConfig>(video_files[video_idx]));
      }

      std::cout << "\n========================================\n"
                << "  Auto-discovered Video Files\n"
                << "========================================\n";
      for (size_t i = 0; i < video_files.size(); ++i) {
        std::cout << "  [" << i << "] " << video_files[i] << "\n";
      }
      std::cout << "Using " << streams << " stream(s) from "
                << available_videos << " available video(s)\n";
      if (streams > available_videos) {
        std::cout << "Note: Videos will be reused (cycled) across streams\n";
      }
      std::cout << "========================================\n" << std::endl;
    } else {
      std::cerr << "\n========================================\n"
                << "  ERROR: No Video Files Found\n"
                << "========================================\n"
                << "No .mp4 files found in:\n"
                << "  " << SAMPLE_VIDEOS_DIR << "\n\n"
                << "Please either:\n"
                << "  1. Place .mp4 files in the directory above\n"
                << "  2. Specify a video file:  -v /path/to/video.mp4\n"
                << "  3. Use a camera:          -c /dev/video0\n"
                << "  4. Use a config file:     -f config.json\n"
                << "  5. Use a test pattern:    --test-pattern\n"
                << "========================================\n" << std::endl;
      return 1;
    }
  }
  // Source explicitly set via CLI flags (-c, -v, -r, --test-pattern)
  // but no config file: use stream_config count or -s value
  else {
    if (!streams_explicitly_set) {
      int configured = static_cast<int>(stream_config->get_stream_count());
      streams = (configured > 0) ? configured : 1;
    }
    // If -s is set higher than configured sources, remaining streams
    // will use the fallback in build_source_pipeline()
  }

  // Final validation of stream count
  if (streams < 1) {
    streams = 1;
  } else if (streams > 8) {
    streams = 8;
  }

  // VALIDATE MODEL EXISTS BEFORE PROCEEDING
  if (!validate_model_exists(model)) {
    return 1;
  }

  // Initialize GStreamer
  gst_init(&argc, &argv);

  // Create and run demo
  {
            Demo demo(streams, endpoint, group, enable_perf_stats, sync, model, console_interval);
            demo.set_no_bbox(no_bbox);
            demo.set_no_osd_stats(no_osd_stats);
            demo.set_only_bbox(only_bbox);
            std::cout << "\n========================================\n"
                      << "  Ara YOLOv8 Multi-Stream Demo\n"
                      << "========================================\n"
                      << "Streams:    " << streams << "\n"
                      << "Endpoint:   " << endpoint << "\n"
                      << "Group:      " << group << "\n"
                      << "Sync:       " << (sync ? "true" : "false") << "\n"
                      << "Model Used: " << model << "\n"
                      << "Resolution: " << demo.get_video_width() << "x" << demo.get_video_height() << "\n";

            if (enable_perf_stats) {
              std::cout << "Perf Stats: ENABLED (stream 0 only)\n";
            }

            // Print stream configuration BEFORE moving ownership to demo
            if (config_file_loaded) {
              stream_config->print_summary();
            }

            std::cout << "========================================\n" << std::endl;

            // Move stream_config into demo AFTER we're done using it
            demo.set_stream_config(std::move(stream_config));

            demo.start();
  }

  // Cleanup
  gst_deinit();

  std::cout << "\nDemo completed successfully\n" << std::endl;

  return 0;
}
