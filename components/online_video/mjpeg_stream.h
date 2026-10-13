#pragma once

// Plain C++ (no ESPHome or ESP-IDF headers) so it can be unit tested on a desktop.
// Everything works on fixed buffers handed in by the owner: nothing here allocates.

#include <cstddef>
#include <cstdint>

namespace esphome::online_video {

/// Parts of an http:// URL. Points into the buffer passed to parse_url().
struct UrlParts {
  char host[64]{};
  uint16_t port{80};
  char path[192]{};
};

/// Parse "http://host[:port][/path]". Returns false for other schemes or when a part does not fit.
bool parse_url(const char *url, UrlParts &out);

/// Reads the status line and headers of an HTTP response.
class HttpResponseParser {
 public:
  /// Feed bytes; returns how many were consumed. Once done() is true, the rest is the body.
  size_t feed(const uint8_t *data, size_t len);
  bool done() const { return this->done_; }
  bool failed() const { return this->failed_; }
  int status() const { return this->status_; }
  bool chunked() const { return this->chunked_; }
  bool is_multipart() const { return this->multipart_; }
  void reset();

 protected:
  void handle_line_();

  char line_[160]{};
  size_t line_len_{0};
  int status_{0};
  bool first_line_{true};
  bool chunked_{false};
  bool multipart_{false};
  bool done_{false};
  bool failed_{false};
};

/// Removes HTTP/1.1 chunked transfer encoding, passing the payload to a sink.
class ChunkedDecoder {
 public:
  using Sink = bool (*)(void *ctx, const uint8_t *data, size_t len);
  void set_sink(Sink sink, void *ctx) {
    this->sink_ = sink;
    this->ctx_ = ctx;
  }
  /// Returns false on malformed input or when the sink asks to stop.
  bool feed(const uint8_t *data, size_t len);
  void reset();

 protected:
  enum class State : uint8_t { SIZE, SIZE_EXTENSION, DATA, DATA_END, TRAILER };
  Sink sink_{nullptr};
  void *ctx_{nullptr};
  State state_{State::SIZE};
  size_t remaining_{0};
  bool size_seen_{false};
};

/// Splits a multipart/x-mixed-replace MJPEG body into JPEG frames.
///
/// Part headers are read until a blank line. With Content-Length the body is read by size, without it
/// the parser stops at the JPEG end-of-image marker (FF D9). A frame is copied into the buffer only when
/// the owner wants one (wanted callback), otherwise its bytes are skipped without copying.
class MjpegParser {
 public:
  using WantedFn = bool (*)(void *ctx);
  using FrameFn = void (*)(void *ctx, const uint8_t *data, size_t len);

  void set_buffer(uint8_t *buffer, size_t capacity) {
    this->buffer_ = buffer;
    this->capacity_ = capacity;
  }
  void set_callbacks(WantedFn wanted, FrameFn frame, void *ctx) {
    this->wanted_ = wanted;
    this->frame_ = frame;
    this->ctx_ = ctx;
  }
  /// Always consumes everything. Returns false only when no buffer was set.
  bool feed(const uint8_t *data, size_t len);
  void reset();

  uint32_t frames() const { return this->frames_; }
  uint32_t skipped() const { return this->skipped_; }
  uint32_t too_big() const { return this->too_big_; }
  size_t largest_frame() const { return this->largest_; }

 protected:
  enum class State : uint8_t { HEADERS, BODY_SIZED, BODY_SCAN };
  void start_body_();
  void finish_frame_();
  void header_line_();

  uint8_t *buffer_{nullptr};
  size_t capacity_{0};
  WantedFn wanted_{nullptr};
  FrameFn frame_{nullptr};
  void *ctx_{nullptr};

  State state_{State::HEADERS};
  char line_[96]{};
  size_t line_len_{0};
  bool header_seen_{false};
  long content_length_{-1};
  size_t remaining_{0};
  size_t filled_{0};
  bool keep_{false};
  bool overflow_{false};
  uint8_t prev_{0};

  uint32_t frames_{0};
  uint32_t skipped_{0};
  uint32_t too_big_{0};
  size_t largest_{0};
};

}  // namespace esphome::online_video
