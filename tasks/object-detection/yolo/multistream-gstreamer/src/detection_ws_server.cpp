/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: detection_ws_server.cpp
 *
 * Implementation of the Boost.Beast WebSocket detection broadcaster declared
 * in detection_ws_server.h. Builds the per-frame detection JSON (no external
 * JSON library), manages the accept loop and per-client sessions, and posts
 * all socket writes onto a single io_context thread so publish() is safe to
 * call from the GStreamer inference callbacks. See the header for the JSON
 * schema and the rtp_ts sync-key semantics.
 */

#include "detection_ws_server.h"
#include "multistream_yolo.h"  // for DetectionBox, get_class_name()

#include <cstdint>
#include <iostream>
#include <sstream>

//==============================================================================
// JSON helpers (no external JSON library required)
//==============================================================================

// Escape a plain ASCII string for embedding in JSON.
static std::string json_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    if (c == '"')       out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else                out += c;
  }
  out += '"';
  return out;
}

// Build the JSON payload for one stream's detections.
// rtp_ts is the wire RTP timestamp (90 kHz) recovered for this frame; when
// rtp_valid is true it is emitted as "rtp_ts", the sync key the browser uses
// to line detections up with the displayed video frame. rtp_valid is false for
// non-RTSP streams or when no PTS match was found, in which case rtp_ts is
// omitted.
static std::string build_json(int stream_id,
                               const std::vector<DetectionBox>& boxes,
                               uint32_t rtp_ts, bool rtp_valid) {
  std::ostringstream o;
  o << "{\"camera\":" << json_str("cam" + std::to_string(stream_id));
  if (rtp_valid) {
    o << ",\"rtp_ts\":" << rtp_ts;
  }
  o << ",\"boxes\":[";

  bool first = true;
  for (const auto& b : boxes) {
    if (!first) o << ',';
    first = false;

    float x      = b.x1;
    float y      = b.y1;
    float width  = b.x2 - b.x1;
    float height = b.y2 - b.y1;

    // Truncate to two decimal places to keep the payload compact.
    o << std::fixed;
    o.precision(2);
    o << "{\"x\":"          << x
      << ",\"y\":"          << y
      << ",\"width\":"      << width
      << ",\"height\":"     << height
      << ",\"label\":"      << json_str(get_class_name(b.class_id))
      << ",\"confidence\":" << b.score
      << ",\"track_id\":"   << b.track_id;
    // Persistent ReID identity: emit person_id when the gallery has committed
    // one, else null. Stamped onto the DetectionBox in new_detection().
    // IdentityManager assigns ids starting at 1 (mNextPersonId's initial
    // value) and never issues 0, so "> 0" and "unassigned" are the same test;
    // -1 is the sentinel used before any identity is committed.
    if (b.person_id > 0)
      o << ",\"person_id\":" << b.person_id;
    else
      o << ",\"person_id\":null";
    o << ",\"normalized\":false}";
  }
  o << "]}";
  return o.str();
}

//==============================================================================
// WsSession
//==============================================================================

WsSession::WsSession(tcp::socket socket, DetectionWebSocketServer* server)
    : ws_(std::move(socket)), server_(server) {}

void WsSession::start() {
  // Must be set BEFORE async_accept: the decorator customizes the handshake
  // response, so applying it afterwards had no effect.
  ws_.set_option(ws::stream_base::decorator([](ws::response_type& res) {
    res.set(beast::http::field::server, "ara2-detection-ws");
  }));
  // The stream's executor is the per-session strand created by accept_loop()
  // via net::make_strand(). Beast propagates that executor to every completion
  // handler installed with bind_front_handler, so all of accept/read/write and
  // the queue mutations they perform run serialized on the one strand.
  ws_.async_accept(
      beast::bind_front_handler(&WsSession::on_accept, shared_from_this()));
}

void WsSession::on_accept(beast::error_code ec) {
  if (ec) {
    server_->remove_session(shared_from_this());
    return;
  }
  // Runs on the session strand. The handshake's pending-write slot is now
  // released, so queued messages can finally be written.
  handshake_done_ = true;
  do_read();
  if (!writing_ && !write_queue_.empty()) {
    writing_ = true;
    do_write();
  }
}

void WsSession::do_read() {
  ws_.async_read(
      read_buf_,
      beast::bind_front_handler(&WsSession::on_read, shared_from_this()));
}

void WsSession::on_read(beast::error_code ec, std::size_t /*bytes*/) {
  if (ec) {
    close_self();
    return;
  }
  // Discard client data; we only send.
  read_buf_.consume(read_buf_.size());
  do_read();
}

void WsSession::send(std::string message) {
  // publish() calls this from the io thread, but not necessarily on this
  // session's strand. Hop onto the stream's executor (the per-session strand)
  // before touching write_queue_/writing_ so all session state stays
  // single-threaded and every async_write is serialized on the same executor
  // Beast reads/writes run on. Fix for the pending_guard assertion.
  net::dispatch(
      ws_.get_executor(),
      [self = shared_from_this(), msg = std::move(message)]() mutable {
        self->write_queue_.push_back(std::move(msg));
        // Bound the backlog: a client that never drains must not grow this
        // without limit. Drop the oldest frames, since detections are only
        // useful live. Logged once per overflow episode (not once per dropped
        // frame) so a slow client causing frame loss is visible without
        // spamming the log every time a frame is dropped.
        constexpr size_t kMaxQueued = 64;
        if (self->write_queue_.size() > kMaxQueued) {
          if (!self->queue_overflow_warned_) {
            self->queue_overflow_warned_ = true;
            std::cerr << "[WS] session write queue full ("
                      << self->write_queue_.size()
                      << " frames), dropping oldest\n";
          }
          while (self->write_queue_.size() > kMaxQueued) {
            self->write_queue_.pop_front();
          }
        } else {
          self->queue_overflow_warned_ = false;
        }
        // Until async_accept completes, the handshake owns the stream's single
        // pending-write slot; starting an async_write here would trip Beast's
        // pending_guard assertion. on_accept() drains the queue instead.
        if (!self->handshake_done_) return;
        if (self->writing_) return;
        self->writing_ = true;
        self->do_write();
      });
}

void WsSession::do_write() {
  // Runs only on the stream's executor (strand).
  if (write_queue_.empty()) {
    writing_ = false;
    return;
  }

  // Move the front message into writing_msg_ so it stays alive at a stable
  // address for the entire async_write: it is a separate std::string once
  // moved out here, so nothing later done to write_queue_ (push_back, or
  // another pop_front) can touch it.
  writing_msg_ = std::move(write_queue_.front());
  write_queue_.pop_front();

  ws_.text(true);
  ws_.async_write(
      net::buffer(writing_msg_),
      beast::bind_front_handler(&WsSession::on_write, shared_from_this()));
}

void WsSession::on_write(beast::error_code ec, std::size_t /*bytes*/) {
  // Runs only on the stream's executor (strand).
  if (ec) {
    close_self();
    return;
  }
  writing_msg_.clear();
  do_write();
}

void WsSession::close() {
  // Every other operation on this stream (accept/read/write, and the queue
  // state do_write() touches) runs on the session's strand. Shutting down the
  // socket directly from the caller's thread -- stop()'s thread during server
  // shutdown -- would touch it concurrently with whatever async op is still in
  // flight, which is a data race. Dispatch onto the same strand so the
  // shutdown is serialized with everything else.
  net::dispatch(ws_.get_executor(), [self = shared_from_this()]() {
    beast::error_code ec;
    self->ws_.next_layer().socket().shutdown(tcp::socket::shutdown_both, ec);
  });
}

void WsSession::close_self() {
  close();
  server_->remove_session(shared_from_this());
}

//==============================================================================
// DetectionWebSocketServer
//==============================================================================

DetectionWebSocketServer::DetectionWebSocketServer(uint16_t port)
    : port_(port),
      acceptor_(ioc_) {}

DetectionWebSocketServer::~DetectionWebSocketServer() {
  stop();
}

void DetectionWebSocketServer::start() {
  tcp::endpoint endpoint(tcp::v4(), port_);

  beast::error_code ec;
  acceptor_.open(endpoint.protocol(), ec);
  if (ec) {
    std::cerr << "[WS] acceptor open: " << ec.message() << "\n";
    return;
  }
  acceptor_.set_option(net::socket_base::reuse_address(true), ec);
  acceptor_.bind(endpoint, ec);
  if (ec) {
    std::cerr << "[WS] bind: " << ec.message() << "\n";
    return;
  }
  acceptor_.listen(net::socket_base::max_listen_connections, ec);
  if (ec) {
    std::cerr << "[WS] listen: " << ec.message() << "\n";
    return;
  }

  accept_loop();

  io_thread_ = std::thread([this]() { ioc_.run(); });

  std::cout << "[WS] detection server listening on port " << port_ << "\n";
}

void DetectionWebSocketServer::stop() {
  beast::error_code ec;
  acceptor_.close(ec);

  // Close all sessions.
  std::vector<std::shared_ptr<WsSession>> to_close;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    to_close.assign(sessions_.begin(), sessions_.end());
    sessions_.clear();
  }
  for (auto& s : to_close) s->close();
  // Give the dispatched close lambdas a chance to run before stopping the context.
  // ioc_.poll() flushes all ready handlers without blocking.
  ioc_.poll();
  ioc_.stop();
  if (io_thread_.joinable()) io_thread_.join();
}

void DetectionWebSocketServer::accept_loop() {
  acceptor_.async_accept(
      net::make_strand(ioc_),
      [this](beast::error_code ec, tcp::socket socket) {
        if (ec) return;  // acceptor closed

        auto session = std::make_shared<WsSession>(std::move(socket), this);
        {
          std::lock_guard<std::mutex> lock(sessions_mutex_);
          sessions_.insert(session);
        }
        session->start();

        accept_loop();  // re-arm
      });
}

void DetectionWebSocketServer::remove_session(
    std::shared_ptr<WsSession> session) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.erase(session);
}

void DetectionWebSocketServer::publish(int stream_id,
                                        const std::vector<DetectionBox>& boxes,
                                        uint32_t rtp_ts, bool rtp_valid) {
  // Build JSON once regardless of how many clients are connected.
  std::string payload = build_json(stream_id, boxes, rtp_ts, rtp_valid);

  // Fan out to every connected session. send() itself hops onto each session's
  // strand, so it is safe to call directly here while holding sessions_mutex_;
  // we do not need to marshal onto ioc_ first. Copy the shared_ptrs out under
  // the lock so we never hold sessions_mutex_ across the per-session dispatch.
  //
  // reserve() uses a fixed estimate rather than sessions_.size(): reading size()
  // without the lock would itself race with accept_loop()/remove_session()
  // mutating sessions_ on another thread, the same class of bug this is fixing.
  // push_back still grows the vector correctly if the real count differs.
  std::vector<std::shared_ptr<WsSession>> targets;
  targets.reserve(16);
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& s : sessions_) targets.push_back(s);
  }
  for (auto& s : targets) {
    s->send(payload);
  }
}
