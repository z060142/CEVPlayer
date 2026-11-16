// Integration point for the offline processing library (CEVPlayerOffline: probe, extract audio, strip audio, WAV, Ogg).
//
// The editor plug-in never calls the library directly: everything goes through IOfflineOps. OfflineOpsReal.cpp implements it
// over CEVPlayerOffline (Core/offline) and the plug-in installs it at start up (EditorCEVPlayerPlugin.cpp: SetOfflineOps).
// OfflineOps.cpp keeps a stub that reports EOpStatus::NotAvailable; it is what GetOfflineOps() returns until SetOfflineOps()
// was called (and after SetOfflineOps(nullptr)), so the editor still works - minus the audio operations - without the library.
//
// Threading: the methods run on a job thread (never the main thread, never inside the engine's render lock): they must
// not touch the engine (gEnv, Qt, renderer). They return a result instead of throwing. `cancel` is polled by long
// operations, `progress` (0..1) and `log` are called from the job thread (the job runner marshals them to the UI).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace CEVEditor
{
	enum class EOpStatus
	{
		Ok,
		NotAvailable, //!< the offline library is not wired into this build
		Failed,
		Cancelled,
	};

	struct SOpResult
	{
		EOpStatus status = EOpStatus::NotAvailable;
		std::string message;   //!< error / warning text for the log (complete, not shortened)
		bool Ok() const { return status == EOpStatus::Ok; }
	};

	struct SOpContext
	{
		const std::atomic<bool>* cancel = nullptr;
		std::function<void(float)> progress;                  //!< 0..1, may be empty
		std::function<void(const std::string&)> log;          //!< one line per call, may be empty
		void Progress(float p) const { if (progress) progress(p); }
		void Log(const std::string& s) const { if (log) log(s); }
		bool Cancelled() const { return cancel != nullptr && cancel->load(); }
	};

	struct SAudioTrackInfo
	{
		std::string codec;          //!< "opus" / "vorbis"
		int sampleRate = 0;
		int channels = 0;
		double startOffsetSec = 0;  //!< time of the first retained sample (pts of the first packet minus codec delay)
		double codecDelaySec = 0;   //!< Opus pre-skip / codec delay
	};

	struct SMediaInfo
	{
		bool valid = false;
		std::string videoCodec;     //!< "vp8" / "vp9"
		int width = 0;
		int height = 0;
		double fps = 0;
		double durationSec = 0;
		bool hasAlpha = false;
		std::vector<SAudioTrackInfo> audioTracks;
	};

	struct IOfflineOps
	{
		virtual ~IOfflineOps() {}

		virtual const char* Name() const = 0;
		//! False for the stub: the editor shows "offline library not available" next to the operation buttons.
		virtual bool IsAvailable() const = 0;

		//! CEVPlayerOffline::ProbeMedia. (The info panel falls back to ReadBasicMediaInfo()
		//! of MediaInfo.h, which only knows the first audio track, while this returns every track.)
		virtual SOpResult ProbeMedia(const std::string& absSrc, SMediaInfo& out, const SOpContext& ctx) = 0;
		//! CEVPlayerOffline::ExtractAudioWebm (re-mux of audio track `trackIndex` into an audio only
		//! WebM, no re-encoding).
		virtual SOpResult ExtractAudioWebm(const std::string& absSrc, int trackIndex, const std::string& absDst, const SOpContext& ctx) = 0;
		//! CEVPlayerOffline::ExportWav (48 kHz / 16 bit, sample 0 = video second 0).
		virtual SOpResult ExportWav(const std::string& absSrc, int trackIndex, const std::string& absDst, const SOpContext& ctx) = 0;
		//! CEVPlayerOffline::ExportOggVorbis (`quality` -0.1 .. 1.0, vorbisenc scale).
		virtual SOpResult ExportOggVorbis(const std::string& absSrc, int trackIndex, const std::string& absDst, float quality, const SOpContext& ctx) = 0;
		//! CEVPlayerOffline::StripAudio (picture only copy of the video).
		virtual SOpResult StripAudio(const std::string& absSrc, const std::string& absDst, const SOpContext& ctx) = 0;
	};

	//! The implementation in use (never null). The stub until SetOfflineOps() is called.
	IOfflineOps& GetOfflineOps();
	//! Replaces the implementation (nullptr = back to the stub). Main thread, at plug-in start up.
	void SetOfflineOps(IOfflineOps* pOps);
}
