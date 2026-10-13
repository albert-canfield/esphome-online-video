#include "online_video.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_API_HOMEASSISTANT_STATES
#include "esphome/components/api/api_server.h"
#endif

#include <JPEGDEC.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef USE_ESP32
#include <chrono>
#endif

namespace esphome::online_video {

static const char *const TAG = "online_video";

static constexpr uint32_t SOCKET_TIMEOUT_MS = 1000;   // how quickly a blocked read notices stop()
static constexpr uint32_t NO_DATA_TIMEOUT_MS = 10000;  // give up on a silent connection
static constexpr uint32_t RECONNECT_DELAY_MS = 3000;
static constexpr uint32_t STATS_INTERVAL_MS = 30000;
static constexpr uint32_t TOKEN_POLL_MS = 200;

static void sleep_ms(uint32_t ms) {
#ifdef USE_ESP32
  vTaskDelay(pdMS_TO_TICKS(ms));
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

OnlineVideo::OnlineVideo(const char *url, int width, int height, uint32_t buffer_size, uint8_t max_fps)
    : image::Image(nullptr, width, height, image::IMAGE_TYPE_RGB565, image::TRANSPARENCY_OPAQUE),
      url_(url),
      buffer_size_(buffer_size),
      frame_interval_ms_(1000 / max_fps) {}

void OnlineVideo::add_header(const char *name, const char *value) {
  if (this->header_count_ < MAX_HEADERS) {
    this->header_names_[this->header_count_] = name;
    this->header_values_[this->header_count_] = value;
    this->header_count_++;
  }
}

void OnlineVideo::setup() {
  if (!parse_url(this->url_, this->url_parts_)) {
    ESP_LOGE(TAG, "Could not use URL %s (only http://host[:port]/path is supported)", this->url_);
    this->mark_failed();
    return;
  }

  // Frames and the JPEG buffer go to PSRAM when there is some, internal RAM otherwise.
  RAMAllocator<uint8_t> allocator;
  this->frame_bytes_ = static_cast<size_t>(this->width_) * this->height_ * 2;
  for (auto &frame : this->frames_) {
    frame = allocator.allocate(this->frame_bytes_);
    if (frame == nullptr) {
      ESP_LOGE(TAG, "Not enough memory for a %dx%d frame (%u bytes)", this->width_, this->height_,
               static_cast<unsigned>(this->frame_bytes_));
      this->mark_failed();
      return;
    }
    memset(frame, 0, this->frame_bytes_);
  }
  this->jpeg_buffer_ = allocator.allocate(this->buffer_size_);
  // The decoder state is used constantly while decoding, so keep it in internal RAM if possible.
  JPEGDEC *decoder_memory = RAMAllocator<JPEGDEC>(RAMAllocator<JPEGDEC>::PREFER_INTERNAL).allocate(1);
  if (this->jpeg_buffer_ == nullptr || decoder_memory == nullptr) {
    ESP_LOGE(TAG, "Not enough memory for the %" PRIu32 " byte JPEG buffer and decoder", this->buffer_size_);
    this->mark_failed();
    return;
  }
  this->jpeg_ = new (decoder_memory) JPEGDEC();
  this->data_start_ = this->frames_[this->front_];

  this->parser_.set_buffer(this->jpeg_buffer_, this->buffer_size_);
  this->parser_.set_callbacks(frame_wanted_, frame_ready_, this);
  this->chunked_.set_sink(chunk_sink_, this);

#ifdef USE_ESP32
  // Low priority on the core that runs Wi-Fi: the network stack still preempts it, and the main
  // loop (LVGL, touch) keeps the other core to itself.
#if portNUM_PROCESSORS > 1
  const BaseType_t core = 0;
#else
  const BaseType_t core = tskNO_AFFINITY;
#endif
  if (xTaskCreatePinnedToCore(task_entry_, "online_video", 5120, this, 1, &this->task_, core) != pdPASS) {
    ESP_LOGE(TAG, "Could not start the stream task");
    this->mark_failed();
    return;
  }
#else
  this->thread_ = std::thread(task_entry_, this);
  this->thread_.detach();
#endif

#ifdef USE_API_HOMEASSISTANT_STATES
  if (this->camera_ != nullptr) {
    api::global_api_server->subscribe_home_assistant_state(this->camera_, "access_token", [this](StringRef token) {
      LockGuard lock(this->token_lock_);
      const size_t n = std::min(token.size(), sizeof(this->token_) - 1);
      memcpy(this->token_, token.c_str(), n);
      this->token_[n] = '\0';
    });
  }
#endif

  // The loop only runs when the task has a new frame or an error to hand over.
  this->disable_loop();
  if (this->autostart_)
    this->start();
}

void OnlineVideo::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Online video:\n"
                "  %s: %s\n"
                "  Size: %dx%d\n"
                "  Max frame rate: %" PRIu32 " fps\n"
                "  JPEG buffer: %" PRIu32 " bytes\n"
                "  Autostart: %s",
                this->camera_ != nullptr ? "Home Assistant" : "URL", this->url_, this->width_, this->height_,
                1000 / this->frame_interval_ms_, this->buffer_size_, YESNO(this->autostart_));
  if (this->camera_ != nullptr)
    ESP_LOGCONFIG(TAG, "  Camera: %s", this->camera_);
  ESP_LOGCONFIG(TAG, "  Fit: %s", this->fit_ == FIT_COVER ? "cover" : "contain");
  for (uint8_t i = 0; i < this->header_count_; i++)
    ESP_LOGCONFIG(TAG, "  Header: %s", this->header_names_[i]);
}

void OnlineVideo::start() {
  if (this->is_failed() || this->running_.exchange(true))
    return;
  this->next_frame_ms_ = millis();
  this->stats_since_ms_ = millis();
#ifdef USE_ESP32
  xTaskNotifyGive(this->task_);
#endif
}

void OnlineVideo::stop() { this->running_.store(false); }

void OnlineVideo::loop() {
  if (this->error_pending_.exchange(false))
    this->error_callback_.call();
  if (this->swap_pending_.load(std::memory_order_acquire)) {
    this->front_ ^= 1;
    this->data_start_ = this->frames_[this->front_];
    // The task may now decode into the old front frame.
    this->swap_pending_.store(false, std::memory_order_release);
    this->frame_callback_.call();
  }
  this->disable_loop();
}

// ---- background task ----

void OnlineVideo::task_entry_(void *arg) { static_cast<OnlineVideo *>(arg)->task_loop_(); }

void OnlineVideo::task_loop_() {
  for (;;) {
    if (!this->running_.load()) {
#ifdef USE_ESP32
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#else
      sleep_ms(50);
#endif
      continue;
    }
    if (!this->stream_once_()) {
      sleep_ms(TOKEN_POLL_MS);  // waiting for Home Assistant to share the camera token
      continue;
    }
    for (uint32_t waited = 0; waited < RECONNECT_DELAY_MS && this->running_.load(); waited += 100)
      sleep_ms(100);
  }
}

void OnlineVideo::report_error_(const char *what) {
  ESP_LOGW(TAG, "%s", what);
  this->error_pending_.store(true);
  this->enable_loop_soon_any_context();
}

bool OnlineVideo::build_path_(char *out, size_t size) {
  if (this->camera_ == nullptr) {
    snprintf(out, size, "%s", this->url_parts_.path);
    return true;
  }
  LockGuard lock(this->token_lock_);
  if (this->token_[0] == '\0') {
    if (!this->token_wait_logged_) {
      ESP_LOGI(TAG, "Waiting for Home Assistant to share the access token of %s", this->camera_);
      this->token_wait_logged_ = true;
    }
    return false;
  }
  this->token_wait_logged_ = false;
  snprintf(out, size, "/api/camera_proxy_stream/%s?interval=%s&token=%s", this->camera_, this->interval_,
           this->token_);
  return true;
}

bool OnlineVideo::stream_once_() {
  char path[256];
  if (!this->build_path_(path, sizeof(path)))
    return false;
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  char port[6];
  snprintf(port, sizeof(port), "%u", this->url_parts_.port);
  struct addrinfo *address = nullptr;
  if (::getaddrinfo(this->url_parts_.host, port, &hints, &address) != 0 || address == nullptr) {
    this->report_error_("Could not resolve the stream host");
    return true;
  }
  const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
  if (fd < 0) {
    ::freeaddrinfo(address);
    this->report_error_("Could not open a socket");
    return true;
  }
  struct timeval timeout {};
  timeout.tv_sec = SOCKET_TIMEOUT_MS / 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  const bool connected = ::connect(fd, address->ai_addr, address->ai_addrlen) == 0;
  ::freeaddrinfo(address);
  if (!connected) {
    ::close(fd);
    this->report_error_("Could not connect to the stream");
    return true;
  }

  // HTTP/1.0 keeps servers from adding chunked encoding; it is handled anyway in case one does.
  char request[640];
  int length = snprintf(request, sizeof(request),
                        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ESPHome online_video\r\n"
                        "Accept: multipart/x-mixed-replace\r\n",
                        path, this->url_parts_.host);
  for (uint8_t i = 0; i < this->header_count_ && length > 0 && length < static_cast<int>(sizeof(request)); i++) {
    length += snprintf(request + length, sizeof(request) - length, "%s: %s\r\n", this->header_names_[i],
                       this->header_values_[i]);
  }
  if (length > 0 && length < static_cast<int>(sizeof(request)))
    length += snprintf(request + length, sizeof(request) - length, "\r\n");
  if (length <= 0 || length >= static_cast<int>(sizeof(request))) {
    ::close(fd);
    this->report_error_("The request headers are too long");
    return true;
  }
  for (int sent = 0; sent < length;) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd, request + sent, length - sent, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, request + sent, length - sent, 0);
#endif
    if (n <= 0) {
      ::close(fd);
      this->report_error_("Could not send the request");
      return true;
    }
    sent += n;
  }

  HttpResponseParser response;
  this->parser_.reset();
  this->chunked_.reset();
  this->last_data_ms_ = millis();
  while (this->running_.load()) {
    const ssize_t n = ::recv(fd, this->rx_, sizeof(this->rx_), 0);
    const uint32_t now = millis();
    if (n > 0) {
      this->last_data_ms_ = now;
      size_t used = 0;
      if (!response.done()) {
        used = response.feed(this->rx_, n);
        if (response.failed()) {
          this->report_error_("The server did not answer with HTTP");
          break;
        }
        if (!response.done())
          continue;
        if (response.status() != 200) {
          ESP_LOGW(TAG, "The server answered with status %d%s", response.status(),
                   response.status() == 401 && this->camera_ != nullptr ? " (the camera token was not accepted yet)" : "");
          this->report_error_("The stream URL did not work");
          break;
        }
        if (!response.is_multipart()) {
          this->report_error_("Not an MJPEG stream (expected Content-Type multipart/x-mixed-replace)");
          break;
        }
        ESP_LOGI(TAG, "Receiving the stream from %s", this->url_parts_.host);
      }
      if (used < static_cast<size_t>(n)) {
        const bool ok = response.chunked() ? this->chunked_.feed(this->rx_ + used, n - used)
                                           : this->parser_.feed(this->rx_ + used, n - used);
        if (!ok) {
          this->report_error_("The stream data was malformed");
          break;
        }
      }
    } else if (n == 0) {
      this->report_error_("The server closed the stream");
      break;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (now - this->last_data_ms_ > NO_DATA_TIMEOUT_MS) {
        this->report_error_("No data from the stream for 10 seconds");
        break;
      }
    } else {
      this->report_error_("The connection to the stream was lost");
      break;
    }
    this->log_stats_(now);
  }
  ::close(fd);
  return true;
}

bool OnlineVideo::chunk_sink_(void *ctx, const uint8_t *data, size_t len) {
  return static_cast<OnlineVideo *>(ctx)->parser_.feed(data, len);
}

bool OnlineVideo::frame_wanted_(void *ctx) {
  auto *self = static_cast<OnlineVideo *>(ctx);
  // Skip (without copying) frames that arrive while the last one is still waiting to be shown,
  // or sooner than max_fps allows. Nothing ever queues up.
  return self->running_.load() && !self->swap_pending_.load(std::memory_order_acquire) &&
         static_cast<int32_t>(millis() - self->next_frame_ms_) >= 0;
}

void OnlineVideo::frame_ready_(void *ctx, const uint8_t *data, size_t len) {
  auto *self = static_cast<OnlineVideo *>(ctx);
  const uint32_t started = millis();
  const bool ok = self->decode_(data, len);
  const uint32_t finished = millis();
  const uint32_t took = finished - started;
  // Keep a fixed cadence (one slot per frame interval) so a frame that arrives a moment before its
  // slot does not push the next one a whole source frame later. After a pause, start a new cadence.
  self->next_frame_ms_ += self->frame_interval_ms_;
  if (static_cast<int32_t>(started - self->next_frame_ms_) > static_cast<int32_t>(self->frame_interval_ms_))
    self->next_frame_ms_ = started + self->frame_interval_ms_;
  // Leave at least a third of the time idle, even if max_fps asks for more than the decoder can do.
  const uint32_t earliest = finished + took / 2;
  if (static_cast<int32_t>(earliest - self->next_frame_ms_) > 0)
    self->next_frame_ms_ = earliest;
  if (!ok)
    return;
  self->stats_decoded_++;
  self->stats_decode_ms_ += took;
  self->swap_pending_.store(true, std::memory_order_release);
  self->enable_loop_soon_any_context();
#ifdef USE_ESP32
  vTaskDelay(1);  // let the idle task run between frames
#endif
}

bool OnlineVideo::decode_(const uint8_t *data, size_t len) {
  JPEGDEC &jpeg = *this->jpeg_;
  if (!jpeg.openRAM(const_cast<uint8_t *>(data), static_cast<int>(len), draw_callback_)) {
    this->stats_failed_++;
    return false;
  }
  if (jpeg.getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    if (!this->progressive_warned_) {
      ESP_LOGW(TAG, "Progressive JPEG frames are not supported");
      this->progressive_warned_ = true;
    }
    jpeg.close();
    this->stats_failed_++;
    return false;
  }

  // Let the decoder shrink by 2, 4 or 8 when the frame is bigger than the image: much cheaper than
  // decoding at full size. Ideally the server already sends the right size.
  const int source_w = jpeg.getWidth();
  const int source_h = jpeg.getHeight();
  int scale = 1;
  if (this->fit_ == FIT_COVER) {
    // The biggest reduction that still covers the whole image; the overflow is cropped.
    while (scale < 8 && source_w / (scale * 2) >= this->width_ && source_h / (scale * 2) >= this->height_)
      scale *= 2;
  } else {
    while (scale < 8 && (source_w / scale > this->width_ || source_h / scale > this->height_))
      scale *= 2;
  }
  this->decoded_w_ = source_w / scale;
  this->decoded_h_ = source_h / scale;
  this->offset_x_ = (this->width_ - this->decoded_w_) / 2;
  this->offset_y_ = (this->height_ - this->decoded_h_) / 2;

  const uint8_t back = this->front_ ^ 1;
  this->decode_target_ = this->frames_[back];
  const bool fills_frame = this->decoded_w_ >= this->width_ && this->decoded_h_ >= this->height_;
  if (!fills_frame && (this->cleared_w_[back] != this->decoded_w_ || this->cleared_h_[back] != this->decoded_h_)) {
    memset(this->decode_target_, 0, this->frame_bytes_);  // black borders, once per size change
    this->cleared_w_[back] = this->decoded_w_;
    this->cleared_h_[back] = this->decoded_h_;
  }

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  jpeg.setUserPointer(this);
  const int options = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER : scale == 2 ? JPEG_SCALE_HALF : 0;
  const bool ok = jpeg.decode(0, 0, options) != 0;
  jpeg.close();
  if (!ok)
    this->stats_failed_++;
  return ok;
}

int OnlineVideo::draw_callback_(jpeg_draw_tag *draw) {
  auto *self = static_cast<OnlineVideo *>(draw->pUser);
  const int width = self->width_;
  const int height = self->height_;
  int block_w = draw->iWidthUsed > 0 ? draw->iWidthUsed : draw->iWidth;
  if (draw->x + block_w > self->decoded_w_)
    block_w = self->decoded_w_ - draw->x;
  for (int row = 0; row < draw->iHeight; row++) {
    const int source_y = draw->y + row;
    if (source_y >= self->decoded_h_)
      break;
    const int y = source_y + self->offset_y_;
    if (y < 0)
      continue;
    if (y >= height)
      break;
    int x = draw->x + self->offset_x_;
    int skip = 0;
    int count = block_w;
    if (x < 0) {
      skip = -x;
      count -= skip;
      x = 0;
    }
    if (x + count > width)
      count = width - x;
    if (count <= 0)
      continue;
    memcpy(self->decode_target_ + (static_cast<size_t>(y) * width + x) * 2, draw->pPixels + row * draw->iWidth + skip,
           static_cast<size_t>(count) * 2);
  }
  return 1;
}

void OnlineVideo::log_stats_(uint32_t now) {
  const uint32_t elapsed = now - this->stats_since_ms_;
  if (elapsed < STATS_INTERVAL_MS)
    return;
  const uint32_t decoded = this->stats_decoded_;
  const uint32_t skipped = this->parser_.skipped() - this->stats_skipped_base_;
  const uint32_t too_big = this->parser_.too_big() - this->stats_too_big_base_;
  ESP_LOGD(TAG,
           "Last %" PRIu32 " s: %" PRIu32 " frames shown (%.1f fps), %" PRIu32 " ms average decode, %" PRIu32
           " skipped, %" PRIu32 " too big, %" PRIu32 " failed, largest %u bytes",
           elapsed / 1000, decoded, decoded * 1000.0f / elapsed, decoded ? this->stats_decode_ms_ / decoded : 0,
           skipped, too_big, this->stats_failed_, static_cast<unsigned>(this->parser_.largest_frame()));
  this->stats_since_ms_ = now;
  this->stats_decoded_ = 0;
  this->stats_decode_ms_ = 0;
  this->stats_failed_ = 0;
  this->stats_skipped_base_ = this->parser_.skipped();
  this->stats_too_big_base_ = this->parser_.too_big();
}

}  // namespace esphome::online_video
