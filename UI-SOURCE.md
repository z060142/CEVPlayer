# Showing CEVPlayer video in a UI system

This document is for authors of UI plug-ins (or any other renderer-side code) that want to show a CEVPlayer video as
part of their own layout. It specifies the **texture source contract** `CryTexSource`: a small C interface that
CEVPlayer exports and a UI plug-in consumes. The two plug-ins share no headers and do not link against each other.

Terms: the **source** is the plug-in that owns the video and its texture (CEVPlayer). The **consumer** is the plug-in
that draws it (a UI system). A **surface** is a video output with a name, the **surface id**.

If you only need the texture and no negotiation, a surface is also reachable as the engine texture `$cev:<id>` (RGBA8,
premultiplied). The contract adds what a UI needs on top of that: visibility throttling, size-dependent quality,
GPU-side colour conversion, playback control and change notification.

## 1. Surface ids

A surface id is chosen by the author of the level or UI: 1 to 63 characters of `A-Z a-z 0-9 _ . : -`, compared
case-insensitively, unique among live surfaces. The id stays the same when the file, audio track, variant or picture
size of the player changes.

A surface comes into existence in one of two ways:

* game logic creates it (FlowGraph node `CEVPlayer:UI:PlayToSurface`, the `Video Player` component with output
  *Surface*, CryVS, or `IVideoPlugin::CreatePlayerForSurface` in C++), and the UI refers to it by id;
* the consumer creates it itself through `open` (section 4), for example for a `<video src="...">` element of its own.

## 2. Discovery

CEVPlayer exports one C function from `CEVPlayer.dll`:

```c
__declspec(dllexport) const CryTexSourceApi* CryTexSource_v1(void);
```

A consumer enumerates the modules loaded in the process and calls `GetProcAddress(module, "CryTexSource_v1")` on each.
Every module that has the export is a source. `CryTexSourceApi::version` is the highest contract version the source
supports (CEVPlayer: 2). Structures only ever grow at the end.

Module lifetime:

* The consumer holds one module reference for every module whose API pointer it keeps.
* When a name cannot be resolved, or no source takes an `open`, the consumer scans again, at most once per second.
* After the source plug-in has shut down, its exported functions stay callable until the process ends: `query`, `open`
  and `control` return 0, `hint` and `close` do nothing. There is no shutdown ordering between the plug-ins; this rule
  replaces it.
* When two sources answer `query` for the same name, the one that comes first in the module scan wins.

## 3. Declarations

Copy this block into the consumer. Do not change the field order.

```c
/* Threading: all calls come from one thread at a time - the consumer's UI update thread. That is
   the main thread, except during level loading (section 8). The source must not assume the engine
   main thread.
   Strings: UTF-8, valid only for the duration of the call. Names are surface ids, compared
   case-insensitively.
   Sizes: every struct starts with `size` (set by whoever allocates it). Neither side reads or writes
   a field that lies beyond the other side's `size`; fields that were not written keep the caller's
   zero-initialised value. */

enum { CTS_LAYOUT_RGBA8 = 0,          /* one RGBA8 texture, premultiplied per flags bit0 */
       CTS_LAYOUT_PACKED_YUV420 = 1,  /* one R8 texture: Y on top, U and V side by side below */
       CTS_LAYOUT_PACKED_YUVA420 = 2, /* same, plus an alpha plane below */
       CTS_LAYOUT_NV12 = 3 };         /* reserved: two textures (R8 luma, R8G8 chroma) */

enum { CTS_SITING_LEFT = 0,           /* chroma co-sited with the left luma column, centred vertically (MPEG-2 style) */
       CTS_SITING_CENTER = 1,         /* centred both ways (JPEG / MPEG-1 style) */
       CTS_SITING_TOPLEFT = 2 };      /* co-sited with the top-left luma sample */

typedef struct CryTexSourceRect { int32_t x, y, w, h; } CryTexSourceRect;   /* texels */

typedef struct CryTexSourceInfo {       /* out */
    /* v1 */
    uint32_t size;
    int32_t  textureId;       /* engine texture id, 0 when there is none right now */
    int32_t  width, height;   /* picture size in pixels (not the packed texture's size) */
    uint32_t flags;           /* bit0 premultiplied (RGBA8), bit1 has alpha, bit2 playing,
                                 bit3 (v2) planar colour planes are already multiplied by alpha */
    uint64_t contentSerial;   /* increases whenever the pixels were replaced AND whenever
                                 `generation` changes; never decreases */
    /* v2 */
    uint32_t generation;      /* +1 whenever textureId, the texture size or layout changed */
    uint32_t layout;          /* CTS_LAYOUT_* */
    uint32_t colorMatrix;     /* 0 BT.601, 1 BT.709 (planar layouts) */
    uint32_t colorRange;      /* 0 limited (16-235), 1 full (planar layouts) */
    uint32_t chromaSiting;    /* CTS_SITING_* (planar layouts) */
    int32_t  textureId2;      /* second texture of a two-texture layout, else 0 */
    CryTexSourceRect planeY, planeU, planeV, planeA;   /* rects inside textureId (packed layouts) */
    uint32_t state;           /* 0 none, 1 loading, 2 ready, 3 playing, 4 paused, 5 ended, 6 error */
    int32_t  errorCode;       /* source-defined, 0 = none */
    double   timeSeconds, durationSeconds;   /* the source's clock (audio master) */
    uint32_t loopCount;       /* +1 on every wrap of a looping video */
} CryTexSourceInfo;

typedef struct CryTexSourceHint {       /* in */
    /* v1 */
    uint32_t size;            /* a v2 consumer (consumer != 0) that sends flags = bit1 only and
                                 pixelWidth = pixelHeight = 0 means "I stopped using the name" */
    uint32_t flags;           /* bit0 visible on screen this frame, bit1 (v2) release */
    int32_t  pixelWidth, pixelHeight;   /* largest on-screen size it is sampled at, 0 = unknown */
    /* v2 */
    uint64_t consumer;        /* unique per consumer (e.g. an address); 0 = anonymous v1 consumer */
    uint32_t acceptLayouts;   /* bit n = can sample CTS_LAYOUT_n; 0 means RGBA8 only */
} CryTexSourceHint;

typedef struct CryTexSourceOpen {       /* in, v2 */
    uint32_t size;
    uint32_t flags;           /* bit0 autoplay, bit1 loop, bit2 muted */
    float    volume;          /* 0..1 */
} CryTexSourceOpen;

enum { CTS_VERB_PLAY = 1, CTS_VERB_PAUSE, CTS_VERB_SEEK /* seconds */, CTS_VERB_RATE,
       CTS_VERB_LOOP /* 0/1 */, CTS_VERB_VOLUME /* 0..1 */, CTS_VERB_MUTE /* 0/1 */ };

typedef struct CryTexSourceApi {
    uint32_t version;         /* highest supported: 1 or 2 */
    /* v1 */
    int  (*query)(const char* name, CryTexSourceInfo* out);            /* 1 = name is ours, out filled */
    void (*hint)(const char* name, const CryTexSourceHint* hint);      /* advisory */
    /* v2 */
    /* 0 = not taken; 1 = opened, owned by this name's opener (who must `close`);
       2 = the name already exists and is not owned by this open - attach by `query`, do not `close`. */
    int  (*open)(const char* name, const char* src, const CryTexSourceOpen* options);
    void (*close)(const char* name);
    int  (*control)(const char* name, uint32_t verb, double value);    /* intent; 0 = refused */
} CryTexSourceApi;

__declspec(dllexport) const CryTexSourceApi* CryTexSource_v1(void);
```

The source side keeps the same block in `Code/CryPlugins/CEVPlayer/Module/Contract/TexSource.h`.

## 4. Rules

**Every frame: `query`.** The consumer calls `query(name, &info)` once per frame for every name it shows.

* A return of 0 means the name does not exist (or the source has shut down): draw transparent and drop the texture
  reference.
* When `generation` differs from the last frame, fetch the texture again from `textureId` and update the intrinsic size
  (`contentSerial` changes too).
* `contentSerial` tells whether the pixels changed; a consumer that caches a composited result can key on it.

**Every frame: `hint`.** A live consumer sends `hint` for every name it holds, every frame, also while the element is
not visible (`flags` bit0 = 0). The source aggregates hints per `(name, consumer)`:

* The surface counts as visible when any consumer reports it visible.
* The on-screen size is the largest reported one.
* A packed layout is used only when **every** live consumer accepts it.
* `consumer` 0 is an anonymous v1 consumer and counts as one consumer that accepts RGBA8 only.
* A consumer leaves by sending the release hint, or by sending no hint for 120 consecutive updates of the source (the
  count is in source updates, not wall-clock time, so a stalled main thread does not expire anybody).
* A name nobody ever sent a hint for is RGBA8 and always counts as visible.

What CEVPlayer does with the hints: while a surface is hidden it only decodes (no upload, no colour conversion) and
stops decoding after `cev_hiddenStopSeconds`; when it becomes visible again it catches up from the previous key frame.
The on-screen size picks a lower-resolution variant of the video when the descriptor has one; the choice is made when
playback starts.

**`open` and `close`.** `open(name, src, options)` asks the source to create a surface.

* `name` is given by the consumer: an id chosen by the UI author, or one the consumer generates.
* `src` is a path CryPak can open: relative to the game folder, or absolute. CEVPlayer takes `.webm` and `.cevvideo`.
* Return 1: the player belongs to this `open`; whoever opened it must `close` it. Opening the same name with the same
  `src` again does nothing.
* Return 2: game logic already created a surface with this id. Attach with `query`; do not `close`.
* Return 0: this source does not take the file. Only then ask the next source; later errors are not handed over.
* `open` does not block. It decides by file extension and the existing registry only. Opening the file, recognising the
  container and creating the decoder happen asynchronously: `state` is 1 (loading) meanwhile, and a failure shows as
  `state` 6 (error) with `errorCode`.

**`control`.** `control(name, verb, value)` expresses an intent; the source may refuse (return 0).

| Verb | `value` |
|---|---|
| `CTS_VERB_PLAY`, `CTS_VERB_PAUSE` | not read |
| `CTS_VERB_SEEK` | seconds |
| `CTS_VERB_RATE` | factor; 0 is returned when it is outside the supported range |
| `CTS_VERB_LOOP`, `CTS_VERB_MUTE` | non-zero = on |
| `CTS_VERB_VOLUME` | 0..1 |

* The clock always belongs to the source (audio is the master). `timeSeconds` reports the real position.
* A seek is frame accurate on a best-effort basis. A seek that arrives while `state` is loading is remembered; other
  verbs are applied in the order they were made.
* A looping video never reports ended; each wrap adds 1 to `loopCount`.

**Threading.** CEVPlayer's contract functions make no engine call. `query` reads a snapshot the player wrote at the end
of its own update; `hint` only records; `open`, `close` and `control` are queued and applied at the player's next
update. They may therefore be called from the consumer's update thread, whichever thread that is, as long as calls do
not overlap.

## 5. Layouts and colour

**RGBA8 (layout 0) is the layout every consumer must be able to sample.** `acceptLayouts` only says which layouts a
consumer can take in addition. The source may go back to RGBA8 at any time: the video cannot be packed, `cev_allowPlanar`
is 0, another consumer accepts RGBA8 only, or the source needs RGBA8 itself (a surface shown in a Flash movie clip). A
consumer must not assume that a negotiated packed layout stays.

A layout change does not reload the video and does not interrupt the sound: only the texture is recreated and
`generation` goes up by one.

* RGBA8 is premultiplied (`flags` bit0 = 1).
* Packed layouts are 8 bit, SDR, sRGB transfer; `colorMatrix`, `colorRange` and `chromaSiting` describe them.
* `flags` bit3 says whether the colour planes of a packed layout are already multiplied by alpha. With straight colour
  planes, bilinear sampling can show fringes where alpha is 0; choose the compositing accordingly. The consumer's
  output is premultiplied.
* Packed layouts have no mip chain. Quality at small sizes comes from the resolution variants.
* CEVPlayer packs only 4:2:0 videos with an even width.

## 6. Sampling a packed layout

The source uploads one `R8` texture of `w x (h + ch [+ h])` texels with tightly packed rows, where `ch = ceil(h / 2)`
and the chroma width is `cw = w / 2`. `planeY`, `planeU`, `planeV` and `planeA` are the rectangles of the planes inside
it. The output is `width x height` premultiplied RGBA8.

**Positions** (texel centres at integer coordinates; output pixel `(x, y)`):

* Y and A: texel `(x, y)` inside their rectangle (1:1).
* Chroma: continuous coordinate `cx = (x + ox) / 2`, `cy = (y + oy) / 2` inside the U / V rectangle, with
  `ox = -0.5` (CENTER) or `0` (LEFT, TOPLEFT) and `oy = 0` (TOPLEFT) or `-0.5` (LEFT, CENTER). Interpolate bilinearly.
* Clamp the chroma coordinate to `[0, cw-1] x [0, ch-1]` first. In texture UV terms that is the rectangle shrunk by
  half a texel: `u` in `[(rx + 0.5)/W, (rx + rw - 0.5)/W]`, `v` in `[(ry + 0.5)/H, (ry + rh - 0.5)/H]`, where `W` and
  `H` are the size of the whole texture. This guarantees that a neighbouring plane is never sampled.

**Colour** (floating point; BT.601 `Kr = 0.299, Kb = 0.114`; BT.709 `Kr = 0.2126, Kb = 0.0722`; `Kg = 1 - Kr - Kb`):

* Limited range: `Y' = (Y - 16) / 219`, `Cb = (U - 128) / 224`, `Cr = (V - 128) / 224`.
* Full range: `Y' = Y / 255`, `Cb = (U - 128) / 255`, `Cr = (V - 128) / 255`.
* `R = Y' + 2(1 - Kr) Cr`
* `G = Y' - (2 Kb (1 - Kb) / Kg) Cb - (2 Kr (1 - Kr) / Kg) Cr`
* `B = Y' + 2(1 - Kb) Cb`
* Clamp each component to [0, 1], **then** multiply by alpha (`a = A / 255`, or 1 without an alpha plane; skip the
  multiplication when `flags` bit3 says the planes are already multiplied), and round to 8 bit.

A reference implementation is in `Code/CryPlugins/CEVPlayer/Core/tests/ReferenceSampler.h` (used by tests only). A
consumer's shader should match its bilinear variant within one 8-bit step. The source's own RGBA8 conversion uses
nearest chroma, so it differs from the bilinear result at sharp colour edges; that is expected.

## 7. Minimal consumer

```c
/* once, and again (at most once per second) when a name cannot be resolved */
const CryTexSourceApi* api = NULL;
/* for each loaded module m: */
typedef const CryTexSourceApi* (*GetApiFn)(void);
GetApiFn fn = (GetApiFn)GetProcAddress(m, "CryTexSource_v1");
if (fn) api = fn();

/* every frame, for each video element */
CryTexSourceHint hint = { sizeof(hint) };
hint.flags = elementIsVisible ? 1u : 0u;
hint.pixelWidth = onScreenWidth;
hint.pixelHeight = onScreenHeight;
hint.consumer = (uint64_t)(uintptr_t)this_consumer;
hint.acceptLayouts = 1u << CTS_LAYOUT_RGBA8;       /* add the packed bits when the shader supports them */
api->hint(name, &hint);

CryTexSourceInfo info = { sizeof(info) };
if (api->query(name, &info) && info.textureId != 0)
{
    if (info.generation != lastGeneration) { /* re-fetch the engine texture by info.textureId */ }
    /* draw info.textureId according to info.layout */
}
else
{
    /* draw transparent, drop the texture reference */
}

/* when the element goes away */
hint.flags = 2u; hint.pixelWidth = hint.pixelHeight = 0;
api->hint(name, &hint);
```

## 8. Level loading screens

During a synchronous level load the main thread is blocked and the engine's loading loop runs on the render thread.
CEVPlayer updates its surface players inside that loop, so a video in a loading screen keeps playing with sound, when
the renderer gives it a way in: the XUI2DL patch of this repository (which calls CEVPlayer once per iteration), or a
renderer that offers `IRenderer::AddLoadtimeCallback(callback, order)` for several callbacks. `cev_info` reports
whether a loading-loop driver was found. Drawing the consumer's own UI inside the loop is the consumer's business;
this section only says what the source does there.

* On the `AddLoadtimeCallback` path CEVPlayer registers with order 100. A consumer that draws in the loop should
  register with a higher order, so that the texture is uploaded before it is sampled in the same iteration.
* The contract functions are then called from the render thread. That is covered by the threading rule of section 4;
  `open` and `control` are applied inside the loop as well, including opening a file.
* A level change does not change `generation` of a surface.
* The step of one loop iteration is capped at 0.25 s.
* The render frame id is not monotonic inside the loop. Do not key caches or "once per frame" logic on it.
* The loading screen does not end at `ESYSTEM_EVENT_LEVEL_LOAD_END`: the engine keeps drawing black for a while until
  the first scene frame. Do not assume the video stops at that event.
* Without a loading-loop driver, or with `cev_loadtime 0`, every player pauses during the load and resumes afterwards.
* Sandbox has no loading loop when it opens a level.

## 9. Checking an integration

* `cev_surfaces` lists every surface with its snapshot and the aggregated hints (consumers, visible, size, layouts).
* `cev_info` prints the contract version and the loading-loop state.
* `cev_contract_selftest` runs the source's own contract tests.
* `cev_allowPlanar 0` forces RGBA8 for every surface, which separates a sampling bug from a decoding bug.
