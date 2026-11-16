// CEVPlayerCore - CRYENGINE independent decoding core.
// Status codes: every public function returns a Status, nothing throws, nothing prints.
#pragma once

#include <cstdint>

namespace cev
{
	enum class Status : uint32_t
	{
		Ok = 0,
		InvalidArgument,
		InvalidState,   // call not allowed in the current player state
		NotReady,       // media is not loaded yet (Load() is asynchronous)
		OutOfMemory,
		IoError,        // byte source failure
		OutOfRange,     // read past the end of a byte source / bad index
		InvalidData,    // corrupt or unexpected container / bitstream data
		Unsupported,    // valid file, feature or codec not supported
		DecodeError,    // codec reported an error for one packet
		Cancelled,      // operation aborted by Unload()/destruction
		LoadFailed,     // Load() finished with an error, see PlayerCore::GetLoadError()
	};

	// Optional second code that says where inside an operation the failure happened.
	enum class StatusDetail : uint16_t
	{
		None = 0,
		EbmlHeader,
		SegmentParse,
		TracksParse,
		ClusterScan,
		NoSupportedTrack,
		ContentEncoding,
		BadCodecPrivate,
		VideoDecoderInit,
		AudioDecoderInit,
		FirstFrame,
		RingInit,
		PoolRegister,
	};

	inline bool Succeeded(Status s) { return s == Status::Ok; }
	inline bool Failed(Status s) { return s != Status::Ok; }

	const char* StatusToString(Status status);
	const char* StatusDetailToString(StatusDetail detail);

	struct LoadError
	{
		Status status = Status::Ok;
		StatusDetail detail = StatusDetail::None;
	};
}
