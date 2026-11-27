// Texture source contract (CryTexSource), source side. The C declarations below are copied verbatim from
// UI-SOURCE.md section 3; the consumer (another plug-in) keeps its own copy. Do not include any
// file of another plug-in here and do not change the field order: both copies must stay identical.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- begin: verbatim copy of UI-SOURCE.md section 3 ----
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
// ---- end: verbatim copy ----

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <vector>

// Source side bookkeeping of CEVPlayer (not part of the contract). Everything here is plain memory guarded by one
// lightweight lock and makes no engine call, so the contract functions may run on any thread and keep working
// after the plug-in shut down (static storage, closed flag). Names are surface ids, compared case-insensitively.
namespace CEVPlayer
{
	namespace Contract
	{
		//! Number of the source's own updates without a hint after which a consumer counts as gone (rule 5).
		constexpr uint32_t kHintExpiryUpdates = 120;

		//! Plug-in start / shutdown. After Close() query / open / control return 0 and hint / close do nothing.
		void Open();
		void Close();

		//! A surface player was created / destroyed. Register creates an empty snapshot (state 0, textureId 0).
		void RegisterSurface(const char* name);
		void UnregisterSurface(const char* name);
		//! End of the surface player's own tick: replaces the snapshot (info.size is ignored, the full struct is
		//! stored) and counts one update of the source for the hint expiry.
		//! `decode`: short text for cev_surfaces (backend in use), may be null.
		void PublishSnapshot(const char* name, const CryTexSourceInfo& info, const char* decode = nullptr);

		//! Hints of all live consumers of one name, aggregated per rule 5.
		struct SHintSummary
		{
			uint32_t consumers = 0;       // live (name, consumer) entries
			bool visible = false;         // any consumer visible
			int32_t pixelWidth = 0;       // largest
			int32_t pixelHeight = 0;
			uint32_t acceptLayouts = 1u;  // AND over consumers; consumer 0 / acceptLayouts 0 count as RGBA8 only
		};
		//! false when the name is not a registered surface.
		bool GetHintSummary(const char* name, SHintSummary& out);

		//! open / close / control of the contract (rule 10): queued by the contract functions, applied by the player
		//! manager at the start of its next tick (TakeCommands), in the order they were made.
		struct SCommand
		{
			enum class EType : uint8_t { Open, Close, Control };
			EType type = EType::Open;
			std::string name;
			std::string src;          // Open
			uint32_t openFlags = 0;   // Open: CryTexSourceOpen::flags (bit0 autoplay, bit1 loop, bit2 muted)
			float volume = 1.f;       // Open: CryTexSourceOpen::volume (1 when the caller's struct has no volume)
			uint32_t verb = 0;        // Control: CTS_VERB_*
			double value = 0.0;       // Control
		};
		//! Moves the queued commands into `out` (cleared first).
		void TakeCommands(std::vector<SCommand>& out);
		//! M10: true while commands are queued (read only: this table's own queue, no engine call).
		bool HasPendingCommands();
		//! The manager tells the table who owns a name: true = the player was made by `open` (its `close` destroys
		//! it, a repeated `open` with the same `src` is a no-op), false = level logic made it (`open` returns 2).
		void SetOwnedByOpen(const char* name, bool owned, const char* src);
		//! A queued `open` could not create its player: the name reports state 6 (error) with `errorCode`.
		void PublishOpenError(const char* name, int32_t errorCode);
		//! open: the extension check (.webm / .cevvideo, ASCII case-insensitive).
		bool IsOpenableSource(const char* src);

		//! cev_surfaces: one log line per surface (snapshot and aggregated hints).
		void LogSurfaces();
		//! Number of registered surfaces (the count cev_surfaces prints).
		uint32_t SurfaceCount();
		//! CryTexSourceApi::version this build exports.
		uint32_t ApiVersion();
	}
}
#endif
