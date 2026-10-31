# mcp-camera design notes

What was built, why, and where it departs from the original handoff plan.
Read `README.md` first for usage.

## Corrections to the handoff plan

The plan was written from exploration that reached two wrong conclusions. Both
were re-tested on this machine before any code was written, and both reversed.

### 1. Native V4L2 works; ffmpeg is not needed

The plan states: *"Raw V4L2 **fails**. `STREAMON`/`DQBUF` return `EINVAL` (22)
… the kernel's `uvcvideo` here does *not* take the legacy `STREAMON` path"*,
and concludes the capture layer should shell out to ffmpeg.

That is not true. A correct mmap capture succeeds on this kernel (7.2.11,
`uvcvideo`, 046d:0821). The earlier attempts failed because they **queued no
buffers before `STREAMON`**. The driver requires:

```
S_FMT → REQBUFS → (QUERYBUF + mmap + QBUF) × N → STREAMON → select → DQBUF → QBUF → …
```

`STREAMON` with zero queued buffers returns `EINVAL`, which reads like an
unsupported ioctl but is not. `src/app/V4l2.c` does it in the right order and
captures reliably.

Consequence: **no ffmpeg dependency at all.** Capture, colour conversion and
JPEG encoding are all in-process C99.

### 2. The MCP server cannot capture its own frames

The plan has `camera_mcp` run its own one-shot ffmpeg grab while
`camera_preview` holds a continuous feed. That cannot work: a V4L2 device
permits only one streaming opener. Verified directly —

```
$ ffmpeg -f v4l2 -i /dev/video0 -t 6 -f null - &    # streaming
$ ffmpeg -f v4l2 -i /dev/video0 -frames:v 1 x.jpg   # second open
Error opening input: Device or resource busy
```

So the daemon owns the camera exclusively and `camera_mcp` asks it for frames.

This is not just a workaround, it is better: in the plan's design the agent
would analyze a *different* frame from the one the user watched freeze, which
defeats the stated purpose of the window ("this helps me understand what its
seeing"). In the built design both come from one `memcpy` under one mutex, so
they are byte-identical.

## Decisions

| Topic | Decision |
|---|---|
| Language | C99 throughout, per `my-code-style.md`. Rust was offered and declined: with native V4L2 and the vision endpoints verified, nothing risky remained, and the crates that would make Rust easy (v4l, serde, image, ureq) are exactly the vendor surface the style guide avoids. |
| Capture | Native V4L2 mmap, YUYV 4:2:2, 640x480. |
| JPEG | Own baseline encoder, 4:4:4, T.81 Annex K tables. |
| Window | sokol_app + sokol_gfx (GLCORE), one textured quad plus border quads. |
| Audio | sokol_audio, with the shutter synthesized rather than shipped. |
| IPC | Newline-delimited JSON over a per-camera Unix socket; frames travel as paths, not bytes. |
| Camera id | `{index}__{card}__{serial}`, each component lowercased and reduced to `[a-z0-9_]`. |
| `persist` | Defaults to `false`; sticky per path once set. |
| Tool count | One (`camera_look`). Enumeration is folded into every result rather than given its own tool. |
| `describe` | Asks **both** backends: the VLM for prose, OCR for text regions. They answer different questions. |
| `decide` | A separate, much cheaper call to the System One decision model. Deliberately does *not* imply `describe`, so polling with `decide` alone never pays for the VLM. |
| JPEG quality | Fixed at 90, not a tool argument. It is a storage knob with no bearing on what the agent can see, and 90 is already past the point where it affects OCR or the VLM. The IPC field remains for `ipc_client`. |

## Things that are load-bearing

**The grab snapshot.** `_Server__grab()` takes `frame_mtx`, copies the live
frame twice — once to `work` (encoded to JPEG) and once to `frozen`
(displayed) — then releases the lock before doing any encoding or I/O. The two
copies are the same bytes; that equality is the entire point of the preview
window, so it must not be optimized into a single shared buffer that the
capture thread can overwrite mid-encode.

**Warmup gating.** A cold UVC camera's first frames are dark. The capture
thread shows them (so the window is visibly alive) but does not set `ready`
until `PREVIEW_WARMUP_FRAMES` have passed. `ping` reports `ok` only when
ready, so `Preview__spawn()` waits through warmup and the first `camera_look`
never analyzes a black frame.

**Alive vs ready.** `Preview__alive()` and `Preview__ready()` are distinct.
Collapsing them would make a warming-up daemon look absent, and `Preview__ensure()`
would spawn a second one — which would then fail on the busy device.

**The transient sweep.** `Registry__sweep()` runs in `_Preview__cleanup()`, on
every exit path that reaches it, because it is the promise `camera_look`'s
description makes about non-persisted frames. Entries are marked swept rather
than removed so a second sweep reports 0 instead of re-deleting a path a later
grab may have recreated.

**Colour range.** V4L2 UVC delivers BT.601 limited range (Y 16–235, C 16–240).
JFIF and the display both want full range. `Jpeg__encodeYuyv()` and
`Gui__yuyvToRgba()` apply the *same* expansion; if they ever diverge, the user
and the agent stop seeing the same picture. Verified by encoding synthetic
colour bars and decoding them with ffmpeg: white comes back `ffffff`, not
`ebebeb`.

**The decision answer is passed through verbatim.** `Vision__decide()` captures
the raw span of `answers.decision` out of the backend's response body and
splices it into `structuredContent` with `JsonW__kraw`. A System One model's
entire value is its calibrated probability distribution; re-serializing that
through our own structs would make us a lossy middleman for the one thing the
caller actually wants. The same raw-span trick is used for JSON-RPC `params`.

**`describe_prompt` is not the OCR filter.** An earlier revision passed the
prompt to both the VLM and the OCR query. A natural-language question is never
a substring of text on a page, so every region was silently discarded. OCR
filtering has its own `ocr_query`.

**stdout is the MCP transport.** Every diagnostic in `camera_mcp` goes to
stderr (`Log.c` has no stdout logger except `Console__out`, which is reserved
for protocol frames). One stray `printf` desynchronizes the client.

**`_sapp` is a vendor internal.** `Gui__place()` reads `_sapp.x11.display` and
`_sapp.x11.window`, which sokol does not expose publicly on Linux. This is only
sound because the unity build puts `SOKOL_IMPL` in the same translation unit.
It is the one vendor internal this project depends on, and the place to look
first after a sokol upgrade.

## Verified on this machine

- Native V4L2 capture, 5 consecutive frames, `uvcvideo` / 046d:0821.
- JPEG encoder round-trip: synthetic colour bars decode through ffmpeg to the
  exact expected RGB (`ffffff ffff00 00ffff 00ff01 ff00fe fd0000 0000fe 000000`).
- MCP handshake, `tools/list`, `tools/call` over stdio.
- A real frame captured, described by the VLM at :1246, OCR'd at :1239 with
  correct bounding boxes, and judged by the decision model at :1237 in all
  three question shapes (~100 ms each).
- `image: true` returns base64 byte-identical to the file on disk.
- The window places bottom-right of the primary XRandR output, sets
  `_NET_WM_STATE_ABOVE`, `_NET_WM_WINDOW_TYPE_UTILITY` and `_NET_WM_ICON`.
- The shutter animation, screenshotted across its phases: live → red-bordered
  freeze → the card rotating away to the bottom-right over the live feed → live.
- Idle retirement: 2 transient frames swept, 1 persisted frame kept, exit 0.

## Not built

- **Raw V4L2 formats other than YUYV.** The camera also offers MJPG, which
  would hand us JPEG bytes for free but needs a decoder for the display. YUYV
  plus an encoder is the smaller amount of code.
- **Frame-rate control.** The driver's default (30fps at 640x480) is used.
- **Windows/macOS.** `V4l2.c` and `Gui__place()` are Linux-only. Everything
  else is portable; sokol already covers the other platforms' window layer.
- **Jev's Box question type** (1.14, opt-in): returns one bounding box in the
  image as 0–1 fractions. `Vision.c` implements noul, choice and score only.
  OCR already covers "where is this text", which is the common case here.
- **Multiple decision questions per call.** Jev accepts a map of questions and
  answers them in one parallel pass; `camera_look` sends exactly one, named
  `decision`. Batching would mean a nested object argument, which is a worse
  trade for an agent-facing tool than calling twice.
