/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: stream_config.cpp
 */

#include "stream_config.h"
#include "simple_json_parser.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>

#include <cctype>
#include <cstdio>
#include <string>

#include <glib.h>  // for guint64 (matches splitmuxsink property type)

// POSIX socket headers for RTSP SDP codec detection
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>


namespace {

// Wrap an argument in single quotes for safe use in a popen() shell command.
// Any embedded single quote is escaped using the standard '\'' idiom. This
// prevents shell-special characters found in RTSP URLs (notably '&', '?',
// '=') from being interpreted by the shell (e.g. '&' backgrounding the
// command), and closes the shell-injection hole for this call site.
std::string shell_single_quote(const std::string& arg) {
  std::string out = "'";
  for (char c : arg) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

}  // namespace

// Parse host, port, path and credentials from an rtsp:// URL.
// Returns false if the URL is not a valid rtsp:// URL.
bool parse_rtsp_url(const std::string& url,
                    std::string& host, std::string& port,
                    std::string& path, std::string& userinfo) {
  const std::string scheme = "rtsp://";
  if (url.rfind(scheme, 0) != 0) return false;
  std::string rest = url.substr(scheme.size());

  // Extract userinfo (user:pass@)
  auto at = rest.find('@');
  if (at != std::string::npos) {
    userinfo = rest.substr(0, at);
    rest = rest.substr(at + 1);
  }

  // Extract host[:port]
  auto slash = rest.find('/');
  std::string hostport = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
  path = (slash != std::string::npos) ? rest.substr(slash) : "/";

  auto colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    host = hostport.substr(0, colon);
    port = hostport.substr(colon + 1);
  } else {
    host = hostport;
    port = "554";
  }
  return !host.empty();
}

// Send an RTSP DESCRIBE request over TCP and parse the SDP response to detect
// the video codec. Returns "h264", "h265", or "unknown". Times out after
// timeout_sec seconds so it never blocks startup.
std::string detect_codec_rtsp_sdp_impl(const std::string& url,
                                        int timeout_sec) {
  std::string host, port, path, userinfo;
  if (!parse_rtsp_url(url, host, port, path, userinfo))
    return "unknown";

  // Resolve host — use AF_UNSPEC + SOCK_STREAM hints
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
    return "unknown";

  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return "unknown"; }

  // Set non-blocking for connect timeout
  fcntl(fd, F_SETFL, O_NONBLOCK);
  connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  struct timeval tv{ timeout_sec, 0 };
  if (select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
    close(fd); return "unknown";
  }
  // Verify the connection actually succeeded — a failed connect (e.g.
  // ECONNREFUSED) makes the socket writable with a pending error.
  {
    int sock_err = 0;
    socklen_t sock_err_len = sizeof(sock_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len) < 0
        || sock_err != 0) {
      close(fd); return "unknown";
    }
  }
  // Restore blocking
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

  // Build RTSP DESCRIBE request
  std::ostringstream req;
  std::string sanitised_url = "rtsp://" + host + ":" + port + path;

  req << "DESCRIBE " << sanitised_url << " RTSP/1.0\r\n"
    << "CSeq: 1\r\n"
    << "Accept: application/sdp\r\n";

  if (!userinfo.empty()) {
    // Basic auth: base64-encode userinfo
    // Simple inline base64 (no dependency)
    static const char* b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    const auto& s = userinfo;
    for (size_t i = 0; i < s.size(); i += 3) {
      unsigned int v = (unsigned char)s[i] << 16;
      if (i+1 < s.size()) v |= (unsigned char)s[i+1] << 8;
      if (i+2 < s.size()) v |= (unsigned char)s[i+2];
      encoded += b64[(v >> 18) & 0x3f];
      encoded += b64[(v >> 12) & 0x3f];
      encoded += (i+1 < s.size()) ? b64[(v >> 6) & 0x3f] : '=';
      encoded += (i+2 < s.size()) ? b64[v & 0x3f] : '=';
    }
    req << "Authorization: Basic " << encoded << "\r\n";
    // Zero out credentials from memory after use
    std::fill(userinfo.begin(), userinfo.end(), '\0');
    std::fill(encoded.begin(), encoded.end(), '\0');
  }
  req << "\r\n";

  std::string reqstr = req.str();
  if (send(fd, reqstr.c_str(), reqstr.size(), 0) < 0) {
    close(fd); return "unknown";
  }

  std::fill(reqstr.begin(), reqstr.end(), '\0');

  // Read response with timeout
  std::string response;
  char buf[1024];
  constexpr size_t kMaxResponseBytes = 64 * 1024;
  while (true) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval rtv{ timeout_sec, 0 };
    if (select(fd + 1, &rfds, nullptr, nullptr, &rtv) <= 0) break;
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) break;
    buf[n] = '\0';
    response += buf;
    // Stop once we have the SDP body (look for a=rtpmap)
    if (response.size() > kMaxResponseBytes) break;
    if (response.find("a=rtpmap") != std::string::npos) break;
  }
  close(fd);

  // Parse a=rtpmap lines for H264 / H265
  // Format: a=rtpmap:<pt> H264/90000  or  a=rtpmap:<pt> H265/90000
  std::istringstream ss(response);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.rfind("a=rtpmap:", 0) != 0) continue;
    auto sp = line.find(' ');
    if (sp == std::string::npos) continue;
    std::string enc = line.substr(sp + 1);
    // Strip trailing \r and everything after '/'
    auto slash = enc.find('/');
    if (slash != std::string::npos) enc = enc.substr(0, slash);
    while (!enc.empty() && (enc.back() == '\r' || enc.back() == ' '))
      enc.pop_back();
    // Convert to uppercase for comparison
    for (auto& c : enc) c = toupper(c);
    if (enc == "H264") { return "h264"; }
    if (enc == "H265" || enc == "HEVC") { return "h265"; }
  }
  return "unknown";
}

int compute_retention_max_files(int retention_minutes, int segment_seconds) {
  int seg = segment_seconds > 0 ? segment_seconds : 60;
  int max_files = (retention_minutes * 60 + seg - 1) / seg;
  if (max_files < 1) {
    max_files = 1;
  }
  return max_files;
}

std::string detect_codec_gst_impl(const std::string& filepath) {

    // Single-quote the file path / URL so shell metacharacters (e.g. '&' in
    // an RTSP query string) are passed literally to gst-discoverer-1.0.
    std::string cmd =
        "gst-discoverer-1.0 " + shell_single_quote(filepath) + " 2>/dev/null";


    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "unknown";

    char buffer[256];
    std::string output = "";

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    pclose(pipe);

    std::string codec = "unknown";

    if (output.find("H.264") != std::string::npos)
        codec = "h264";
    else if (output.find("H.265") != std::string::npos ||
             output.find("HEVC") != std::string::npos)
        codec = "h265";

    return codec;
}

//==============================================================================
// VideoFileConfig Implementation
//==============================================================================

std::string VideoFileConfig::build_gstreamer_source(int stream_id) const {
    std::ostringstream pipeline;

    std::string codec = get_codec();

    pipeline << "multifilesrc loop=true location=" << filepath_ << " ! ";
    pipeline << "tsdemux name=demux_" << stream_id << " ";

    if (codec == "h264")
    	pipeline << "demux_" << stream_id << ". ! h264parse ! v4l2h264dec ";
    else if (codec == "h265")
        pipeline << "demux_" << stream_id << ". ! h265parse ! v4l2h265dec ";

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
  
  return pipeline.str();
}

std::string CameraV4L2Config::to_string() const {
  std::ostringstream ss;
  ss << "V4L2 Camera: " << device_ 
     << " (" << width_ << "x" << height_ << " @ " << fps_ << " fps)";
  return ss.str();
}

//==============================================================================
// CameraLibcameraConfig Implementation
//==============================================================================

std::string CameraLibcameraConfig::build_gstreamer_source(int stream_id) const {
  std::ostringstream pipeline;
  const char* dewarp_env = std::getenv("DEWARP");
  std::string dewarp = (dewarp_env && dewarp_env[0] != '\0') ? dewarp_env : "";

  pipeline << "libcamerasrc name=src_" << stream_id;
  if (!camera_id_.empty()) {
    pipeline << " camera-name=" << camera_id_;
  }
  pipeline << " ";

  pipeline << "! video/x-raw,format=YUY2 ";
  if (!dewarp.empty()) {
    pipeline << "! " << dewarp << " ";
  }
  pipeline << "! queue max-size-buffers=1 leaky=downstream ";

  return pipeline.str();
}

std::string CameraLibcameraConfig::to_string() const {
  std::ostringstream ss;
  ss << "libcamera: "
     << (camera_id_.empty() ? "default" : camera_id_);
  if (record_) {
    ss << " [recording -> " << record_dir_ << "]";
  }
  return ss.str();
}

//==============================================================================
// RTSPConfig Implementation
//==============================================================================
std::string RTSPConfig::build_gstreamer_source(int stream_id) const {
  std::ostringstream pipeline;

  // The inference url_ is interpolated directly into the pipeline description.
  // gst_parse_launch does not use a shell, so RTSP reserved characters
  // (user:pass@host, %-encoding, ?query&params) are safe here; this is the
  // same property the record_url validator allows through, just without the
  // extra rtsp:// prefix / whitespace check applied to the recorder branch.
  pipeline << "rtspsrc location=" << url_ 
            << " latency=" << latency_ms_;
  
  if (use_tcp_) {
    pipeline << " protocols=tcp";
  } else {
    pipeline << " protocols=udp";
  } 
  
  
  pipeline << " tcp-timeout=" << tcp_timeout_us_
           << " timeout=" << tcp_timeout_us_
           << " do-retransmission=false retry=0";
  pipeline << " name=src_" << stream_id << " ";
  pipeline << "! application/x-rtp,media=video "
           << "! watchdog name=watchdog_" << stream_id << " timeout=5000 "
           << "! decodebin3 name=decodebin_" << stream_id << " ";

  return pipeline.str();
}

namespace {

// Reject strings that could break out of the GStreamer pipeline description
// when interpolated. The pipeline string is parsed by gst_parse_launch, which
// does NOT invoke a shell, so shell metacharacters ($ ( ) | & < > ;) are not
// interpreted and are harmless here. We therefore only reject the characters
// that are meaningful to the gst_parse_launch pipeline-description grammar and
// would let an injected value add extra elements or properties: whitespace
// (token/element separator), '!' (element-link operator), quotes, backtick,
// and backslash (escape). These characters do not appear in well-formed RTSP
// URLs, whose reserved chars ('@' '%' '?' '=' '&' ':' '/') remain allowed.
bool has_pipeline_breaking_chars(const std::string& s) {
  for (unsigned char c : s) {
    // Reject any whitespace (token/element separator) and any non-printable
    // control character (e.g. embedded NUL, newline, tab) that could truncate
    // or splice the pipeline description.
    if (std::isspace(c) || !std::isprint(c)) {
      return true;
    }
    // Reject every character that carries meaning in the gst_parse_launch
    // grammar so an injected value cannot add elements, properties, links,
    // caps, bins, or comments. None of these appear in a well-formed RTSP
    // URL, whose reserved chars ('@' '%' '?' '=' '&' ':' '/') stay allowed.
    switch (c) {
      case '!':   // element-link operator
      case '"':   // quoting
      case '\'':  // quoting
      case '`':   // quoting / shell-style
      case '\\':  // escape
      case ',':   // caps field / property separator
      case ';':   // pipeline / statement separator
      case '(':   // bin open
      case ')':   // bin close
      case '[':   // range / list
      case ']':   // range / list
      case '{':   // list open
      case '}':   // list close
      case '<':   // structure/array open
      case '>':   // structure/array close
      case '#':   // comment
        return true;
      default:
        break;
    }
  }
  return false;
}



// A record URL must be a plain rtsp:// URL with a non-empty host, free of
// pipeline-breaking chars.
bool is_valid_record_url(const std::string& url) {
  const std::string scheme = "rtsp://";
  if (url.rfind(scheme, 0) != 0) {
    return false;
  }
  // Require at least one character after the scheme (a host); reject a bare
  // "rtsp://" that would never connect.
  if (url.size() <= scheme.size()) {
    return false;
  }
  return !has_pipeline_breaking_chars(url);
}


// A record directory must be a non-empty absolute path free of
// pipeline-breaking chars.
bool is_valid_record_dir(const std::string& dir) {
  if (dir.empty() || dir[0] != '/') {
    return false;
  }
  return !has_pipeline_breaking_chars(dir);
}

}  // namespace

std::string RTSPConfig::build_recording_branch(int stream_id) const {
  // Recording requires an explicit high-resolution record URL; it never
  // uses the low-resolution inference URL.
  if (!record_ || record_url_.empty()) {
    return "";
  }

  // Validate the record URL and directory before injecting them into the
  // pipeline description. Reject anything that could break out of the
  // pipeline string or is not a well-formed rtsp:// URL / absolute path.
  if (!is_valid_record_url(record_url_)) {
    std::cerr << "Warning: RTSP 'record_url' is not a valid rtsp:// URL or "
                 "contains invalid characters; recording disabled for stream "
              << stream_id << ".\n";
    return "";
  }
  if (!is_valid_record_dir(record_dir_)) {
    std::cerr << "Warning: RTSP 'record_dir' is not a valid absolute path or "
                 "contains invalid characters; recording disabled for stream "
              << stream_id << ".\n";
    return "";
  }

  // Compute how many segments to retain so that total retained duration
  // covers at least retention_minutes_. Shared with the application-side
  // retention janitor via compute_retention_max_files() so the two can never
  // diverge.
  int segment_seconds = segment_seconds_ > 0 ? segment_seconds_ : 60;
  int max_files =
      compute_retention_max_files(retention_minutes_, segment_seconds);

  // splitmuxsink max-size-time is expressed in nanoseconds. Use guint64 to
  // match the type of the splitmuxsink "max-size-time" property (guint64).
  guint64 segment_ns =
      static_cast<guint64>(segment_seconds) * 1000000000ULL;

  // Detect the record_url video codec so the recorder can select matching
  // passthrough elements. rtspsrc exposes pads dynamically, and a static
  // caps-filtered link only completes when a pad with the matching
  // encoding-name appears; pinning the wrong codec leaves the video pad
  // unlinked and GStreamer posts a "not-linked" streaming error. Cameras
  // commonly deliver H.264 on the high-resolution record substream, so the
  // recorder must adapt rather than assume H.265.
  std::string codec = detect_codec_gst_impl(record_url_);

  // The RTP encoding-name and RTP depayloader/parser pair are codec-specific.
  // For an unrecognized codec, fall back to H.265 (the previous default) but
  // warn, since detection may have failed (e.g. gst-discoverer unavailable)
  // rather than the stream truly being H.265.
  std::string encoding_name;
  std::string depay_parse;
  if (codec == "h264") {
    encoding_name = "H264";
    depay_parse = "rtph264depay ! h264parse";
  } else if (codec == "h265") {
    encoding_name = "H265";
    depay_parse = "rtph265depay ! h265parse";
  } else {
    std::cerr << "Warning: could not determine record_url video codec for "
                 "stream "
              << stream_id
              << " (gst-discoverer reported '" << codec
              << "'); assuming H.265. If recording fails to start, verify the "
                 "record_url codec.\n";
    encoding_name = "H265";
    depay_parse = "rtph265depay ! h265parse";
  }

  std::ostringstream branch;
  // Standalone recorder: pull the dedicated high-resolution record URL with
  // its own rtspsrc (independent of the inference source), depay and parse the
  // elementary stream WITHOUT decoding or re-encoding, and write bounded,
  // self-contained MP4 segments. splitmuxsink deletes the oldest segment once
  // max-files is exceeded, so total disk usage stays bounded regardless of
  // runtime.
  //
  // CODEC: recording supports H.264 and H.265/HEVC via passthrough. The video
  // codec is auto-detected from record_url and the branch pins the RTP caps to
  // the matching encoding-name and uses the matching depay/parse pair
  // (rtph264depay/h264parse or rtph265depay/h265parse). The caps keep
  // media=video only, so any additional non-video pad (for example an AAC
  // audio track) is left unlinked and ignored rather than causing an error.
  // Passthrough of the already-encoded elementary stream avoids any
  // decode/re-encode cost.
  branch << "rtspsrc location=" << record_url_
         << " latency=" << latency_ms_
         << (use_tcp_ ? " protocols=tcp" : " protocols=udp")
         << " name=recsrc_" << stream_id << " "
         << "! application/x-rtp,media=video,encoding-name=" << encoding_name
         << " "
         << "! " << depay_parse << " "
         // The actual per-segment file name is supplied at runtime by the
         // "format-location" signal handler (cam<N>_<timestamp>.mp4); the
         // location template below is only a fallback if that is not wired.
         << "! splitmuxsink name=recsink_" << stream_id << " "
         << "location=" << record_dir_ << "/cam" << stream_id
         << "_%05d.mp4 "
         << "max-size-time=" << segment_ns << " "
         << "max-files=" << max_files << " "
         // mp4mux wraps the raw H.264 (avc1) or H.265 (hvc1) elementary
         // stream into MP4.
         << "muxer-factory=mp4mux "
         // Robust muxing periodically reserves and rewrites the MP4 moov
         // atom, so the segment currently being recorded (and any segment
         // left behind by an unclean shutdown) stays playable, NVR-style,
         // instead of only becoming playable once finalized.
         << "use-robust-muxing=true ";

  return branch.str();
}


std::string RTSPConfig::to_string() const {
  std::ostringstream ss;
  ss << "RTSP: " << url_ 
     << " (latency=" << latency_ms_ << "ms, " 
     << (use_tcp_ ? "TCP" : "UDP");
  if (is_recording_enabled()) {
    ss << ", recording " << record_url_ << " -> " << record_dir_
       << " (" << segment_seconds_ << "s segments, "
       << retention_minutes_ << "min retention)";
  }
  ss << ")";
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

StreamSourceType StreamConfigManager::get_stream_type(int stream_id) const {
  if (stream_id < 0 || stream_id >= static_cast<int>(streams_.size()))
    return StreamSourceType::VIDEO_FILE;  // safe non-RTSP fallback for out-of-range ids
  return streams_[stream_id]->get_type();
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
      } else if (type == "camera_libcamera") {
        std::string camera_id = "";
        bool record = false;
        std::string record_dir = "/var/lib/ara2-vision-examples/recordings";
        int segment_seconds = 60;
        int max_files = 10;

        auto id_it = stream.find("camera_id");
        if (id_it != stream.end()) camera_id = id_it->second;

        auto record_it = stream.find("record");
        if (record_it != stream.end()) {
          record = (record_it->second == "true" || record_it->second == "1");
        }

        auto record_dir_it = stream.find("record_dir");
        if (record_dir_it != stream.end() && !record_dir_it->second.empty()) {
          record_dir = record_dir_it->second;
        }

        auto seg_it = stream.find("record_segment_sec");
        if (seg_it != stream.end()) {
          try { segment_seconds = std::stoi(seg_it->second); }
          catch (...) {}
        }

        auto maxf_it = stream.find("record_max_files");
        if (maxf_it != stream.end()) {
          try { max_files = std::stoi(maxf_it->second); }
          catch (...) {}
        }

        bool rtsp_out = false;
        int rtsp_port = 8554;

        auto rtsp_out_it = stream.find("rtsp_out");
        if (rtsp_out_it != stream.end()) {
          rtsp_out = (rtsp_out_it->second == "true" || rtsp_out_it->second == "1");
        }

        auto rtsp_port_it = stream.find("rtsp_port");
        if (rtsp_port_it != stream.end()) {
          try {
            int parsed = std::stoi(rtsp_port_it->second);
            if (parsed > 0 && parsed <= 65535) {
              rtsp_port = parsed;
            } else {
              g_printerr("rtsp_port %d out of range [1-65535]; "
                         "using default %d\n", parsed, rtsp_port);
            }
          }
          catch (...) {
            g_printerr("rtsp_port '%s' is not a valid number; "
                       "using default %d\n",
                       rtsp_port_it->second.c_str(), rtsp_port);
          }
        }

        add_stream(std::make_unique<CameraLibcameraConfig>(
            camera_id, record, record_dir, segment_seconds, max_files,
            rtsp_out, rtsp_port));
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

          // Optional recording configuration (RTSP-only feature). Recording
          // uses a dedicated high-resolution record URL (record_url); the
          // primary 'url' is the low-resolution inference stream.
          bool record = false;
          std::string record_url = "";
          std::string record_dir = "/var/lib/ara2-vision-examples/recordings";
          int segment_seconds = 60;
          int retention_minutes = 30;

          auto record_it = stream.find("record");
          if (record_it != stream.end()) {
            record = (record_it->second == "true" || record_it->second == "1");
          }

          auto record_url_it = stream.find("record_url");
          if (record_url_it != stream.end()) {
            record_url = record_url_it->second;
          }

          auto record_dir_it = stream.find("record_dir");
          if (record_dir_it != stream.end() && !record_dir_it->second.empty()) {
            record_dir = record_dir_it->second;
          }

          // Parse the numeric recording parameters defensively: fall back to
          // the defaults if the value is missing or not a valid integer.
          auto segment_it = stream.find("segment_seconds");
          if (segment_it != stream.end()) {
            try {
              segment_seconds = std::stoi(segment_it->second);
            } catch (const std::exception&) {
              std::cerr << "Warning: invalid 'segment_seconds' value '"
                        << segment_it->second << "'; using default "
                        << segment_seconds << ".\n";
            }
          }

          auto retention_it = stream.find("retention_minutes");
          if (retention_it != stream.end()) {
            try {
              retention_minutes = std::stoi(retention_it->second);
            } catch (const std::exception&) {
              std::cerr << "Warning: invalid 'retention_minutes' value '"
                        << retention_it->second << "'; using default "
                        << retention_minutes << ".\n";
            }
          }

          // Recording requires a dedicated high-resolution record URL; if
          // requested without one, warn and disable recording for this stream.
          if (record && record_url.empty()) {
            std::cerr << "Warning: RTSP 'record' is true but no 'record_url' "
                         "is set; recording disabled for this stream.\n";
            record = false;
          }

          add_stream(std::make_unique<RTSPConfig>(
              url_it->second, latency, use_tcp, /*tcp_timeout_us=*/5000000,
              record, record_url, record_dir,
              segment_seconds, retention_minutes));
        }
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
