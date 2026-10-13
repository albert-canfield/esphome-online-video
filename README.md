# online_video for ESPHome

Show a live camera on an ESPHome display or LVGL panel, without making the ESP struggle.

I built this for my ESP32-S3 wall panels (480x480, LVGL), which until now could only show a
camera snapshot every few seconds with `online_image`. `online_video` reads an MJPEG stream and
hands each frame to LVGL as a normal image.

## Why it stays light

- **The server does the heavy work.** Let go2rtc (or similar) send MJPEG already scaled to your
  screen, so the ESP only decodes small JPEGs. No H.264 on the device.
- **The main loop never waits.** A background task reads and decodes the stream. The main loop only
  swaps two buffers and redraws, so touch and LVGL stay smooth.
- **No memory churn.** Two frame buffers and the JPEG buffer are allocated once at boot (in PSRAM
  when you have it). Nothing is allocated per frame.
- **Frames never pile up.** A `max_fps` cap (5 by default); frames that arrive early, or while the
  last one is still waiting to be shown, are skipped without even being copied.
- **CPU guard.** Decoding never takes more than about two thirds of the time, so the Wi-Fi core
  always has room and the watchdog stays happy.
- **Off when not visible.** Start and stop actions, so it only runs while the camera page is open.
- **Plain HTTP only**, no TLS stack pulled in. If a frame is bigger than the screen, the decoder
  shrinks it by 2, 4 or 8 for free (a smaller frame is centred with black borders).

## Example

```yaml
external_components:
  - source: github://albert-canfield/esphome-online-video
    components: [online_video]

online_video:
  - id: door_camera
    url: http://go2rtc.local:1984/api/stream.mjpeg?src=door_480
    resize: 480x480
    max_fps: 5
    on_frame:
      - lvgl.image.update:
          id: camera_view
          src: door_camera

lvgl:
  pages:
    - id: camera_page
      on_load:
        - online_video.start: door_camera
      on_unload:
        - online_video.stop: door_camera
      widgets:
        - image:
            id: camera_view
            src: door_camera
            align: CENTER
```

A full panel example is in [tests/esp32s3-panel.yaml](tests/esp32s3-panel.yaml).

### go2rtc

A stream that go2rtc scales to the panel size:

```yaml
streams:
  door: rtsp://user:pass@192.168.1.20/stream1
  door_480: ffmpeg:door#video=mjpeg#width=480#height=480
```

Then use `http://<go2rtc-host>:1984/api/stream.mjpeg?src=door_480`.

### Home Assistant camera proxy

`http://<ha-host>:8123/api/camera_proxy_stream/camera.front_door` also works. It needs a
long-lived access token, and sends the camera at full size (the decoder will shrink it, but
go2rtc is lighter on the ESP):

```yaml
    headers:
      Authorization: !secret ha_bearer_token   # "Bearer eyJ..."
```

## Options

- **url** (required): `http://` URL of an MJPEG (`multipart/x-mixed-replace`) stream.
- **resize** (required): size of the image, normally your screen or widget size, e.g. `480x480`.
- **max_fps** (default `5`, 1 to 30): the most frames per second to show.
- **buffer_size** (default `100kB`): the largest JPEG frame accepted. Bigger frames are skipped.
- **autostart** (default `false`): start streaming at boot instead of waiting for `online_video.start`.
- **headers**: up to 4 extra request headers, for example `Authorization`.
- **on_frame**: runs on the main loop after each new frame. Use it to update the LVGL image.
- **on_error**: runs when the stream can't be reached or stops. It retries every 3 s by itself.

Actions: `online_video.start`, `online_video.stop`. Condition: `online_video.is_running`.

## Requirements and limits

- ESP32 (tested on ESP32-S3 with PSRAM) or the `host` platform. ESPHome 2026.6 or newer.
- A 480x480 frame needs about 460 kB per buffer, so two buffers want PSRAM.
- Baseline JPEG only (no progressive), RGB565 output.

## Tests

- `tests/test_mjpeg_stream.cpp`: unit tests for the HTTP, chunked and MJPEG parsing, runs on any
  computer (build command at the top of the file).
- `tests/run_host_test.py`: end to end on a computer with the ESPHome `host` platform and SDL.
  `tests/mjpeg_server.py` streams red, green and blue frames, and the test checks every frame
  arrives with the right colour and in order, with Content-Length, chunked (like go2rtc) and
  without Content-Length, and frames 2x and 4x too big or smaller than the image.
- `tests/esp32s3-panel.yaml`: compile test for an ESP32-S3 panel.
