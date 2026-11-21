// WinHttpFetcher: IFfmpegFetcher over WinHTTP (system component).
#include "CEVOffline/FfmpegInstall.h"

#include <cstdio>
#include <filesystem>
#include <vector>

#include "FfmpegInternal.h"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
#if defined(_WIN32)
			namespace
			{
				struct HInternet
				{
					HINTERNET h = nullptr;
					HInternet() = default;
					explicit HInternet(HINTERNET handle) : h(handle) {}
					~HInternet() { if (h != nullptr) WinHttpCloseHandle(h); }
					HInternet(const HInternet&) = delete;
					HInternet& operator=(const HInternet&) = delete;
				};

				bool IsLoopback(const std::wstring& host)
				{
					return host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1";
				}
			}

			Result WinHttpFetcher::Fetch(const std::string& url, const std::string& dstFile, const Control& ctl)
			{
				using detail::Res;
				bool ok = true;
				const std::wstring wurl = detail::Utf8ToWide(url, &ok);
				if (!ok || wurl.empty())
					return Res(Status::InvalidArgument, "bad URL");

				URL_COMPONENTSW uc = {};
				uc.dwStructSize = sizeof(uc);
				uc.dwSchemeLength = static_cast<DWORD>(-1);
				uc.dwHostNameLength = static_cast<DWORD>(-1);
				uc.dwUrlPathLength = static_cast<DWORD>(-1);
				uc.dwExtraInfoLength = static_cast<DWORD>(-1);
				if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
					return Res(Status::InvalidArgument, "cannot parse the URL " + url);
				const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
				const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
				if (!https && !(uc.nScheme == INTERNET_SCHEME_HTTP && IsLoopback(host)))
					return Res(Status::InvalidArgument, "only https URLs are allowed (http only to localhost)");
				std::wstring objectName(uc.lpszUrlPath, uc.dwUrlPathLength);
				objectName.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

				HInternet session(WinHttpOpen(L"CEVPlayer-ffmpeg-installer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
				if (session.h == nullptr)
					return Res(Status::IoError, "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")");
				WinHttpSetTimeouts(session.h, m_timeoutMs, m_timeoutMs, m_timeoutMs, m_timeoutMs);
				DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
				WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
				HInternet connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
				if (connect.h == nullptr)
					return Res(Status::IoError, "cannot connect to " + detail::WideToUtf8(host) + " (error " + std::to_string(GetLastError()) + ")");
				HInternet request(WinHttpOpenRequest(connect.h, L"GET", objectName.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
					https ? WINHTTP_FLAG_SECURE : 0));
				if (request.h == nullptr)
					return Res(Status::IoError, "WinHttpOpenRequest failed (error " + std::to_string(GetLastError()) + ")");
				// Redirects: GitHub release assets answer 302 to a CDN host. The default policy follows them (never https -> http).
				DWORD redirects = 10;
				WinHttpSetOption(request.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));

				if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr))
					return Res(Status::IoError, "request to " + url + " failed (error " + std::to_string(GetLastError()) + ")");

				DWORD status = 0;
				DWORD size = sizeof(status);
				WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
				if (status != 200)
					return Res(Status::IoError, "the server answered HTTP " + std::to_string(status) + " for " + url);
				uint64_t total = 0;
				{
					wchar_t len[32] = {};
					DWORD lsize = sizeof(len);
					if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsize, WINHTTP_NO_HEADER_INDEX))
						total = _wcstoui64(len, nullptr, 10);
				}

				const std::filesystem::path outPath = std::filesystem::u8path(dstFile);
				std::FILE* out = _wfopen(outPath.c_str(), L"wb");
				if (out == nullptr)
					return Res(Status::IoError, "cannot write " + dstFile);
				std::vector<char> buf(256 * 1024);
				uint64_t got = 0;
				Result result;
				for (;;)
				{
					if (ctl.cancel != nullptr && ctl.cancel->load(std::memory_order_relaxed))
					{
						result = Res(Status::Cancelled, "cancelled");
						break;
					}
					DWORD n = 0;
					if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &n))
					{
						result = Res(Status::IoError, "download interrupted (error " + std::to_string(GetLastError()) + ")");
						break;
					}
					if (n == 0)
						break;
					if (std::fwrite(buf.data(), 1, n, out) != n)
					{
						result = Res(Status::IoError, "cannot write " + dstFile + " (disk full?)");
						break;
					}
					got += n;
					if (ctl.progress && total != 0)
						ctl.progress(static_cast<double>(got) / static_cast<double>(total) * 0.999);
				}
				std::fclose(out);
				if (result.Ok() && total != 0 && got != total)
					result = Res(Status::IoError, "download is incomplete (" + std::to_string(got) + " of " + std::to_string(total) + " bytes)");
				if (!result.Ok())
				{
					std::error_code ec;
					std::filesystem::remove(outPath, ec);
					return result;
				}
				if (ctl.progress)
					ctl.progress(1.0);
				return result;
			}
#else
			Result WinHttpFetcher::Fetch(const std::string&, const std::string&, const Control&)
			{
				return detail::Res(Status::Unsupported, "WinHttpFetcher is only available on Windows");
			}
#endif
		}
	}
}
