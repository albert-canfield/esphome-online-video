# online_video for ESPHome

Show a live camera on an ESPHome display or LVGL panel, without making the ESP struggle.

I built this for my ESP32-S3 wall panels (480x480, LVGL), which until now could only show a
camera snapshot every few seconds with `online_image`. `online_video` reads an MJPEG stream and
hands each frame to LVGL as a normal image.

## Why it stays light

- **Home Assistant does the work.** The panel asks Home Assistant for the camera, and Home
  Assistant turns snapshots into a 2 fps MJPEG stream. With a Reolink camera, use the "Fluent"
  (sub-stream) entity, so the snapshots are already small. No add-ons, no H.264 on the device.
- **No token to manage.** Home Assistant shares the camera's short-lived access token with the
  panel over the native API, like a `homeassistant` sensor. Nothing secret goes in your YAML.
- **The main loop never waits.** A background task reads and decodes the stream. The main loop only
  swaps two buffers and redraws, so touch and LVGL stay smooth.
- **No memory churn.** Two frame buffers and the JPEG buffer are allocated once at boot (in PSRAM
  when you have it). Nothing is allocated per frame.
- **Only what you show.** Home Assistant is asked for no more snapshots than `max_fps` (2 by
  default). Frames that arrive early, or while the last one is still waiting to be shown, are
  skipped without even being copied, and decoding never takes more than about two thirds of the CPU.
- **Off when not visible.** Start and stop actions, so it only runs while the camera page is open.
- **Fills the screen for free.** The decoder can only shrink by 2, 4 or 8, so a 640x480 sub-stream
  is decoded at full size and its edges are cropped to fill a 480x480 screen (`fit: cover`).

## What it costs

Measured on an ESP32-S3 480x480 LVGL panel build, compared with the same build without it:

- **Flash:** +48 kB (the JPEG decoder and this component)
- **Static RAM:** +3.5 kB
- **Allocated once at boot:** two 460 kB frames and the 100 kB JPEG buffer in PSRAM (about 1 MB
  for 480x480), plus about 18 kB for the decoder and a 5 kB task stack in internal RAM.

## Example (Home Assistant)

```yaml
api:

external_components:
  - source: github://albert-canfield/esphome-online-video
    components: [online_video]

online_video:
  - id: door_camera
    home_assistant: http://192.168.1.10:8123   # your Home Assistant address
    camera: camera.front_door_fluent           # the Reolink sub-stream entity
    resize: 480x480
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

There is nothing to set up in Home Assistant: the device just needs to be added through the
ESPHome integration as usual. Use the IP address of Home Assistant; `.local` names may not
resolve on the device. A full panel example is in [tests/esp32s3-panel.yaml](tests/esp32s3-panel.yaml).

### Smoother video with go2rtc (optional)

Home Assistant streams from snapshots, so it stops at 2 fps. For 5 to 10 fps, any MJPEG URL
works too, for example a go2rtc stream scaled to the panel:

```yaml
# go2rtc.yaml
streams:
  door_480: ffmpeg:rtsp://user:pass@192.168.1.20/stream1#video=mjpeg#width=480#height=480

# ESPHome
online_video:
  - id: door_camera
    url: http://192.168.1.11:1984/api/stream.mjpeg?src=door_480
    resize: 480x480
    max_fps: 8
```

## Options

- **home_assistant** and **camera**: the Home Assistant address and a `camera.` entity. Needs
  `api:`. Or instead:
- **url**: an `http://` URL of any MJPEG (`multipart/x-mixed-replace`) stream.
- **resize** (required): size of the image, normally your screen or widget size, e.g. `480x480`.
- **fit** (default `cover`): `cover` fills the image and crops what does not fit, `contain` shows
  the whole frame with black borders.
- **max_fps** (default `2`, 1 to 30): the most frames per second to show.
- **buffer_size** (default `100kB`): the largest JPEG frame accepted. Bigger frames are skipped.
- **autostart** (default `false`): start streaming at boot instead of waiting for `online_video.start`.
- **headers**: up to 4 extra request headers for `url` streams, for example `Authorization`.
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
- `tests/run_ha_test.py`: end to end in Home Assistant mode. It plays the part of Home Assistant
  over the native API, and checks the device waits for the camera token, streams with it,
  reconnects with a new token after it rotates, and never logs it.
- `tests/esp32s3-panel.yaml`: compile test for an ESP32-S3 panel in Home Assistant mode.
