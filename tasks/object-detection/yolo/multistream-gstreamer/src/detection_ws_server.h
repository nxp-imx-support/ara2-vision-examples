/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * File: detection_ws_server.h
 *
 * Boost.Beast WebSocket server that broadcasts YOLO detection results to
 * connected clients. Each inference result is published as a JSON text frame:
 *
 *   {
 *     "camera": "cam0",
 *     "rtp_ts": 3125551234,
 *     "boxes": [
 *       {
 *         "x": 120, "y": 80, "width": 150, "height": 300,
 *         "label": "person", "confidence": 0.92, "track_id": 7,
 *         "normalized": false
 *       }
 *     ]
 *   }
 *
 * Coordinates are in video pixel space as returned by the dvInf plugin.
 * "normalized" is always false so the JS client maps them 1:1 using the
 * actual video element dimensions. "rtp_ts" is the wire RTP timestamp used to
 * synchronize detections with the displayed video frame; it is present only
 * for RTSP streams and omitted otherwise.
 */

#ifndef DETECTION_WS_SERVER_H
#define DETECTION_WS_SERVER_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Forward declaration to avoid pulling in multistream_yolo.h from here.
struct DetectionBox;

namespace beast = boost::beast;
namespace ws    = beast::websocket;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

// ---- Forward declarations --------------------------------------------------

class WsSession;

// ---- DetectionWebSocketServer ----------------------------------------------

class DetectionWebSocketServer {
 public:
  explicit DetectionWebSocketServer(uint16_t port = 8765);
  ~DetectionWebSocketServer();

  // Non-copyable, non-movable (owns a running thread).
  DetectionWebSocketServer(const DetectionWebSocketServer&) = delete;
  DetectionWebSocketServer& operator=(const DetectionWebSocketServer&) = delete;

  // Start listening and spawn the io thread.
  void start();

  // Stop accepting, close all sessions, join the io thread.
  void stop();

  // Publish detections for one stream to all connected clients.
  // Safe to call from any thread (posts work onto the io_context).
  // rtp_ts is the wire RTP timestamp (90 kHz) recovered for this frame from the
  // pre-decode probe; it is the browser-facing sync key. rtp_valid is false for
  // non-RTSP streams or when no PTS match was found, in which case the JSON
  // omits the rtp_ts field.
  void publish(int stream_id, const std::vector<DetectionBox>& boxes,
               uint32_t rtp_ts, bool rtp_valid);

  // Called by WsSession when it closes so it can be removed from the set.
  void remove_session(std::shared_ptr<WsSession> session);

 private:
  void accept_loop();

  uint16_t port_;
  net::io_context ioc_;
  tcp::acceptor acceptor_;

  std::set<std::shared_ptr<WsSession>> sessions_;
  std::mutex sessions_mutex_;

  std::thread io_thread_;
};

// ---- WsSession -------------------------------------------------------------

class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(tcp::socket socket, DetectionWebSocketServer* server);

  // Perform the async WebSocket upgrade handshake then start reading
  // (reads are discarded; we only need write direction).
  void start();

  // Enqueue a text message for async delivery. Thread-safe.
  void send(std::string message);

  // Close the underlying TCP socket. Called by the server during shutdown.
  void close();

 private:
  void on_accept(beast::error_code ec);
  void do_read();
  void on_read(beast::error_code ec, std::size_t bytes);
  void do_write();
  void on_write(beast::error_code ec, std::size_t bytes);
  void close_self();

  ws::stream<beast::tcp_stream> ws_;
  DetectionWebSocketServer* server_;
  beast::flat_buffer read_buf_;

  // All session state below is only ever touched from the stream's executor
  // (a strand created in DetectionWebSocketServer::accept_loop), so no mutex is
  // required. send() dispatches onto that executor, which serializes every
  // async op (accept/read/write) and every queue mutation.
  //
  // deque, not vector: do_write() pops from the front on every completed
  // write, which is O(1) on a deque and O(n) on a vector (it has to shift
  // every remaining element down).
  std::deque<std::string> write_queue_;

  // The message currently being transmitted. Kept alive here for the whole
  // duration of async_write so the net::buffer never aliases a vector element
  // that a later push_back could reallocate.
  std::string writing_msg_;
  bool writing_{false};
  // False until async_accept has completed. Beast allows only one pending
  // operation of each kind per stream, and the WebSocket handshake counts as a
  // pending write, so starting an async_write before the handshake finishes
  // trips the pending_guard assertion. Sessions are published to the server's
  // session set before the handshake completes, so publish() can reach send()
  // during that window; queue those messages instead of writing them.
  bool handshake_done_{false};

  // Set while write_queue_ is over its cap, so the overflow warning in send()
  // logs once per overflow episode rather than once per dropped frame. Clears
  // once the queue drains back under the cap.
  bool queue_overflow_warned_{false};
};

#endif  // DETECTION_WS_SERVER_H
