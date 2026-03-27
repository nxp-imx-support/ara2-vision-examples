/*
 * Copyright (c) 2025, Kinara, Inc. All rights reserved.
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: multistream_yolov8.cpp
 */

#include "multistream_yolov8.h"

// System headers
#include <gst/video/video.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// Standard C++ headers
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

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
              << "2. Verify installation:\n"
              << "   ls -lh /usr/share/cnn/detection/yolov8*/\n\n"
              << "  Available model variants:\n"
              << "   - yolov8n (nano)        - Fastest, lowest accuracy\n"
              << "   - yolov8s (small)       - Balanced speed and accuracy\n"
              << "   - yolov8m (medium)      - Good accuracy, moderate speed\n"
              << "   - yolov8l (large)       - High accuracy, slower\n"
              << "   - yolov8x (extra-large) - Highest accuracy, slowest\n\n"
              << "========================================\n"
              << std::endl;
    return false;
  }
  return true;
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
      video_caps_(nullptr) {
  const auto now = std::chrono::steady_clock::now();
  start_time_ = now;
  last_fps_update_ = now;
  last_ips_update_ = now;
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

  double current_ips;
  {
    std::lock_guard<std::mutex> lock(ips_mutex_);
    current_ips = avg_ips_;
  }

  //----------------------------------------------------------------------------
  // Draw FPS/IPS overlay
  //----------------------------------------------------------------------------
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

int Detection::get_terminal_columns() {
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
    return w.ws_col;
  }
  return 80;  // Default fallback
}

//==============================================================================
// Demo Class Implementation
//==============================================================================
 Demo::Demo(int streams, int endpoint, const std::string& group, bool enable_perf_stats, bool sync, const std::string& model)
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
      video_height_(360) {
  main_loop_ = g_main_loop_new(nullptr, FALSE);
}
Demo::~Demo() { cleanup(); }

bool Demo::start() {
  const std::string pipeline_str = build_compositor(streams_);

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
    detections_[i]->start(pipeline_);
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

    // Build detection pipelines
    for (int num = 0; num < number_of_streams; ++num) {
      detection_pipeline += "multifilesrc location=/usr/share/ara2-vision-examples/sample_videos/video_" +
                            std::to_string(num) + ".mp4 loop=true name=src_" +
                            std::to_string(num) + " ";
      detection_pipeline += "! h264parse ! v4l2h264dec ";
      detection_pipeline +=
          "! imxvideoconvert_g2d ! "
          "video/x-raw,format=BGRx,width=640,height=360 ";
      detection_pipeline += "! tee name=t" + std::to_string(num) + " ";
    }

    // Build processing branches
    for (int num = 0; num < number_of_streams; ++num) {
      const int xpos = (num % cols) * stream_width;
      const int ypos = (num / cols) * stream_height;

      // Calculate proper scaling to maintain aspect ratio
      // Source is 640x360 (16:9)
      constexpr float source_aspect = 640.0f / 360.0f;
      const float target_aspect = static_cast<float>(stream_width) / stream_height;
    
      int scaled_width, scaled_height;
      if (source_aspect > target_aspect) {
        // Width-constrained
        scaled_width = stream_width;
        scaled_height = static_cast<int>(stream_width / source_aspect);
      } else {
        // Height-constrained
        scaled_height = stream_height;
        scaled_width = static_cast<int>(stream_height * source_aspect);
      }

      //--------------------------------------------------------------------------
      // Inference branch
      //--------------------------------------------------------------------------
      detection_pipeline += "t" + std::to_string(num) + ". ";
      detection_pipeline +=
          "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";

      // Build dvPre element with new performance stats support
      detection_pipeline += "! dvPre model=" + model_ +
                            " skip=1 offset=0 silent=true";
    
      // Only enable perf stats for the first stream to avoid cluttered output
      if (enable_perf_stats_ && num == 0) {
        detection_pipeline += " enable-perf-stats=true stats-interval=5";
      }
    
      detection_pipeline += " ";

      detection_pipeline += "! dvInf model=" + model_ + " sock=" + sock_ +
                            " endpoint=" + std::to_string(endpoint_) +
                            " group=" + std::to_string(group_id) +
                            " stream=" + std::to_string(num) +
                            " use-shm=true shm-path=/dev/shm/ara_inf_" +
                            std::to_string(num) +
                            " shm-size=24 skip=1 offset=0 silent=true ";

      detection_pipeline +=
          "! dvPost model=" + model_ +
          " orig-width=640 orig-height=360 iou=" + std::to_string(0.45) +
          " nms=" + std::to_string(0.25) +
          " use-shm=true skip=1 offset=0 silent=true ";

      detection_pipeline +=
          "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
      detection_pipeline += "! appsink name=dv_post_sink_" + std::to_string(num) +
                            " emit-signals=true sync=false drop=false "
                            "max-buffers=2 ";

      //--------------------------------------------------------------------------
      // Display branch - scale maintaining aspect ratio, then letterbox to fill grid cell
      //--------------------------------------------------------------------------
      detection_pipeline += "t" + std::to_string(num) + ". ";
      detection_pipeline +=
          "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
      detection_pipeline +=
          "! cairooverlay name=draw_detections_" + std::to_string(num) + " ";
      detection_pipeline +=
          "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 ";
      // Scale to fit while maintaining aspect ratio
      detection_pipeline += "! imxvideoconvert_g2d ! video/x-raw,format=RGBA,width=" + 
                            std::to_string(scaled_width) + ",height=" + 
                            std::to_string(scaled_height) + " ";
      // Add letterboxing to fill the grid cell
      const int left = (stream_width - scaled_width) / 2;
      const int top = (stream_height - scaled_height) / 2;
      detection_pipeline += "! videobox left=" + std::to_string(-left) + 
                            " right=" + std::to_string(-(stream_width - scaled_width - left)) +
                            " top=" + std::to_string(-top) + 
                            " bottom=" + std::to_string(-(stream_height - scaled_height - top)) +
                            " fill=black ";
      detection_pipeline += "! comp.sink_" + std::to_string(num) + " ";

      //--------------------------------------------------------------------------
      // Compositor configuration
      //--------------------------------------------------------------------------
      compositor_pipeline +=
          "sink_" + std::to_string(num) + "::xpos=" + std::to_string(xpos) + " ";
      compositor_pipeline +=
          "sink_" + std::to_string(num) + "::ypos=" + std::to_string(ypos) + " ";
      compositor_pipeline += "sink_" + std::to_string(num) +
                             "::width=" + std::to_string(stream_width) + " ";
      compositor_pipeline += "sink_" + std::to_string(num) +
                             "::height=" + std::to_string(stream_height) + " ";
    }

    const std::string entire_pipeline =
        "imxcompositor_g2d name=comp " + compositor_pipeline +
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
    g_object_set(waylandsink, "fullscreen", TRUE, NULL);
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
// Main Function
//==============================================================================

void print_usage(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [OPTIONS]\n"
            << "Options:\n"
            << "  -s, --stream <1-8>          Number of streams to process "
               "(default: 8)\n"
            << "  -e, --endpoint <0-10>       ARA2 Endpoint to use (default: 0)\n"
            << "  -y, --sync <true|false>     Enable/disable Waylandsink synchronization "
               "(default: false)\n"   
            << "  -m, --model <model-name>    Select model. Currently supported models: yolov8n, yolov8l, yolov8s, yolov8m and yolov8x\n"
            << "  -h, --help                  Show this help message\n";
}

int main(int argc, char* argv[]) {
  int streams = 8;
  int endpoint = 0;
  std::string group = "all";
  bool enable_perf_stats = false;
  bool sync = false;
  std::string model = "/usr/share/cnn/detection/yolov8n/model.dvm";

  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "-s" || arg == "--stream") {
      if (i + 1 < argc) {
        streams = std::atoi(argv[++i]);
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
	      else {
		      std::cerr << "Error: model not found\n";
		      return 0;
	      }
	    }
    }
  }

  // Override endpoint if group is not "all"
  if (group != "all") {
    endpoint = 0;
  }

  // VALIDATE MODEL EXISTS BEFORE PROCEEDING
  if (!validate_model_exists(model)) {
    return 1;  // Exit with error code
  }

  // Initialize GStreamer
  gst_init(&argc, &argv);

  std::cout << "\n========================================\n"
            << "  Ara YOLOv8n Multi-Stream Demo\n"
            << "========================================\n"
            << "Streams:    " << streams << "\n"
            << "Endpoint:   " << endpoint << "\n"
            << "Sync:       " << (sync ? "true" : "false") << "\n"
            << "Model Used: " << model << "\n";
  if (enable_perf_stats) {
    std::cout << "Perf Stats: ENABLED (stream 0 only)\n";
  }
  
  std::cout << "========================================\n"
            << std::endl;

  // Create and run demo
  {
    Demo demo(streams, endpoint, group, enable_perf_stats, sync, model);
    demo.start();
  }

  // Cleanup
  gst_deinit();

  std::cout << "\nDemo completed successfully\n" << std::endl;

  return 0;
}
