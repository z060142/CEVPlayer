#include "StdAfx.h"
#include "Offline/IOfflineOps.h"

namespace CEVEditor
{
	namespace
	{
		SOpResult NotWired(const char* what)
		{
			SOpResult r;
			r.status = EOpStatus::NotAvailable;
			r.message = std::string(what) + ": the offline processing library (CEVPlayerOffline) is not wired into this build";
			return r;
		}

		// Stub: every operation reports NotAvailable. replaced by SetOfflineOps().
		class CStubOfflineOps final : public IOfflineOps
		{
		public:
			const char* Name() const override { return "stub (CEVPlayerOffline not wired)"; }
			bool IsAvailable() const override { return false; }
			SOpResult ProbeMedia(const std::string&, SMediaInfo&, const SOpContext&) override { return NotWired("probe"); }
			SOpResult ExtractAudioWebm(const std::string&, int, const std::string&, const SOpContext&) override { return NotWired("split audio"); }
			SOpResult ExportWav(const std::string&, int, const std::string&, const SOpContext&) override { return NotWired("export WAV"); }
			SOpResult ExportOggVorbis(const std::string&, int, const std::string&, float, const SOpContext&) override { return NotWired("export Ogg"); }
			SOpResult StripAudio(const std::string&, const std::string&, const SOpContext&) override { return NotWired("strip audio"); }
		};

		CStubOfflineOps g_stub;
		IOfflineOps* g_pOps = nullptr;
	}

	IOfflineOps& GetOfflineOps()
	{
		return g_pOps != nullptr ? *g_pOps : static_cast<IOfflineOps&>(g_stub);
	}

	void SetOfflineOps(IOfflineOps* pOps)
	{
		g_pOps = pOps;
	}
}
