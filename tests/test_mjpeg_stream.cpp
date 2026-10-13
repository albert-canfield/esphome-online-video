// Desktop unit tests for mjpeg_stream.{h,cpp}. Build and run with:
//   clang++ -std=c++20 -Wall -Wextra -Werror -I components/online_video \
//     components/online_video/mjpeg_stream.cpp tests/test_mjpeg_stream.cpp -o /tmp/test_mjpeg && /tmp/test_mjpeg

#include "mjpeg_stream.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace esphome::online_video;

static int failures = 0;
#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++; \
    } \
  } while (0)

static std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

// A fake JPEG: SOI, some payload (with a stray FF that is not FF D9), EOI.
static std::string fake_jpeg(char fill, size_t n) {
  std::string s = "\xFF\xD8";
  s += std::string(n, fill);
  s += "\xFF\x00";
  s += "\xFF\xD9";
  return s;
}

struct Collector {
  std::vector<std::string> frames;
  int want_calls{0};
  std::vector<bool> wants;  // answers for successive frames; empty = always yes
  static bool wanted(void *ctx) {
    auto *c = static_cast<Collector *>(ctx);
    const size_t n = c->want_calls++;
    return c->wants.empty() || (n < c->wants.size() ? c->wants[n] : true);
  }
  static void frame(void *ctx, const uint8_t *data, size_t len) {
    static_cast<Collector *>(ctx)->frames.emplace_back(reinterpret_cast<const char *>(data), len);
  }
};

// Feed `stream` into a parser in chunks of `step` bytes.
static void feed_in_steps(MjpegParser &p, const std::string &stream, size_t step) {
  for (size_t i = 0; i < stream.size(); i += step) {
    const size_t n = std::min(step, stream.size() - i);
    p.feed(reinterpret_cast<const uint8_t *>(stream.data() + i), n);
  }
}

static void test_parse_url() {
  UrlParts u;
  CHECK(parse_url("http://go2rtc.local:1984/api/stream.mjpeg?src=door", u));
  CHECK(std::strcmp(u.host, "go2rtc.local") == 0);
  CHECK(u.port == 1984);
  CHECK(std::strcmp(u.path, "/api/stream.mjpeg?src=door") == 0);

  CHECK(parse_url("HTTP://192.168.1.5", u));
  CHECK(std::strcmp(u.host, "192.168.1.5") == 0);
  CHECK(u.port == 80);
  CHECK(std::strcmp(u.path, "/") == 0);

  CHECK(parse_url("http://cam?action=stream", u));
  CHECK(std::strcmp(u.path, "/?action=stream") == 0);

  CHECK(!parse_url("https://secure.example/x", u));
  CHECK(!parse_url("http://:80/x", u));
  CHECK(!parse_url("http://host:99999/x", u));
  CHECK(!parse_url(("http://" + std::string(80, 'a') + "/").c_str(), u));
}

static void test_http_response() {
  const std::string head =
      "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-cache\r\n\r\n";
  const auto all = bytes(head + "BODY");
  for (size_t step : {size_t{1}, size_t{7}, all.size()}) {
    HttpResponseParser p;
    size_t used = 0;
    while (!p.done() && used < all.size())
      used += p.feed(all.data() + used, std::min(step, all.size() - used));
    CHECK(p.done());
    CHECK(!p.failed());
    CHECK(p.status() == 200);
    CHECK(p.is_multipart());
    CHECK(!p.chunked());
    CHECK(used == head.size());  // the body starts right after the blank line
  }

  HttpResponseParser chunked;
  const auto c = bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
  chunked.feed(c.data(), c.size());
  CHECK(chunked.done() && chunked.chunked() && !chunked.is_multipart());

  HttpResponseParser bad;
  const auto b = bytes("<html>\r\n\r\n");
  bad.feed(b.data(), b.size());
  CHECK(bad.failed());

  HttpResponseParser auth;
  const auto a = bytes("HTTP/1.0 401 Unauthorized\r\n\r\n");
  auth.feed(a.data(), a.size());
  CHECK(auth.done() && auth.status() == 401);
}

static bool append_sink(void *ctx, const uint8_t *data, size_t len) {
  static_cast<std::string *>(ctx)->append(reinterpret_cast<const char *>(data), len);
  return true;
}

static void test_chunked() {
  const std::string body = "5\r\nhello\r\n6;name=value\r\n world\r\n0\r\n\r\n";
  for (size_t step = 1; step <= body.size(); step++) {
    std::string out;
    ChunkedDecoder d;
    d.set_sink(append_sink, &out);
    bool ok = true;
    for (size_t i = 0; i < body.size(); i += step)
      ok &= d.feed(reinterpret_cast<const uint8_t *>(body.data() + i), std::min(step, body.size() - i));
    CHECK(ok);
    CHECK(out == "hello world");
  }
  std::string out;
  ChunkedDecoder d;
  d.set_sink(append_sink, &out);
  const auto bad = bytes("zz\r\n");
  CHECK(!d.feed(bad.data(), bad.size()));
}

static void test_frames_with_content_length() {
  const std::string a = fake_jpeg('a', 300), b = fake_jpeg('b', 500);
  const std::string stream = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(a.size()) +
                             "\r\n\r\n" + a + "\r\n--frame\r\nContent-Type: image/jpeg\r\ncontent-length:" +
                             std::to_string(b.size()) + "\r\n\r\n" + b + "\r\n";
  for (size_t step = 1; step <= 64; step++) {
    uint8_t buf[1024];
    Collector c;
    MjpegParser p;
    p.set_buffer(buf, sizeof(buf));
    p.set_callbacks(Collector::wanted, Collector::frame, &c);
    feed_in_steps(p, stream, step);
    CHECK(c.frames.size() == 2);
    if (c.frames.size() == 2) {
      CHECK(c.frames[0] == a);
      CHECK(c.frames[1] == b);
    }
    CHECK(p.frames() == 2 && p.largest_frame() == b.size());
  }
}

static void test_frames_without_content_length() {
  const std::string a = fake_jpeg('x', 200), b = fake_jpeg('y', 50);
  const std::string stream =
      "\r\n--myboundary\r\nContent-Type: image/jpeg\r\n\r\n" + a + "\r\n--myboundary\r\n\r\n" + b + "\r\n";
  for (size_t step = 1; step <= 64; step++) {
    uint8_t buf[1024];
    Collector c;
    MjpegParser p;
    p.set_buffer(buf, sizeof(buf));
    p.set_callbacks(Collector::wanted, Collector::frame, &c);
    feed_in_steps(p, stream, step);
    CHECK(c.frames.size() == 2);
    if (c.frames.size() == 2) {
      CHECK(c.frames[0] == a);
      CHECK(c.frames[1] == b);
    }
  }
}

static void test_part_without_headers() {
  const std::string a = fake_jpeg('q', 40);
  const std::string stream = "--frame\r\n" + a + "\r\n";
  uint8_t buf[256];
  Collector c;
  MjpegParser p;
  p.set_buffer(buf, sizeof(buf));
  p.set_callbacks(Collector::wanted, Collector::frame, &c);
  feed_in_steps(p, stream, 3);
  CHECK(c.frames.size() == 1);
  if (!c.frames.empty())
    CHECK(c.frames[0] == a);
}

static void test_unwanted_frames_are_skipped() {
  const std::string a = fake_jpeg('a', 100), b = fake_jpeg('b', 100), d = fake_jpeg('d', 100);
  std::string stream;
  for (const auto *f : {&a, &b, &d})
    stream += "--f\r\nContent-Length: " + std::to_string(f->size()) + "\r\n\r\n" + *f + "\r\n";
  uint8_t buf[512];
  Collector c;
  c.wants = {false, true, false};
  MjpegParser p;
  p.set_buffer(buf, sizeof(buf));
  p.set_callbacks(Collector::wanted, Collector::frame, &c);
  feed_in_steps(p, stream, 9);
  CHECK(c.frames.size() == 1);
  if (!c.frames.empty())
    CHECK(c.frames[0] == b);
  CHECK(p.skipped() == 2);
}

static void test_too_big_frames_do_not_break_the_stream() {
  const std::string big = fake_jpeg('B', 400), small = fake_jpeg('s', 20);
  const std::string sized = "--f\r\nContent-Length: " + std::to_string(big.size()) + "\r\n\r\n" + big +
                            "\r\n--f\r\nContent-Length: " + std::to_string(small.size()) + "\r\n\r\n" + small + "\r\n";
  const std::string scanned = "--f\r\n\r\n" + big + "\r\n--f\r\n\r\n" + small + "\r\n";
  for (const auto *stream : {&sized, &scanned}) {
    uint8_t buf[128];
    Collector c;
    MjpegParser p;
    p.set_buffer(buf, sizeof(buf));
    p.set_callbacks(Collector::wanted, Collector::frame, &c);
    feed_in_steps(p, *stream, 5);
    CHECK(p.too_big() == 1);
    CHECK(c.frames.size() == 1);
    if (!c.frames.empty())
      CHECK(c.frames[0] == small);
  }
}

static void test_chunked_into_mjpeg() {
  // go2rtc-style: HTTP/1.1 chunked transfer carrying the multipart stream.
  const std::string a = fake_jpeg('c', 120);
  const std::string multipart = "--frame\r\nContent-Length: " + std::to_string(a.size()) + "\r\n\r\n" + a + "\r\n";
  char size_line[16];
  const std::string first = multipart.substr(0, multipart.size() / 2);
  const std::string second = multipart.substr(multipart.size() / 2);
  std::snprintf(size_line, sizeof(size_line), "%zx\r\n", first.size());
  std::string body = std::string(size_line) + first + "\r\n";
  std::snprintf(size_line, sizeof(size_line), "%zx\r\n", second.size());
  body += std::string(size_line) + second + "\r\n";

  uint8_t buf[512];
  Collector c;
  MjpegParser p;
  p.set_buffer(buf, sizeof(buf));
  p.set_callbacks(Collector::wanted, Collector::frame, &c);
  ChunkedDecoder d;
  d.set_sink([](void *ctx, const uint8_t *data, size_t len) { return static_cast<MjpegParser *>(ctx)->feed(data, len); },
             &p);
  for (size_t i = 0; i < body.size(); i += 11)
    CHECK(d.feed(reinterpret_cast<const uint8_t *>(body.data() + i), std::min<size_t>(11, body.size() - i)));
  CHECK(c.frames.size() == 1);
  if (!c.frames.empty())
    CHECK(c.frames[0] == a);
}

int main() {
  test_parse_url();
  test_http_response();
  test_chunked();
  test_frames_with_content_length();
  test_frames_without_content_length();
  test_part_without_headers();
  test_unwanted_frames_are_skipped();
  test_too_big_frames_do_not_break_the_stream();
  test_chunked_into_mjpeg();
  if (failures == 0)
    std::printf("all mjpeg_stream tests passed\n");
  return failures == 0 ? 0 : 1;
}
