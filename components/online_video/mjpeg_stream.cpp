#include "mjpeg_stream.h"

#include <cstdlib>
#include <cstring>

namespace esphome::online_video {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

/// Case-insensitive "does `line` start with `prefix`".
bool starts_with_ci(const char *line, const char *prefix) {
  for (; *prefix != '\0'; line++, prefix++) {
    if (lower(*line) != *prefix)
      return false;
  }
  return true;
}

/// Case-insensitive search for a lowercase `needle` in `haystack`.
bool contains_ci(const char *haystack, const char *needle) {
  const size_t n = strlen(needle);
  for (; *haystack != '\0'; haystack++) {
    if (starts_with_ci(haystack, needle))
      return true;
  }
  return n == 0;
}

int hex_value(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

}  // namespace

bool parse_url(const char *url, UrlParts &out) {
  out = UrlParts{};
  if (url == nullptr || !starts_with_ci(url, "http://"))
    return false;
  const char *p = url + 7;
  size_t host_len = strcspn(p, ":/?");
  if (host_len == 0 || host_len >= sizeof(out.host))
    return false;
  memcpy(out.host, p, host_len);
  p += host_len;
  if (*p == ':') {
    p++;
    char *end = nullptr;
    long port = strtol(p, &end, 10);
    if (end == p || port <= 0 || port > 65535)
      return false;
    out.port = static_cast<uint16_t>(port);
    p = end;
  }
  if (*p == '\0') {
    out.path[0] = '/';
    return true;
  }
  if (*p == '?') {
    // "http://host?x" means path "/?x"
    if (strlen(p) + 1 >= sizeof(out.path))
      return false;
    out.path[0] = '/';
    strcpy(out.path + 1, p);  // NOLINT(clang-analyzer-security.insecureAPI.strcpy): length checked above
    return true;
  }
  if (*p != '/' || strlen(p) >= sizeof(out.path))
    return false;
  strcpy(out.path, p);  // NOLINT(clang-analyzer-security.insecureAPI.strcpy): length checked above
  return true;
}

// ---- HttpResponseParser ----

void HttpResponseParser::reset() { *this = HttpResponseParser{}; }

size_t HttpResponseParser::feed(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len && !this->done_ && !this->failed_) {
    const char c = static_cast<char>(data[i++]);
    if (c == '\n') {
      if (this->line_len_ > 0 && this->line_[this->line_len_ - 1] == '\r')
        this->line_len_--;
      this->line_[this->line_len_] = '\0';
      this->handle_line_();
      this->line_len_ = 0;
    } else if (this->line_len_ < sizeof(this->line_) - 1) {
      this->line_[this->line_len_++] = c;
    }  // longer lines are cut; the parts we look at come first
  }
  return i;
}

void HttpResponseParser::handle_line_() {
  if (this->first_line_) {
    this->first_line_ = false;
    // "HTTP/1.0 200 OK"
    if (!starts_with_ci(this->line_, "http/")) {
      this->failed_ = true;
      return;
    }
    const char *space = strchr(this->line_, ' ');
    this->status_ = space != nullptr ? atoi(space + 1) : 0;
    return;
  }
  if (this->line_len_ == 0) {
    this->done_ = true;
    return;
  }
  if (starts_with_ci(this->line_, "transfer-encoding:") && contains_ci(this->line_, "chunked"))
    this->chunked_ = true;
  if (starts_with_ci(this->line_, "content-type:") && contains_ci(this->line_, "multipart"))
    this->multipart_ = true;
}

// ---- ChunkedDecoder ----

void ChunkedDecoder::reset() {
  this->state_ = State::SIZE;
  this->remaining_ = 0;
  this->size_seen_ = false;
}

bool ChunkedDecoder::feed(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len) {
    switch (this->state_) {
      case State::SIZE:
      case State::SIZE_EXTENSION: {
        const uint8_t c = data[i++];
        if (c == '\n') {
          if (!this->size_seen_)
            break;  // tolerate an empty line before the size
          this->state_ = this->remaining_ == 0 ? State::TRAILER : State::DATA;
          this->size_seen_ = false;
        } else if (c == '\r' || this->state_ == State::SIZE_EXTENSION) {
          // nothing to do
        } else if (c == ';') {
          this->state_ = State::SIZE_EXTENSION;
        } else {
          const int v = hex_value(c);
          if (v < 0 || this->remaining_ > 0x0FFFFFFF)
            return false;
          this->remaining_ = this->remaining_ * 16 + static_cast<size_t>(v);
          this->size_seen_ = true;
        }
        break;
      }
      case State::DATA: {
        const size_t n = (len - i) < this->remaining_ ? (len - i) : this->remaining_;
        if (this->sink_ != nullptr && !this->sink_(this->ctx_, data + i, n))
          return false;
        i += n;
        this->remaining_ -= n;
        if (this->remaining_ == 0)
          this->state_ = State::DATA_END;
        break;
      }
      case State::DATA_END: {
        const uint8_t c = data[i++];
        if (c == '\n') {
          this->state_ = State::SIZE;
        } else if (c != '\r') {
          return false;
        }
        break;
      }
      case State::TRAILER:
        return true;  // the last chunk was seen; ignore trailers
    }
  }
  return true;
}

// ---- MjpegParser ----

void MjpegParser::reset() {
  this->state_ = State::HEADERS;
  this->line_len_ = 0;
  this->header_seen_ = false;
  this->content_length_ = -1;
  this->remaining_ = 0;
  this->filled_ = 0;
  this->keep_ = false;
  this->overflow_ = false;
  this->prev_ = 0;
}

void MjpegParser::header_line_() {
  this->line_[this->line_len_] = '\0';
  if (starts_with_ci(this->line_, "content-length:"))
    this->content_length_ = strtol(this->line_ + 15, nullptr, 10);
}

void MjpegParser::start_body_() {
  this->keep_ = this->wanted_ == nullptr || this->wanted_(this->ctx_);
  this->overflow_ = this->content_length_ > static_cast<long>(this->capacity_);
  this->filled_ = 0;
  this->prev_ = 0;
  if (this->content_length_ >= 0) {
    this->state_ = State::BODY_SIZED;
    this->remaining_ = static_cast<size_t>(this->content_length_);
    if (this->remaining_ == 0)
      this->finish_frame_();
  } else {
    this->state_ = State::BODY_SCAN;
  }
}

void MjpegParser::finish_frame_() {
  if (this->overflow_) {
    this->too_big_++;
  } else if (this->keep_) {
    this->frames_++;
    if (this->filled_ > this->largest_)
      this->largest_ = this->filled_;
    if (this->frame_ != nullptr)
      this->frame_(this->ctx_, this->buffer_, this->filled_);
  } else {
    this->skipped_++;
  }
  this->state_ = State::HEADERS;
  this->line_len_ = 0;
  this->header_seen_ = false;
  this->content_length_ = -1;
}

bool MjpegParser::feed(const uint8_t *data, size_t len) {
  if (this->buffer_ == nullptr)
    return false;
  size_t i = 0;
  while (i < len) {
    switch (this->state_) {
      case State::HEADERS: {
        const uint8_t c = data[i++];
        // A part with no headers: the JPEG start-of-image marker (FF D8) right at the start of a line.
        if (c == 0xD8 && this->line_len_ == 1 && static_cast<uint8_t>(this->line_[0]) == 0xFF) {
          this->content_length_ = -1;
          this->start_body_();
          if (this->keep_ && this->capacity_ >= 2) {
            this->buffer_[0] = 0xFF;
            this->buffer_[1] = 0xD8;
            this->filled_ = 2;
          }
          this->prev_ = 0xD8;
          break;
        }
        if (c == '\n') {
          if (this->line_len_ > 0 && this->line_[this->line_len_ - 1] == '\r')
            this->line_len_--;
          if (this->line_len_ == 0) {
            if (this->header_seen_)
              this->start_body_();  // blank line ends the part headers
          } else {
            this->header_line_();
            this->header_seen_ = true;
          }
          this->line_len_ = 0;
        } else if (this->line_len_ < sizeof(this->line_) - 1) {
          this->line_[this->line_len_++] = static_cast<char>(c);
        }
        break;
      }
      case State::BODY_SIZED: {
        const size_t n = (len - i) < this->remaining_ ? (len - i) : this->remaining_;
        if (this->keep_ && !this->overflow_)
          memcpy(this->buffer_ + this->filled_, data + i, n);
        this->filled_ += n;
        i += n;
        this->remaining_ -= n;
        if (this->remaining_ == 0)
          this->finish_frame_();
        break;
      }
      case State::BODY_SCAN: {
        const uint8_t c = data[i++];
        if (this->keep_ && !this->overflow_) {
          if (this->filled_ < this->capacity_) {
            this->buffer_[this->filled_++] = c;
          } else {
            this->overflow_ = true;
          }
        }
        const bool end_of_image = this->prev_ == 0xFF && c == 0xD9;
        this->prev_ = c;
        if (end_of_image)
          this->finish_frame_();
        break;
      }
    }
  }
  return true;
}

}  // namespace esphome::online_video
