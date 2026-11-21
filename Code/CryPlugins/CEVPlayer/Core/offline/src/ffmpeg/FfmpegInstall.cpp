#include <cstdlib>
#include "CEVOffline/FfmpegInstall.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "CEVOffline/FfmpegLocator.h"
#include "CEVOffline/FfmpegRunner.h"
#include "FfmpegInternal.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			namespace
			{
				namespace fs = std::filesystem;

				bool IsHex64(const std::string& s)
				{
					return s.size() == 64 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
				}

				bool SafeVersion(const std::string& v)
				{
					return !v.empty() && v.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos;
				}

				bool Cancelled(const Control& ctl) { return ctl.cancel != nullptr && ctl.cancel->load(std::memory_order_relaxed); }

				void RemoveAll(const fs::path& p)
				{
					std::error_code ec;
					fs::remove_all(p, ec);
				}

				std::string SystemTarPath()
				{
#if defined(_WIN32)
					wchar_t dir[MAX_PATH] = {};
					const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
					if (n == 0 || n >= MAX_PATH)
						return std::string();
					return detail::WideToUtf8(std::wstring(dir, n)) + "\\tar.exe";
#else
					return "tar";
#endif
				}

				// Everything below `staging` must be a plain file or folder inside it.
				Result ValidateStaging(const fs::path& staging)
				{
					using detail::Res;
					std::error_code ec;
					const fs::path root = fs::weakly_canonical(staging, ec);
					if (ec)
						return Res(Status::IoError, "cannot resolve the staging folder");
					size_t count = 0;
					for (fs::recursive_directory_iterator it(staging, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
					{
						if (++count > 20000)
							return Res(Status::InvalidData, "the archive holds an unreasonable number of files");
						const fs::path& p = it->path();
						std::error_code e2;
						if (fs::is_symlink(fs::symlink_status(p, e2)))
							return Res(Status::InvalidData, "the archive contains a link: " + p.u8string());
						const fs::path canon = fs::weakly_canonical(p, e2);
						auto r = root.begin();
						auto c = canon.begin();
						for (; r != root.end() && c != canon.end(); ++r, ++c)
							if (detail::ToLower(r->u8string()) != detail::ToLower(c->u8string()))
								break;
						if (r != root.end())
							return Res(Status::InvalidData, "the archive extracts outside the target folder: " + p.u8string());
					}
					if (ec)
						return Res(Status::IoError, "cannot scan the extracted files: " + ec.message());
					return detail::Okay();
				}

				// Shallowest ffmpeg.exe below the staging folder (depth <= 3).
				fs::path FindFfmpegIn(const fs::path& staging)
				{
					const std::string name = detail::ToLower(
#if defined(_WIN32)
						"ffmpeg.exe"
#else
						"ffmpeg"
#endif
					);
					fs::path best;
					int bestDepth = 99;
					std::error_code ec;
					for (fs::recursive_directory_iterator it(staging, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
					{
						if (it.depth() > 3 || !it->is_regular_file(ec))
							continue;
						if (detail::ToLower(it->path().filename().u8string()) == name && it.depth() < bestDepth)
						{
							best = it->path();
							bestDepth = it.depth();
						}
					}
					return best;
				}
			}

			std::string FfmpegMinimalZipName(const std::string& version)
			{
				return "ffmpeg-minimal-" + version + "-win64.zip";
			}

			std::string FfmpegMinimalDownloadUrl(const std::string& version)
			{
				return std::string("https://github.com/") + kFfmpegMinimalRepo + "/releases/download/ffmpeg-minimal-v" + version + "/" + FfmpegMinimalZipName(version);
			}

			bool FfmpegDownloadEnabled(const std::string& version, const std::string& sha256)
			{
				return SafeVersion(version) && version != "unreleased" && IsHex64(sha256);
			}

			bool FfmpegDownloadEnabled()
			{
				return FfmpegDownloadEnabled(kFfmpegMinimalVersion, kFfmpegMinimalSha256);
			}

			std::string FfmpegDownloadDisabledReason()
			{
				if (FfmpegDownloadEnabled())
					return std::string();
				return "The FFmpeg download is not available yet: no minimal build has been released (version '" + std::string(kFfmpegMinimalVersion) +
					"', no SHA-256 pinned in the plugin). Choose an ffmpeg executable manually.";
			}

			std::string DefaultFfmpegInstallRoot()
			{
#if defined(_WIN32)
				const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
				if (n == 0)
					return std::string();
				std::wstring w(n, L'\0');
				const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", &w[0], n);
				w.resize(got);
				return detail::WideToUtf8(w) + "\\CEVPlayer\\ffmpeg";
#else
				return std::string();
#endif
			}

			std::string FfmpegInstallDir(const std::string& root, const std::string& version)
			{
				if (root.empty() || !SafeVersion(version))
					return std::string();
				return (fs::u8path(root) / fs::u8path(version)).u8string();
			}

			FfmpegPackage PinnedFfmpegPackage()
			{
				FfmpegPackage p;
				if (!FfmpegDownloadEnabled())
					return p;
				p.version = kFfmpegMinimalVersion;
				p.sha256 = kFfmpegMinimalSha256;
				p.url = FfmpegMinimalDownloadUrl(p.version);
				return p;
			}

			bool IsFfmpegConnectionError(unsigned long e)
			{
				switch (e)
				{
				case 12002: case 12007: case 12029: case 12030: case 12031: case 12152: case 12157: case 12175:
					return true;
				default:
					return false;
				}
			}

			std::string ExplainFfmpegDownloadFailure(const std::string& fetchMessage, const FfmpegPackage& package)
			{
				const size_t at = fetchMessage.rfind("(error ");
				if (at == std::string::npos)
					return fetchMessage;
				const unsigned long code = std::strtoul(fetchMessage.c_str() + at + 7, nullptr, 10);
				if (!IsFfmpegConnectionError(code))
					return fetchMessage;
				return "The FFmpeg download could not connect: the connection was blocked or is unavailable (commonly a firewall or "
					"security software blocking the editor, or no network). To continue manually: download " + package.url +
					" with a browser, check that its SHA-256 is " + package.sha256 + ", unpack it anywhere and set the path to ffmpeg.exe in "
					"Edit > Preferences > CEVPlayer/FFmpeg. [" + fetchMessage + "]";
			}

			Result LocalFileFetcher::Fetch(const std::string& urlIn, const std::string& dstFile, const Control& ctl)
			{
				using detail::Res;
				std::string src = urlIn;
				if (src.compare(0, 8, "file:///") == 0)
					src = src.substr(8);
				std::error_code ec;
				const fs::path from = fs::u8path(src);
				if (!fs::is_regular_file(from, ec))
					return Res(Status::IoError, "no such file: " + src);
				if (Cancelled(ctl))
					return Res(Status::Cancelled, "cancelled");
				fs::copy_file(from, fs::u8path(dstFile), fs::copy_options::overwrite_existing, ec);
				if (ec)
					return Res(Status::IoError, "cannot copy " + src + ": " + ec.message());
				if (ctl.progress)
					ctl.progress(1.0);
				return detail::Okay();
			}

			Result InstallFfmpegFromZip(const std::string& zipPath, const std::string& expectedSha256, const std::string& installDir,
				std::string* installedExe, const Control& ctl)
			{
				using detail::Res;
				if (installedExe != nullptr)
					installedExe->clear();
				if (installDir.empty())
					return Res(Status::InvalidArgument, "install folder is empty");
				if (!IsHex64(expectedSha256))
					return Res(Status::InvalidArgument, "no valid SHA-256 to verify against: refusing to install an unverified download");

				// 1. hash first: nothing of an unverified archive is ever extracted
				std::string actual;
				Result r = Sha256FileHex(zipPath, actual, ctl);
				if (!r.Ok())
					return r;
				if (actual != detail::ToLower(expectedSha256))
					return Res(Status::InvalidData, "SHA-256 mismatch: the download is " + actual + ", expected " + detail::ToLower(expectedSha256) + "; refusing to install");
				if (Cancelled(ctl))
					return Res(Status::Cancelled, "cancelled");

				// 2. extract into a staging folder next to the target
				const fs::path target = fs::u8path(installDir);
				const fs::path staging = fs::u8path(installDir + ".staging");
				std::error_code ec;
				RemoveAll(staging);
				fs::create_directories(staging, ec);
				if (ec)
					return Res(Status::IoError, "cannot create " + staging.u8string() + ": " + ec.message());

				const std::string tar = SystemTarPath();
				if (tar.empty() || !fs::exists(fs::u8path(tar), ec))
					{
						RemoveAll(staging);
						return Res(Status::Unsupported, "tar.exe (Windows 10 1803 or later) is needed to unpack the zip and was not found");
					}
				RunOptions run;
				run.exe = tar;
				run.args = {"-xf", zipPath, "-C", staging.u8string()};
				run.cancel = ctl.cancel;
				run.noProgressTimeoutMs = 0;
				run.totalTimeoutMs = 10 * 60 * 1000;
				const RunResult rr = RunProcess(run);
				if (!rr.result.Ok())
				{
					RemoveAll(staging);
					Result fail = rr.result;
					if (rr.result.status == Status::InvalidData)
						fail.message = "tar could not unpack the archive: " + rr.stderrLog;
					return fail;
				}

				// 3. checks, find the payload, move it into place
				r = ValidateStaging(staging);
				if (!r.Ok())
				{
					RemoveAll(staging);
					return r;
				}
				const fs::path exe = FindFfmpegIn(staging);
				if (exe.empty())
				{
					RemoveAll(staging);
					return Res(Status::InvalidData, "the archive does not contain ffmpeg.exe");
				}
				fs::path payload = exe.parent_path();
				if (detail::ToLower(payload.filename().u8string()) == "bin" && payload != staging)
					payload = payload.parent_path();

				RemoveAll(target);
				if (target.has_parent_path())
					fs::create_directories(target.parent_path(), ec);
				fs::rename(payload, target, ec);
				if (ec)
				{
					RemoveAll(staging);
					return Res(Status::IoError, "cannot move the extracted files to " + installDir + ": " + ec.message());
				}
				RemoveAll(staging);

				std::string installedPath = ResolveExecutable(installDir);
				if (installedPath.empty())
					return Res(Status::InvalidData, "ffmpeg.exe is missing after the install");
				// Remember what was installed (informational; the locator re-probes the executable anyway).
				{
					const fs::path marker = target / "cev-ffmpeg-install.txt";
					std::FILE* f = nullptr;
#if defined(_WIN32)
					f = _wfopen(marker.c_str(), L"wb");
#else
					f = std::fopen(marker.c_str(), "wb");
#endif
					if (f != nullptr)
					{
						const std::string text = "sha256=" + actual + "\n";
						std::fwrite(text.data(), 1, text.size(), f);
						std::fclose(f);
					}
				}
				if (installedExe != nullptr)
					*installedExe = installedPath;
				return detail::Okay();
			}

			Result DownloadAndInstallFfmpeg(const FfmpegPackage& package, IFfmpegFetcher& fetcher, const std::string& installDir,
				std::string* installedExe, const Control& ctl)
			{
				using detail::Res;
				if (installedExe != nullptr)
					installedExe->clear();
				if (!FfmpegDownloadEnabled(package.version, package.sha256) || package.url.empty())
					return Res(Status::Unsupported, FfmpegDownloadDisabledReason().empty()
						? std::string("This FFmpeg package is not downloadable (empty version, hash or URL).")
						: FfmpegDownloadDisabledReason());
				if (installDir.empty())
					return Res(Status::InvalidArgument, "install folder is empty");

				std::error_code ec;
				const std::string zipPath = installDir + ".download";
				const fs::path zip = fs::u8path(zipPath);
				if (zip.has_parent_path())
					fs::create_directories(zip.parent_path(), ec);
				fs::remove(zip, ec);

				Control fetchCtl;
				fetchCtl.cancel = ctl.cancel;
				if (ctl.progress)
					fetchCtl.progress = [&ctl](double f) { ctl.progress(0.8 * f); };
				Result r = fetcher.Fetch(package.url, zipPath, fetchCtl);
				if (!r.Ok())
				{
					fs::remove(zip, ec);
					if (r.status != Status::Cancelled)
						r.message = ExplainFfmpegDownloadFailure(r.message, package);
					return r;
				}
				if (ctl.progress)
					ctl.progress(0.8);
				r = InstallFfmpegFromZip(zipPath, package.sha256, installDir, installedExe, ctl);
				fs::remove(zip, ec); // on a mismatch the bad download must not stay around either
				if (r.Ok() && ctl.progress)
					ctl.progress(1.0);
				return r;
			}
		}
	}
}
