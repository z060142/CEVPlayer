// Replaceable video decoder backends (M9, rulings R1 / R2).
//
// A backend takes one access unit (colour packet + optional alpha packet) together with a ticket that names the ring
// slot the picture goes into, and hands completed tickets back in submission order:
//
//   * Libvpx (software): decodes synchronously inside Submit() on the calling (pool) thread and writes the slot at
//     once; the ticket is available from Receive() right after. MaxInFlight() == 1.
//   * Media Foundation (Windows, hardware): the VP9 decoder MFT on a private D3D11 device. Everything COM / D3D lives
//     on one dedicated thread per backend ("CEV MF Decode"); Submit() only copies the packet into a bounded queue.
//     The decoded NV12 texture is copied to a staging texture, the context is flushed and the Map happens up to two
//     frames later (read-back delay), then Y and the interleaved UV are split into the slot's planes (packed layout).
//     MaxInFlight() == 3.
//
// The slot of a ticket belongs to the backend from Submit() until the ticket comes back through Receive() or is
// listed by Flush(). Tickets submitted without a slot (skip decoding: seek pre-roll, hidden decode-only, catch-up)
// advance the decoder, produce no picture, are not read back and never come back.
// No function throws.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "CEVCore/FrameRing.h"
#include "CEVCore/Status.h"
#include "CEVCore/WebmDemuxer.h"

namespace cev
{
	struct DecodedImageInfo;

	//! Decoder choice of a player (PlayerConfig::decodeMode).
	//!   Auto: Media Foundation when it is usable and the video is at least PlayerConfig::autoMinHeight tall.
	//!   Gpu:  Media Foundation when it is usable (VP8 and unusable hardware fall back to libvpx).
	//!   Cpu:  libvpx.
	enum class DecodeMode : uint8_t { Auto, Gpu, Cpu };

	enum class DecodeBackendKind : uint8_t { Libvpx, MediaFoundation };

	//! The backend a player actually uses (PlayerCore::GetDecodeBackend()).
	struct DecodeBackendInfo
	{
		DecodeBackendKind kind = DecodeBackendKind::Libvpx;
		bool hardware = false;      // decoded by the GPU's video engine
		bool alphaHardware = false; // the alpha stream as well (false: alpha by libvpx, or no alpha)
		// Why Media Foundation is not used (or stopped being used), empty when it was not asked for or is in use.
		char reason[192] = {};
		// DXGI adapter the private D3D11 device was created on (Media Foundation only).
		char adapter[128] = {};
	};

	struct DecodePacket
	{
		const uint8_t* data = nullptr;
		uint32_t size = 0;
		bool keyframe = false;
	};

	struct DecodeTicket
	{
		uint64_t epoch = 0;              // FrameRing epoch at AcquireWrite
		double pts = 0.0;                // seconds
		int slotIndex = -1;              // ring slot index, -1 = skip decode (no picture)
		FrameRing::Slot* slot = nullptr; // that slot's memory (null with slotIndex -1)
		// Filled by the backend for completed tickets (milliseconds; 0 when not measured).
		float decodeMs = 0.0f;           // libvpx decode / MF ProcessInput + ProcessOutput (colour + alpha)
		float copyMs = 0.0f;             // writing the planes into the slot
		float readbackMs = 0.0f;         // MF: copy to staging + Flush + Map (0 for libvpx)
	};

	//! Result codes of completed tickets (Receive) besides Ok:
	//!   Cancelled     the packet produced no visible picture (free the slot, not an error);
	//!   DecodeError   the decoder rejected the packet (free the slot, count an error);
	//!   InvalidData   the picture does not fit the ring format (stream change: compare LastFrameInfo());
	//!   BackendFailed the backend cannot continue (device removed, unexpected output): switch to another backend.
	class IVideoDecoderBackend
	{
	public:
		virtual ~IVideoDecoderBackend() = default;

		// The ring format the pictures are written in (before the first Submit with a slot, and after a ring change).
		virtual void SetOutputFormat(const FrameFormat& format) = 0;

		// Ok = consumed; Busy = try again later (nothing was consumed, the slot is still the caller's);
		// DecodeError / Unsupported (libvpx: rejected / high bit depth or oversized) = consumed but failed, the ticket
		// does not come back; BackendFailed = this backend is unusable (the ticket does not come back).
		virtual Status Submit(const DecodePacket& color, const DecodePacket* alpha, const DecodeTicket& ticket) = 0;
		// A completed ticket in submission order: true and `out` / `result` set, false = nothing completed yet.
		virtual bool Receive(DecodeTicket& out, Status& result) = 0;
		// Forgets everything in flight (seek, catch-up jump, unload). Afterwards no earlier ticket comes back; the slot
		// indices of the tickets that were in flight (completed or not) are written to `abandoned` (up to `capacity`,
		// the return value is the number written) for the caller to free. Blocks until the backend confirmed it.
		virtual uint32_t Flush(int* abandoned, uint32_t capacity) = 0;
		// No more input for now (end of stream): push out the pictures still inside the decoder.
		virtual void Drain() = 0;
		virtual uint32_t MaxInFlight() const = 0;
		virtual DecodeBackendInfo Info() const = 0;
		// Size / subsampling of the last picture produced (stream change diagnostics). false = none yet.
		virtual bool LastFrameInfo(DecodedImageInfo& out) const = 0;
	};

	// ---- libvpx ----
	// threads: libvpx worker threads (>= 1). The ring format is learnt from the first picture: LibvpxFirstFrame().
	Status CreateLibvpxBackend(const VideoStreamInfo& stream, uint32_t threads, std::unique_ptr<IVideoDecoderBackend>& out);
	// Load helper for the libvpx backend: decodes one packet without a slot and tells whether it produced a picture
	// (`info` set); LibvpxWriteLastFrame() then copies that picture into a slot (valid until the next Submit).
	Status LibvpxDecodeProbe(IVideoDecoderBackend& backend, const DecodePacket& color, const DecodePacket* alpha, bool& gotPicture);
	Status LibvpxWriteLastFrame(IVideoDecoderBackend& backend, const FrameFormat& format, FrameRing::Slot& slot);

	// ---- Media Foundation ----
	struct MediaFoundationConfig
	{
		VideoStreamInfo stream;          // VP9 only; container size = ring size (4:2:0)
		uint32_t maxPacketSize = 0;      // largest colour packet
		uint32_t maxAdditionalSize = 0;  // largest alpha packet
		uint32_t alphaThreads = 2;       // libvpx threads when the alpha stream falls back to libvpx
		// Called on the backend thread whenever a ticket completed or queue space became free (wakes the pool task).
		void (*wake)(void* context) = nullptr;
		void* wakeContext = nullptr;
	};

	//! True when this build contains the Media Foundation backend (Windows).
	bool MediaFoundationCompiled();
	// Creates the backend and its thread and waits (bounded) until the thread opened the device and the MFTs.
	// Fails (out stays null, `reason` says why) when the MFT is missing, SetInputType fails with the D3D manager
	// (= no hardware decode), the device cannot be created, the codec is not VP9, ... Hardware is fully confirmed by
	// the first output sample being an IMFDXGIBuffer; otherwise the first ticket completes with BackendFailed.
	Status CreateMediaFoundationBackend(const MediaFoundationConfig& config, std::unique_ptr<IVideoDecoderBackend>& out,
		char* reason, size_t reasonSize);

	//! Test switches (selftest only; process wide, take effect for backends created afterwards).
	enum class MediaFoundationTestFault : uint8_t
	{
		None,
		NoDecoderMft,        // the MFT enumeration returns nothing
		SetInputTypeFails,   // SetInputType fails as on a GPU without VP9 decode
		DeviceLostAfter,     // after `param` completed pictures the backend behaves as on DXGI_ERROR_DEVICE_REMOVED
	};
	void SetMediaFoundationTestFault(MediaFoundationTestFault fault, uint32_t param = 0);
	//! Media Foundation start-ups currently held by backend threads (MFStartup / MFShutdown are paired: 0 when no
	//! backend exists).
	uint32_t MediaFoundationStartupCount();
	//! Backend threads that did not finish within the bounded join of the destructor (2 s) and were left running
	//! detached (a defect to report; the module logs it).
	uint32_t MediaFoundationShutdownTimeouts();
}
