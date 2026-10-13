#pragma once

#include "esphome/components/image/image.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "mjpeg_stream.h"

#include <atomic>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#endif

class JPEGDEC;
struct jpeg_draw_tag;

namespace esphome::online_video {

static constexpr uint8_t MAX_HEADERS = 4;

/// An image whose pixels come from an MJPEG stream.
///
/// A background task reads the stream and decodes frames into the back buffer. The main loop only
/// swaps the two buffers and fires on_frame, so it never waits on the network or the decoder.
/// All memory is allocated once in setup().
class OnlineVideo : public Component, public image::Image {
 public:
  OnlineVideo(const char *url, int width, int height, uint32_t buffer_size, uint8_t max_fps);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void add_header(const char *name, const char *value);
  void set_autostart(bool autostart) { this->autostart_ = autostart; }

  /// Start reading the stream. Safe to call when already running.
  void start();
  /// Stop reading the stream; the last frame stays in the image.
  void stop();
  bool is_running() const { return this->running_.load(); }

  template<typename F> void add_on_frame_callback(F &&callback) { this->frame_callback_.add(std::forward<F>(callback)); }
  template<typename F> void add_on_error_callback(F &&callback) { this->error_callback_.add(std::forward<F>(callback)); }

 protected:
  static void task_entry_(void *arg);
  void task_loop_();
  void stream_once_();
  void report_error_(const char *what);
  void log_stats_(uint32_t now);

  // Called by the MJPEG parser on the background task.
  static bool frame_wanted_(void *ctx);
  static void frame_ready_(void *ctx, const uint8_t *data, size_t len);
  static bool chunk_sink_(void *ctx, const uint8_t *data, size_t len);
  bool decode_(const uint8_t *data, size_t len);
  static int draw_callback_(jpeg_draw_tag *draw);

  const char *url_;
  UrlParts url_parts_{};
  const char *header_names_[MAX_HEADERS]{};
  const char *header_values_[MAX_HEADERS]{};
  uint8_t header_count_{0};
  uint32_t buffer_size_;
  uint32_t frame_interval_ms_;
  bool autostart_{false};

  // Two RGB565 frames: data_start_ (from image::Image) always points at the front one.
  uint8_t *frames_[2]{};
  uint8_t front_{0};
  size_t frame_bytes_{0};
  uint8_t *jpeg_buffer_{nullptr};
  JPEGDEC *jpeg_{nullptr};
  MjpegParser parser_;
  ChunkedDecoder chunked_;
  uint8_t rx_[1436]{};

  // Shared between the background task and the main loop.
  std::atomic<bool> running_{false};
  std::atomic<bool> swap_pending_{false};
  std::atomic<bool> error_pending_{false};
  std::atomic<bool> wake_{false};

  // Background task only.
  uint8_t *decode_target_{nullptr};
  int offset_x_{0};
  int offset_y_{0};
  int decoded_w_{0};
  int decoded_h_{0};
  int cleared_w_[2]{};
  int cleared_h_[2]{};
  uint32_t next_frame_ms_{0};
  uint32_t last_data_ms_{0};
  uint32_t stats_since_ms_{0};
  uint32_t stats_decoded_{0};
  uint32_t stats_decode_ms_{0};
  uint32_t stats_failed_{0};
  uint32_t stats_skipped_base_{0};
  uint32_t stats_too_big_base_{0};
  bool progressive_warned_{false};

  CallbackManager<void()> frame_callback_;
  CallbackManager<void()> error_callback_;

#ifdef USE_ESP32
  TaskHandle_t task_{nullptr};
#else
  std::thread thread_;
#endif
};

class FrameTrigger : public Trigger<> {
 public:
  explicit FrameTrigger(OnlineVideo *parent) {
    parent->add_on_frame_callback([this]() { this->trigger(); });
  }
};

class ErrorTrigger : public Trigger<> {
 public:
  explicit ErrorTrigger(OnlineVideo *parent) {
    parent->add_on_error_callback([this]() { this->trigger(); });
  }
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OnlineVideo> {
 public:
  void play(const Ts &...x) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<OnlineVideo> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<OnlineVideo> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_running(); }
};

}  // namespace esphome::online_video
