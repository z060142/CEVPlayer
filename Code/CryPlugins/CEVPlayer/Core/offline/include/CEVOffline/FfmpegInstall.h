// FfmpegInstall: download + verify + extract our minimal ffmpeg build.
//
//   * The plugin hard-codes the version and the SHA-256 of the release zip (kFfmpegMinimalVersion /
//     kFfmpegMinimalSha256). Until a release exists they are "unreleased" / "": downloading is disabled and
//     FfmpegDownloadDisabledReason() says so.
//   * The zip comes through an IFfmpegFetcher (WinHTTP in the editor, a local file in the selftest). Its SHA-256 is
//     checked BEFORE anything is extracted; a mismatch deletes the download and refuses (Status::InvalidData).
//   * Extraction uses the Windows 10 (1803+) bsdtar in %SystemRoot%\System32\tar.exe, which reads zip archives.
//     Nothing is vendored. The archive is extracted into a staging folder, checked (no links, nothing outside
//     the folder) and only then moved to the install folder, so a failed install never leaves a half copy.
//   * The install folder lives in the editor tools / user folder (DefaultFfmpegInstallRoot()), never in the
//     project and never in a pak.
#pragma once

#include <cstdint>
#include <string>

#include "CEVOffline/Offline.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			// ---- pinned release (update both together with every new ffmpeg-minimal-v* release) ------------------------
			constexpr const char* kFfmpegMinimalVersion = "unreleased";
			constexpr const char* kFfmpegMinimalSha256 = ""; // 64 lower case hex digits of the release zip
			constexpr const char* kFfmpegMinimalRepo = "z060142/CEVPlayer";

			// "ffmpeg-minimal-<version>-win64.zip"
			std::string FfmpegMinimalZipName(const std::string& version);
			// https://github.com/<repo>/releases/download/ffmpeg-minimal-v<version>/<zip name>
			std::string FfmpegMinimalDownloadUrl(const std::string& version);
			// Both constants set (version != "unreleased", a 64 digit hash).
			bool FfmpegDownloadEnabled();
			bool FfmpegDownloadEnabled(const std::string& version, const std::string& sha256);
			std::string FfmpegDownloadDisabledReason(); // empty when enabled

			// %LOCALAPPDATA%\CEVPlayer\ffmpeg (UTF-8), empty when unknown. The install folder of a version is
			// <root>\<version>; that folder is what LocateOptions::downloadedDir takes.
			std::string DefaultFfmpegInstallRoot();
			std::string FfmpegInstallDir(const std::string& root, const std::string& version);

			// ---- SHA-256 (FIPS 180-4) --------------------------------------------------------------------------------
			class Sha256
			{
			public:
				Sha256() { Reset(); }
				void Reset();
				void Update(const void* data, size_t size);
				std::string FinalHex(); // 64 lower case hex digits; the object must be Reset() before reuse

			private:
				void Block(const uint8_t* block);
				uint32_t m_state[8];
				uint8_t m_buffer[64];
				uint64_t m_total = 0;
				size_t m_fill = 0;
			};
			std::string Sha256Hex(const void* data, size_t size);
			Result Sha256FileHex(const std::string& utf8Path, std::string& hexOut, const Control& ctl = Control());

			// ---- fetching --------------------------------------------------------------------------------------------
			class IFfmpegFetcher
			{
			public:
				virtual ~IFfmpegFetcher() = default;
				// Writes the resource at `url` to `dstFile` (overwritten). Progress 0..1 when the size is known.
				// Failure / cancel: the partial dstFile is removed by the caller.
				virtual Result Fetch(const std::string& url, const std::string& dstFile, const Control& ctl) = 0;
			};

			// url is a local path or file:///C:/path (selftests, offline installs).
			class LocalFileFetcher final : public IFfmpegFetcher
			{
			public:
				Result Fetch(const std::string& url, const std::string& dstFile, const Control& ctl) override;
			};

			// WinHTTP (system component, no extra dependency, follows redirects: GitHub release assets redirect to a
			// CDN). https only, plus http to a loopback host. Windows only (Unsupported elsewhere).
			class WinHttpFetcher final : public IFfmpegFetcher
			{
			public:
				explicit WinHttpFetcher(int timeoutMs = 30000) : m_timeoutMs(timeoutMs) {}
				Result Fetch(const std::string& url, const std::string& dstFile, const Control& ctl) override;

			private:
				int m_timeoutMs;
			};

			// ---- install ---------------------------------------------------------------------------------------------
			struct FfmpegPackage
			{
				std::string version;
				std::string sha256;     // expected, hex, case insensitive
				std::string url;        // what the fetcher gets
			};

			// The pinned package (url built from the constants). Empty fields when download is disabled.
			FfmpegPackage PinnedFfmpegPackage();

			// Verifies `zipPath` against `expectedSha256` and installs it into installDir (see the header comment).
			// Status::InvalidData on a hash mismatch (nothing extracted, the zip is NOT deleted: the caller owns it).
			// installedExe receives <installDir>\ffmpeg.exe (or ...\bin\ffmpeg.exe when the zip has that layout).
			Result InstallFfmpegFromZip(const std::string& zipPath, const std::string& expectedSha256, const std::string& installDir,
				std::string* installedExe = nullptr, const Control& ctl = Control());

			// Fetch (to "<installDir>.download"), verify, install, delete the download. Disabled package (empty
			// version / hash) -> Status::Unsupported with FfmpegDownloadDisabledReason()-style text. progress: fetch
			// 0 .. 0.8, verify + extract .. 1.
			Result DownloadAndInstallFfmpeg(const FfmpegPackage& package, IFfmpegFetcher& fetcher, const std::string& installDir,
				std::string* installedExe = nullptr, const Control& ctl = Control());
		}
	}
}
