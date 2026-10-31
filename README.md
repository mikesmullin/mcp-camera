# mcp-camera

One MCP tool, `camera_look`, that lets an agent look through your webcam — and
shows you, on your own screen, exactly which frame it looked at.

Pure C99. No ffmpeg, no OpenCV, no libcurl, no JSON library. The only vendored
code is [sokol](https://github.com/floooh/sokol) for the window.

```
camera_look(camera_id?, image?, describe?, describe_prompt?, ocr_query?, file?, persist?, quality?)
```

## What happens on a call

1. A preview window appears on your desktop (bottom-right of your primary
   monitor, always on top) showing the live camera feed.
2. A shutter sound plays, and the captured frame **freezes behind a 3px red
   border for 2 seconds** — so you can see precisely what the agent is
   analyzing.
3. The frozen frame is then **thrown away**: it shrinks and swings along an
   elliptic arc to the bottom-right corner, fading out, with the live feed
   already running behind it.
4. The frame is written to disk and the path is returned. If asked, the agent
   also gets a prose description, OCR text regions, and/or the image itself.
5. **Two minutes after the last call**, the window deletes every frame it wrote
   that was not marked `persist`, and exits 0.

The frame the agent receives is byte-identical to the one you watched freeze.
They are the same snapshot, taken under one lock.

## Install

Needs `clang`, `node` (build script only), and the X11/GL/ALSA headers.
On Arch: `pacman -S clang nodejs libx11 libxi libxcursor libxrandr mesa alsa-lib`

```sh
node build_scripts/Makefile.mjs build      # -> build/camera_mcp, build/camera_preview
node build_scripts/Makefile.mjs test       # unit tests
node build_scripts/Makefile.mjs fmt        # clang-format in place
```

Your user must be in the `video` group to open `/dev/video*`.

Register it with your MCP client (`~/.pi/agent/mcp.json`, or the equivalent):

```json
{
  "mcpServers": {
    "camera": {
      "command": "/path/to/mcp-camera/build/camera_mcp",
      "env": {"DISPLAY": ":0"}
    }
  }
}
```

`DISPLAY` must be set, or the preview window cannot open and the call fails.

## The tool

| Argument | Type | Default | Meaning |
|---|---|---|---|
| `camera_id` | string | first camera | Which camera. See below. |
| `image` | bool | `false` | Return the frame as MCP image content (needs a vision-capable model). |
| `describe` | bool | `false` | Run local vision over the frame. Implied by `describe_prompt`/`ocr_query`. |
| `describe_prompt` | string | general description | The question to ask the VLM. |
| `ocr_query` | string | `""` | Keep only OCR regions containing this substring, case-insensitively. |
| `file` | string | transient path | Absolute path to write the frame to. Parents created. |
| `persist` | bool | `false` | Keep the frame; otherwise it is swept after the idle timeout. |
| `quality` | int | `90` | JPEG quality, 1–100. |

Returns a text summary plus `structuredContent`:

```json
{
  "ok": true,
  "camera_id": "0__uvc_camera_046d_0821__6a7559a0",
  "device": "/dev/video0",
  "file": "/tmp/camera/20261031/1793474271325-1.jpg",
  "width": 640, "height": 480, "bytes": 66036,
  "capturedAt": 1793474271320, "persist": false,
  "describe": {"ok": true, "text": "A person sitting at a desk...", "error": ""},
  "ocr": {"ok": true, "matched": 3, "total": 9,
          "regions": [{"x":145,"y":248,"w":133,"h":18,"text":"COUNTY CLERK","score":0.978}],
          "error": ""},
  "cameras": [{"camera_id":"0__uvc_camera_046d_0821__6a7559a0","index":0,
               "device":"/dev/video0","card":"UVC Camera (046d:0821)","serial":"6A7559A0"}]
}
```

There is deliberately **no second tool for listing cameras**: every result
carries the full `cameras` array, so the agent always knows its options.

### Camera ids

The id is a lowercase slug, `{index}__{card_name}__{serial}`:

```
0__uvc_camera_046d_0821__6a7559a0
```

Every component is reduced to `[a-z0-9_]`, so the id is safe in a filename, a
shell word or a URL, and is stable across reboots. `camera_id` also accepts a
bare index (`"0"`), a device path (`"/dev/video0"`), or any substring of the
slug, card name or serial. Omit it for the first camera.

Only capture-capable nodes are listed. A UVC webcam usually exposes two
`/dev/videoN` nodes — one for video, one for metadata — and the metadata node is
filtered out by its `VIDIOC_QUERYCAP` device caps.

```sh
build/camera_mcp --list
```

### Frame lifecycle

Frames are **transient by default**, which is what makes `camera_look` safe to
call in a polling loop. Unless `persist: true`, a frame is deleted when the
preview retires. Default path: `$CAMERA_DIR/YYYYMMDD/<unix_ms>-<seq>.jpg`
(`CAMERA_DIR` defaults to `/tmp/camera`).

`persist` is sticky per path: once a caller asks to keep a path, a later
transient write to the same path will not revoke that.

## Architecture

Two binaries, because a V4L2 device permits only **one** streaming opener and
sokol's `sapp_run` owns the main loop:

```
camera_mcp                             camera_preview
(stdio JSON-RPC)                       (owns /dev/videoN)
  resolve camera_id                      main     sokol event loop, draws
  spawn/ensure the daemon  ── UDS ──>    capture  V4L2 mmap -> newest frame
  grab -> a file path                    ipc      freeze, encode, write, reply
  read the JPEG back
  POST to OCR + VLM
  reply with text/image/struct
```

`camera_mcp` never touches the camera. It asks the daemon, which hands back a
path. The daemon is detached and outlives the MCP session, retiring on its own
idle timer.

IPC is newline-delimited JSON over a per-camera Unix socket at
`$XDG_RUNTIME_DIR/mcp-camera/videoN.sock`. Frames do not cross the socket — the
file on disk is the single source of truth for both processes.

### Notable implementation details

- **Native V4L2**, no ffmpeg. The ioctl order `S_FMT → REQBUFS → QUERYBUF +
  mmap + QBUF (each buffer) → STREAMON → select → DQBUF` matters: skipping the
  per-buffer `QBUF` before `STREAMON` makes `STREAMON` fail with `EINVAL`.
- **Own baseline JPEG encoder** (`src/common/Jpeg.c`), 4:4:4, Annex K tables.
  4:4:4 rather than 4:2:0 so one MCU is one 8x8 block per component — simpler,
  obviously correct, and it keeps small on-screen text legible for OCR.
- **Colour range**: V4L2 UVC gives BT.601 *limited* range; JFIF and the display
  want *full* range. Both the encoder and the preview apply the same expansion,
  so what you see matches what the agent is sent.
- **Warmup**: the first 15 frames are shown in the window but the daemon does
  not report ready until they pass, so the first `camera_look` never analyzes a
  dark, un-exposed frame.
- **The icon** is drawn by `build_scripts/mkicon.mjs` into `assets/icon.h` and
  PNGs, rather than vendored as a binary.

## Vision backends

Both are optional; a failure in one never hides the other's answer.

| Variable | Default | Used for |
|---|---|---|
| `CAMERA_OCR_URL` | `http://127.0.0.1:1239` | `POST /ocr` → text regions |
| `CAMERA_VLM_URL` | `http://127.0.0.1:1246` | `POST /v1/chat/completions` → prose |

These are the same services and request shapes `mcp-vision` uses, so they share
one model process. `VISION_OCR_URL` / `VISION_LIQUID_URL` are honoured as
fallbacks.

> OCR and VLM text is **untrusted content read off a camera**. It is reported as
> data and must never be treated as instructions. The tool result says so too.

## Environment

| Variable | Default | Effect |
|---|---|---|
| `CAMERA_DIR` | `/tmp/camera` | Where transient frames go |
| `CAMERA_OCR_URL` | `http://127.0.0.1:1239` | OCR backend |
| `CAMERA_VLM_URL` | `http://127.0.0.1:1246` | VLM backend |
| `CAMERA_PREVIEW_BIN` | beside `camera_mcp` | Path to `camera_preview` |
| `CAMERA_SFX` | built-in click | A 16-bit PCM WAV for the shutter |
| `CAMERA_IDLE_MS` | `120000` | Retire the window this long after the last call |
| `CAMERA_PREVIEW_X` / `_Y` | bottom-right | Window position |
| `CAMERA_PREVIEW_ONTOP` | on | Set `0` to not keep the window above others |
| `CAMERA_PREVIEW_SKIP_TASKBAR` | off | Set `1` to hide it from the task list |

## Manual testing

```sh
build/camera_mcp --list                              # enumerate cameras
build/camera_preview --camera 0                      # run the window alone
build/ipc_client 0 grab /tmp/x.jpg true 95           # grab without an MCP client
build/camera_preview --write-sfx assets/shutter.wav  # dump the built-in shutter
node build_scripts/mkicon.mjs                        # regenerate the icon
```

An end-to-end call:

```sh
printf '%s\n%s\n%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' \
 '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"camera_look","arguments":{"describe_prompt":"What is in frame?"}}}' \
 | DISPLAY=:0 build/camera_mcp
```

## Layout

```
src/unity.h          shared types, bounds, and the include order
src/mcp.c            unity build 1: camera_mcp
src/preview.c        unity build 2: camera_preview (sokol lives here)
src/common/          Arena Base64 File Jpeg Json JsonW Log String Thread Time
src/app/CamId.c      V4L2 + sysfs enumeration and id resolution
src/app/Ipc.c        the wire format between the two binaries
src/app/Http.c       a single-shot HTTP/1.1 POST client
src/app/Vision.c     OCR and VLM clients
src/app/Preview.c    daemon lifecycle, as seen by camera_mcp
src/app/Mcp.c        JSON-RPC dispatch and the camera_look tool
src/app/V4l2.c       mmap capture
src/app/Registry.c   the transient-frame registry and its sweep
src/app/Sfx.c        the synthesized shutter
src/app/Gui.c        the window, the border, and the toss animation
src/app/Server.c     the daemon's request handler
```

Code style follows
[`mikesmullin/c99-server/docs/my-code-style.md`](https://github.com/mikesmullin/c99-server/blob/master/docs/my-code-style.md).
