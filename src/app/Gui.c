#pragma once

#include <X11/Xatom.h>  // IWYU pragma: keep // XA_ATOM
#include <X11/extensions/Xrandr.h>  // IWYU pragma: keep // XRRGetOutputPrimary()

#include "../unity.h"  // IWYU pragma: keep

// The preview window: a live camera feed, and the shutter animation that tells
// the user exactly which frame the agent is looking at.
//
// Rendering is one textured quad plus, while frozen, four solid quads for the
// border. Vertices are regenerated each frame because the 3px border is
// specified in pixels and must stay 3px as the window resizes.
//
// NOTE: this file reads sokol_app's private `_sapp` state to reach the X11
// window handle, which sokol does not expose publicly on Linux. That is sound
// here only because the unity build puts SOKOL_IMPL in this same translation
// unit. It is the one place this project depends on a vendor internal.

#define GUI_BORDER_PX (3)  // the red border's thickness, per the spec
#define GUI_MARGIN_PX (24)  // gap from the screen edge
#define GUI_DEFAULT_W (480)
#define GUI_DEFAULT_H (360)
#define GUI_MAX_QUADS (16)  // 1 backdrop + 1 card + 4 border, with headroom
#define GUI_VERTS_PER_QUAD (6)

// The shutter animation, in three phases:
//   FROZEN  the captured frame is held still behind a 3px red border
//   TOSS    it is thrown away -- shrinking and swinging along an elliptic arc
//           to the bottom-right corner, fading out, with the LIVE feed already
//           running behind it
//   LIVE    only the feed
// The toss exists to make the handoff legible: the user sees the exact frame
// that was analyzed leave the screen, rather than the image just blinking back
// to live.
#define GUI_TOSS_ARC_X (1.9f)  // how far right the card flies, in NDC
#define GUI_TOSS_ARC_Y (2.1f)  // how far down it finally drops
#define GUI_TOSS_LIFT (0.22f)  // the little hop at the start of the throw
#define GUI_TOSS_SPIN (0.55f)  // final rotation, radians
#define GUI_TOSS_MIN_SCALE (0.12f)

typedef enum {
  GUI_PHASE_LIVE,
  GUI_PHASE_FROZEN,
  GUI_PHASE_TOSS,
} GuiPhase;

typedef struct {
  GuiPhase phase;
  f32 t;  // 0..1 progress, only meaningful during GUI_PHASE_TOSS
} GuiScene;

// @class Gui
// Function | Purpose
// --- | ---
// Gui__setup(g, w, h) | Create the GPU resources for a w*h feed
// Gui__shutdown(g) | Release GPU resources
// Gui__uploadLive(g, rgba) | Push a new RGBA frame into the live texture
// Gui__uploadFrozen(g, rgba) | Push the captured frame into the frozen texture
// Gui__draw(g, scene) | Draw one frame of the preview
// Gui__place(title) | Position the window bottom-right and keep it on top
// Gui__yuyvToRgba(yuyv, rgba, w, h) | Convert a captured frame for display

typedef struct {
  f32 x, y, u, v;
} GuiVert;

// where the tossed card sits this frame, in normalized device coords
typedef struct {
  f32 cx, cy;  // centre
  f32 scale;  // 1 = fills the window
  f32 rot;  // radians, applied in aspect-corrected space
  f32 alpha;
} GuiCard;

typedef struct {
  sg_image tex_live;  // the running feed
  sg_image tex_frozen;  // the frame the agent was given
  sg_sampler smp;
  sg_buffer vbuf;
  sg_shader shd;
  sg_pipeline pip;
  u32 w, h;  // feed dimensions
  bool ready;
} Gui;

// uniform block, mirrored by the `tint` and `flags` uniforms in the shader
typedef struct {
  f32 tint[4];  // rgba multiplier / solid colour
  f32 flags[4];  // x: 1 = sample the texture, 0 = use tint as a solid colour
} GuiUniforms;

// Convert a captured YUYV422 frame to RGBA8 for display.
// BT.601 limited-range in, full-range sRGB out -- the same expansion the JPEG
// encoder applies, so what the user sees matches what the agent is sent.
void Gui__yuyvToRgba(const u8* yuyv, u8* rgba, u32 w, u32 h) {
  for (u32 y = 0; y < h && y < MAX_FRAME_H; y++) {
    const u8* src = yuyv + (u64)y * (u64)w * 2ULL;
    u8* dst = rgba + (u64)y * (u64)w * 4ULL;
    // two pixels share one chroma pair, so step the row two pixels at a time
    for (u32 x = 0; x + 1 < w; x += 2) {
      f32 y0 = ((f32)src[x * 2] - 16.0f) * (255.0f / 219.0f);
      f32 y1 = ((f32)src[x * 2 + 2] - 16.0f) * (255.0f / 219.0f);
      f32 cb = ((f32)src[x * 2 + 1] - 128.0f) * (255.0f / 224.0f);
      f32 cr = ((f32)src[x * 2 + 3] - 128.0f) * (255.0f / 224.0f);

      for (u32 k = 0; k < 2; k++) {
        f32 yy = 0 == k ? y0 : y1;
        f32 r = yy + 1.402f * cr;
        f32 g = yy - 0.344136f * cb - 0.714136f * cr;
        f32 b = yy + 1.772f * cb;
        if (r < 0.0f) {
          r = 0.0f;
        }
        if (r > 255.0f) {
          r = 255.0f;
        }
        if (g < 0.0f) {
          g = 0.0f;
        }
        if (g > 255.0f) {
          g = 255.0f;
        }
        if (b < 0.0f) {
          b = 0.0f;
        }
        if (b > 255.0f) {
          b = 255.0f;
        }
        u8* p = dst + (u64)(x + k) * 4ULL;
        p[0] = (u8)r;
        p[1] = (u8)g;
        p[2] = (u8)b;
        p[3] = 255;
      }
    }
    // an odd final column has no chroma partner; replicate the previous pixel
    if (0 != (w & 1u) && w >= 2) {
      u8* p = dst + (u64)(w - 1) * 4ULL;
      const u8* q = dst + (u64)(w - 2) * 4ULL;
      p[0] = q[0];
      p[1] = q[1];
      p[2] = q[2];
      p[3] = 255;
    }
  }
}

// Create the GPU resources for a w*h feed
bool Gui__setup(Gui* g, u32 w, u32 h) {
  memset(g, 0, sizeof(*g));
  g->w = w;
  g->h = h;

  // two textures, because the toss draws the frozen frame OVER the live one
  g->tex_live = sg_make_image(&(sg_image_desc){
      .width = (int)w,
      .height = (int)h,
      .pixel_format = SG_PIXELFORMAT_RGBA8,
      .usage = SG_USAGE_STREAM,
      .label = "camera-live",
  });
  g->tex_frozen = sg_make_image(&(sg_image_desc){
      .width = (int)w,
      .height = (int)h,
      .pixel_format = SG_PIXELFORMAT_RGBA8,
      .usage = SG_USAGE_STREAM,
      .label = "camera-frozen",
  });
  g->smp = sg_make_sampler(&(sg_sampler_desc){
      .min_filter = SG_FILTER_LINEAR,
      .mag_filter = SG_FILTER_LINEAR,
      .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
      .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
      .label = "camera-sampler",
  });
  g->vbuf = sg_make_buffer(&(sg_buffer_desc){
      .size = sizeof(GuiVert) * GUI_MAX_QUADS * GUI_VERTS_PER_QUAD,
      .usage = SG_USAGE_STREAM,
      .label = "camera-quads",
  });

  // GLSL for sokol's GLCORE backend (GL 4.3 on Linux). Uniforms are plain
  // globals because sokol's GL path resolves them by name, one at a time.
  g->shd = sg_make_shader(&(sg_shader_desc){
      .vs.source = "#version 430\n"
                   "layout(location=0) in vec2 pos;\n"
                   "layout(location=1) in vec2 uv0;\n"
                   "out vec2 uv;\n"
                   "void main() {\n"
                   "  gl_Position = vec4(pos, 0.0, 1.0);\n"
                   "  uv = uv0;\n"
                   "}\n",
      .fs.source = "#version 430\n"
                   "uniform vec4 tint;\n"
                   "uniform vec4 flags;\n"
                   "uniform sampler2D tex;\n"
                   "in vec2 uv;\n"
                   "out vec4 frag_color;\n"
                   "void main() {\n"
                   "  vec4 c = texture(tex, uv);\n"
                   // flags.x selects textured vs solid; tint multiplies either way, so
                   // tint.a drives the fade as the card is thrown away
                   "  frag_color = mix(tint, c * tint, flags.x);\n"
                   "}\n",
      .fs.uniform_blocks[0] =
          {
              .size = sizeof(GuiUniforms),
              .layout = SG_UNIFORMLAYOUT_NATIVE,
              .uniforms =
                  {
                      [0] = {.name = "tint", .type = SG_UNIFORMTYPE_FLOAT4},
                      [1] = {.name = "flags", .type = SG_UNIFORMTYPE_FLOAT4},
                  },
          },
      .fs.images[0] =
          {.used = true, .image_type = SG_IMAGETYPE_2D, .sample_type = SG_IMAGESAMPLETYPE_FLOAT},
      .fs.samplers[0] = {.used = true, .sampler_type = SG_SAMPLERTYPE_FILTERING},
      .fs.image_sampler_pairs[0] =
          {.used = true, .image_slot = 0, .sampler_slot = 0, .glsl_name = "tex"},
      .label = "camera-shader",
  });

  g->pip = sg_make_pipeline(&(sg_pipeline_desc){
      .shader = g->shd,
      .layout =
          {
              // attribute names are omitted; layout(location=) in the shader
              // already matches these slots
              .attrs =
                  {
                      [0] = {.format = SG_VERTEXFORMAT_FLOAT2},
                      [1] = {.format = SG_VERTEXFORMAT_FLOAT2},
                  },
          },
      .primitive_type = SG_PRIMITIVETYPE_TRIANGLES,
      // the thrown card fades out over the live feed, so it must blend
      .colors[0].blend =
          {
              .enabled = true,
              .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
              .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
              .src_factor_alpha = SG_BLENDFACTOR_ONE,
              .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
          },
      .label = "camera-pipeline",
  });

  g->ready = sg_query_image_state(g->tex_live) == SG_RESOURCESTATE_VALID &&
             sg_query_image_state(g->tex_frozen) == SG_RESOURCESTATE_VALID &&
             sg_query_pipeline_state(g->pip) == SG_RESOURCESTATE_VALID;
  if (!g->ready) {
    LOG_ERRORF("GPU resources failed to validate; the preview cannot render");
  }
  return g->ready;
}

// Release GPU resources
void Gui__shutdown(Gui* g) {
  if (!g->ready) {
    return;
  }
  sg_destroy_pipeline(g->pip);
  sg_destroy_shader(g->shd);
  sg_destroy_buffer(g->vbuf);
  sg_destroy_sampler(g->smp);
  sg_destroy_image(g->tex_frozen);
  sg_destroy_image(g->tex_live);
  g->ready = false;
}

// push RGBA pixels into one of the two textures
static void _Gui__upload(Gui* g, sg_image tex, const u8* rgba) {
  if (!g->ready) {
    return;
  }
  sg_update_image(
      tex,
      &(sg_image_data){
          .subimage[0][0] = {.ptr = rgba, .size = (size_t)g->w * (size_t)g->h * 4u},
      });
}

// Push a new RGBA frame into the live texture
void Gui__uploadLive(Gui* g, const u8* rgba) {
  _Gui__upload(g, g->tex_live, rgba);
}

// Push the captured frame into the frozen texture
void Gui__uploadFrozen(Gui* g, const u8* rgba) {
  _Gui__upload(g, g->tex_frozen, rgba);
}

// append one quad from four explicit corners (so it may be rotated), in the
// order bottom-left, bottom-right, top-right, top-left
static u32 _Gui__quad4(
    GuiVert* out, u32 at, const f32* xs, const f32* ys, f32 u0, f32 v0, f32 u1, f32 v1) {
  if (at + GUI_VERTS_PER_QUAD > GUI_MAX_QUADS * GUI_VERTS_PER_QUAD) {
    return at;
  }
  // uvs follow the same corner order; v is flipped because RGBA rows are
  // stored top-down while NDC y points up
  out[at + 0] = (GuiVert){xs[0], ys[0], u0, v0};
  out[at + 1] = (GuiVert){xs[1], ys[1], u1, v0};
  out[at + 2] = (GuiVert){xs[2], ys[2], u1, v1};
  out[at + 3] = (GuiVert){xs[0], ys[0], u0, v0};
  out[at + 4] = (GuiVert){xs[2], ys[2], u1, v1};
  out[at + 5] = (GuiVert){xs[3], ys[3], u0, v1};
  return at + GUI_VERTS_PER_QUAD;
}

// map a point in card-local space ([-1,1] square) to NDC under the card's
// scale, rotation and translation. Rotation happens in aspect-corrected space
// so a square card stays square in a non-square window.
static void _Gui__xform(const GuiCard* c, f32 aspect, f32 lx, f32 ly, f32* ox, f32* oy) {
  f32 sx = lx * c->scale * aspect;
  f32 sy = ly * c->scale;
  f32 cs = cosf(c->rot), sn = sinf(c->rot);
  f32 rx = sx * cs - sy * sn;
  f32 ry = sx * sn + sy * cs;
  *ox = rx / aspect + c->cx;
  *oy = ry + c->cy;
}

// emit a card: its image quad, then the four border quads around it.
// Returns the vertex count; *out_image_verts receives the image's share.
static u32 _Gui__card(
    GuiVert* verts, u32 at, const GuiCard* c, f32 aspect, f32 bx, f32 by, u32* out_image_verts) {
  f32 xs[4], ys[4];
  // image quad: the whole local square
  static const f32 LX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
  static const f32 LY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
  for (u32 i = 0; i < 4; i++) {
    _Gui__xform(c, aspect, LX[i], LY[i], &xs[i], &ys[i]);
  }
  u32 start = at;
  at = _Gui__quad4(verts, at, xs, ys, 0.0f, 1.0f, 1.0f, 0.0f);
  *out_image_verts = at - start;

  // four border strips, specified in the SAME local space so they rotate and
  // shrink with the card
  const f32 EDGES[4][4] = {
      {-1.0f, 1.0f - by, 1.0f, 1.0f},  // top:    x0, y0, x1, y1
      {-1.0f, -1.0f, 1.0f, -1.0f + by},  // bottom
      {-1.0f, -1.0f, -1.0f + bx, 1.0f},  // left
      {1.0f - bx, -1.0f, 1.0f, 1.0f},  // right
  };
  for (u32 e = 0; e < 4; e++) {
    f32 x0 = EDGES[e][0], y0 = EDGES[e][1], x1 = EDGES[e][2], y1 = EDGES[e][3];
    const f32 ex[4] = {x0, x1, x1, x0};
    const f32 ey[4] = {y0, y0, y1, y1};
    for (u32 i = 0; i < 4; i++) {
      _Gui__xform(c, aspect, ex[i], ey[i], &xs[i], &ys[i]);
    }
    at = _Gui__quad4(verts, at, xs, ys, 0.0f, 0.0f, 0.0f, 0.0f);
  }
  return at;
}

// ease-in cubic: the throw starts slow, as if the card is being released
static f32 _Gui__easeIn(f32 t) {
  return t * t * t;
}

// ease-out quadratic
static f32 _Gui__easeOut(f32 t) {
  f32 u = 1.0f - t;
  return 1.0f - u * u;
}

// where the card is at progress t of the toss
static GuiCard _Gui__tossCard(f32 t) {
  if (t < 0.0f) {
    t = 0.0f;
  }
  if (t > 1.0f) {
    t = 1.0f;
  }
  GuiCard c;
  // A quarter-ellipse: it leaves to the right first (horizontal tangent at
  // t=0) and drops at the end (vertical tangent at t=1), which reads as a
  // throw rather than a slide. The small sine lift is the hop off the hand.
  f32 a = t * 1.5707963f;  // pi/2
  c.cx = GUI_TOSS_ARC_X * sinf(a);
  c.cy = GUI_TOSS_ARC_Y * (cosf(a) - 1.0f) + GUI_TOSS_LIFT * sinf(t * 3.14159265f);
  // shrink with an ease-in so it hangs around a beat before flying off
  c.scale = 1.0f - (1.0f - GUI_TOSS_MIN_SCALE) * _Gui__easeIn(t);
  c.rot = -GUI_TOSS_SPIN * _Gui__easeOut(t);  // tips clockwise as it falls
  // hold opacity through most of the arc, then fade over the last third
  c.alpha = t < 0.65f ? 1.0f : 1.0f - (t - 0.65f) / 0.35f;
  if (c.alpha < 0.0f) {
    c.alpha = 0.0f;
  }
  return c;
}

// Draw one frame of the preview
void Gui__draw(Gui* g, const GuiScene* scene) {
  if (!g->ready) {
    return;
  }

  GuiVert verts[GUI_MAX_QUADS * GUI_VERTS_PER_QUAD];
  memset(verts, 0, sizeof(verts));

  f32 fw = sapp_widthf() > 0.0f ? sapp_widthf() : (f32)GUI_DEFAULT_W;
  f32 fh = sapp_heightf() > 0.0f ? sapp_heightf() : (f32)GUI_DEFAULT_H;
  f32 aspect = fw / fh;
  // border thickness in local units: 3 device pixels across the full window
  f32 bx = 2.0f * (f32)GUI_BORDER_PX / fw;
  f32 by = 2.0f * (f32)GUI_BORDER_PX / fh;

  // --- quad 0: the full-window backdrop, used by every phase ---
  GuiCard full = {.cx = 0.0f, .cy = 0.0f, .scale = 1.0f, .rot = 0.0f, .alpha = 1.0f};
  f32 xs[4], ys[4];
  static const f32 LX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
  static const f32 LY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
  for (u32 i = 0; i < 4; i++) {
    _Gui__xform(&full, aspect, LX[i], LY[i], &xs[i], &ys[i]);
  }
  u32 n = _Gui__quad4(verts, 0, xs, ys, 0.0f, 1.0f, 1.0f, 0.0f);
  u32 backdrop_verts = n;

  // --- the card: either held still, or mid-throw ---
  u32 card_at = n, card_image_verts = 0, card_end = n;
  GuiCard card = full;
  bool has_card = GUI_PHASE_FROZEN == scene->phase || GUI_PHASE_TOSS == scene->phase;
  if (GUI_PHASE_TOSS == scene->phase) {
    card = _Gui__tossCard(scene->t);
  }
  if (has_card) {
    card_end = _Gui__card(verts, card_at, &card, aspect, bx, by, &card_image_verts);
    n = card_end;
  }

  sg_update_buffer(g->vbuf, &(sg_range){.ptr = verts, .size = sizeof(GuiVert) * n});

  sg_begin_pass(&(sg_pass){
      .action =
          {.colors[0] =
               {.load_action = SG_LOADACTION_CLEAR, .clear_value = {0.05f, 0.05f, 0.06f, 1.0f}}},
      .swapchain = sglue_swapchain(),
  });
  sg_apply_pipeline(g->pip);

  GuiUniforms solid_red = {.tint = {0.93f, 0.11f, 0.14f, 1.0f}, .flags = {0.0f, 0.0f, 0.0f, 0.0f}};
  GuiUniforms plain = {.tint = {1.0f, 1.0f, 1.0f, 1.0f}, .flags = {1.0f, 0.0f, 0.0f, 0.0f}};

  // --- backdrop ---
  // During FROZEN the card covers the window completely, so drawing the live
  // feed underneath would be wasted; the frozen texture is used instead. In
  // TOSS the live feed must already be running behind the departing card.
  bool backdrop_is_live = GUI_PHASE_FROZEN != scene->phase;
  sg_apply_bindings(&(sg_bindings){
      .vertex_buffers[0] = g->vbuf,
      .fs.images[0] = backdrop_is_live ? g->tex_live : g->tex_frozen,
      .fs.samplers[0] = g->smp,
  });
  sg_apply_uniforms(SG_SHADERSTAGE_FS, 0, &(sg_range){.ptr = &plain, .size = sizeof(plain)});
  sg_draw(0, (int)backdrop_verts, 1);

  if (has_card) {
    if (GUI_PHASE_TOSS == scene->phase) {
      // the departing card is its own textured quad over the live backdrop
      GuiUniforms fade = {
          .tint = {1.0f, 1.0f, 1.0f, card.alpha},
          .flags = {1.0f, 0.0f, 0.0f, 0.0f}};
      sg_apply_bindings(&(sg_bindings){
          .vertex_buffers[0] = g->vbuf,
          .fs.images[0] = g->tex_frozen,
          .fs.samplers[0] = g->smp,
      });
      sg_apply_uniforms(SG_SHADERSTAGE_FS, 0, &(sg_range){.ptr = &fade, .size = sizeof(fade)});
      sg_draw((int)card_at, (int)card_image_verts, 1);
      solid_red.tint[3] = card.alpha;  // the border fades with its card
    }
    // the red border, last, so it sits on top of the image
    u32 border_at = card_at + card_image_verts;
    if (card_end > border_at) {
      sg_apply_uniforms(
          SG_SHADERSTAGE_FS,
          0,
          &(sg_range){.ptr = &solid_red, .size = sizeof(solid_red)});
      sg_draw((int)border_at, (int)(card_end - border_at), 1);
    }
  }

  sg_end_pass();
  sg_commit();
}

// ---
// X11 placement
//
// sokol_app has no cross-platform "put this window here and keep it on top", so
// the EWMH calls are made directly. Everything here is best-effort: a window
// manager is free to ignore any of it, and the preview still works if it does.

// find the primary monitor's geometry, falling back to the whole X screen
static void _Gui__primaryRect(Display* dpy, int screen, int* x, int* y, int* w, int* h) {
  *x = 0;
  *y = 0;
  *w = DisplayWidth(dpy, screen);
  *h = DisplayHeight(dpy, screen);

  Window root = RootWindow(dpy, screen);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  if (NULL == res) {
    return;
  }
  RROutput primary = XRRGetOutputPrimary(dpy, root);
  // prefer the primary output; otherwise take the first connected one
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < res->noutput; i++) {
      if (0 == pass && res->outputs[i] != primary) {
        continue;
      }
      XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
      if (NULL == oi) {
        continue;
      }
      if (RR_Connected == oi->connection && 0 != oi->crtc) {
        XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
        if (NULL != ci && ci->width > 0 && ci->height > 0) {
          *x = ci->x;
          *y = ci->y;
          *w = (int)ci->width;
          *h = (int)ci->height;
          XRRFreeCrtcInfo(ci);
          XRRFreeOutputInfo(oi);
          XRRFreeScreenResources(res);
          return;
        }
        if (NULL != ci) {
          XRRFreeCrtcInfo(ci);
        }
      }
      XRRFreeOutputInfo(oi);
    }
  }
  XRRFreeScreenResources(res);
}

// ask the window manager to add a _NET_WM_STATE property
static void _Gui__wmState(Display* dpy, Window win, const char* name) {
  Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
  Atom which = XInternAtom(dpy, name, False);
  if (None == state || None == which) {
    return;
  }
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = ClientMessage;
  ev.xclient.window = win;
  ev.xclient.message_type = state;
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = 1;  // _NET_WM_STATE_ADD
  ev.xclient.data.l[1] = (long)which;
  ev.xclient.data.l[2] = 0;
  ev.xclient.data.l[3] = 1;  // source: normal application
  (void)XSendEvent(
      dpy,
      DefaultRootWindow(dpy),
      False,
      SubstructureNotifyMask | SubstructureRedirectMask,
      &ev);
}

// read an integer from the environment, or return the fallback
static int _Gui__envInt(const char* name, int fallback) {
  const char* v = getenv(name);
  u32 out = 0;
  if (NULL != v && 0 != v[0] && cstr__toU32(v, &out)) {
    return (int)out;
  }
  return fallback;
}

// Position the window bottom-right of the primary monitor and keep it on top.
// $CAMERA_PREVIEW_X / _Y override the position; $CAMERA_PREVIEW_ONTOP=0 opts out.
void Gui__place(const char* title) {
  Display* dpy = _sapp.x11.display;
  Window win = _sapp.x11.window;
  if (NULL == dpy || None == win) {
    LOG_ERRORF("no X11 window handle; skipping placement");
    return;
  }
  int screen = DefaultScreen(dpy);

  int mx = 0, my = 0, mw = 0, mh = 0;
  _Gui__primaryRect(dpy, screen, &mx, &my, &mw, &mh);

  int ww = sapp_width() > 0 ? sapp_width() : GUI_DEFAULT_W;
  int wh = sapp_height() > 0 ? sapp_height() : GUI_DEFAULT_H;
  int x = mx + mw - ww - GUI_MARGIN_PX;
  int y = my + mh - wh - GUI_MARGIN_PX;
  x = _Gui__envInt("CAMERA_PREVIEW_X", x);
  y = _Gui__envInt("CAMERA_PREVIEW_Y", y);

  // a utility window is the closest EWMH type to "a small tool panel"; it also
  // keeps most window managers from giving it a tiling slot
  Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
  Atom utility = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
  if (None != type && None != utility) {
    (void)XChangeProperty(
        dpy,
        win,
        type,
        XA_ATOM,
        32,
        PropModeReplace,
        (const unsigned char*)&utility,
        1);
  }

  const char* ontop = getenv("CAMERA_PREVIEW_ONTOP");
  if (NULL == ontop || !cstr__eq(ontop, "0")) {
    _Gui__wmState(dpy, win, "_NET_WM_STATE_ABOVE");
  }
  // The window stays in the taskbar by default: a camera icon sitting in the
  // task list is useful standing feedback that an agent currently holds the
  // camera. $CAMERA_PREVIEW_SKIP_TASKBAR=1 hides it for anyone who disagrees.
  const char* skip = getenv("CAMERA_PREVIEW_SKIP_TASKBAR");
  if (NULL != skip && cstr__eq(skip, "1")) {
    _Gui__wmState(dpy, win, "_NET_WM_STATE_SKIP_TASKBAR");
  }

  (void)XMoveResizeWindow(dpy, win, x, y, (unsigned)ww, (unsigned)wh);
  (void)XFlush(dpy);
  sapp_set_window_title(title);
  LOG_INFOF("preview placed at %d,%d (%dx%d) on monitor %dx%d+%d+%d", x, y, ww, wh, mw, mh, mx, my);
}
