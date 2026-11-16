// FfmpegRunner: portable parts (quoting, progress parser, text helpers). The process code is in ProcessWin.cpp.
#include "CEVOffline/FfmpegRunner.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "FfmpegInternal.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			namespace detail
			{
				std::vector<std::string> SplitLines(const std::string& text)
				{
					std::vector<std::string> lines;
					size_t start = 0;
					while (start <= text.size())
					{
						size_t end = text.find('\n', start);
						if (end == std::string::npos)
							end = text.size();
						size_t e = end;
						if (e > start && text[e - 1] == '\r')
							--e;
						lines.emplace_back(text, start, e - start);
						start = end + 1;
					}
					if (!lines.empty() && lines.back().empty())
						lines.pop_back();
					return lines;
				}

				std::string Trim(const std::string& s)
				{
					size_t b = 0;
					size_t e = s.size();
					while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
						++b;
					while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
						--e;
					return s.substr(b, e - b);
				}

				std::string ToLower(std::string s)
				{
					for (char& c : s)
						if (c >= 'A' && c <= 'Z')
							c = static_cast<char>(c - 'A' + 'a');
					return s;
				}
			}

			std::string QuoteWindowsArg(const std::string& arg)
			{
				if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
					return arg;
				std::string out = "\"";
				size_t backslashes = 0;
				for (const char c : arg)
				{
					if (c == '\\')
					{
						++backslashes;
						continue;
					}
					if (c == '"')
					{
						out.append(backslashes * 2 + 1, '\\');
						out.push_back('"');
					}
					else
					{
						out.append(backslashes, '\\');
						out.push_back(c);
					}
					backslashes = 0;
				}
				out.append(backslashes * 2, '\\'); // before the closing quote
				out.push_back('"');
				return out;
			}

			std::string BuildWindowsCommandLine(const std::string& exe, const std::vector<std::string>& args)
			{
				// The program name is parsed with simpler rules (no escapes): quote it plainly.
				std::string line = "\"" + exe + "\"";
				for (const std::string& a : args)
				{
					line.push_back(' ');
					line += QuoteWindowsArg(a);
				}
				return line;
			}

			// ---------------------------------------------------------------------------------------------------
			// Progress stream
			// ---------------------------------------------------------------------------------------------------
			void ProgressParser::Feed(const char* data, size_t size)
			{
				for (size_t i = 0; i < size; ++i)
				{
					const char c = data[i];
					if (c == '\n')
					{
						if (!m_line.empty() && m_line.back() == '\r')
							m_line.pop_back();
						Line(m_line);
						m_line.clear();
					}
					else if (m_line.size() < 4096) // a line of the progress stream is short; ignore garbage beyond that
					{
						m_line.push_back(c);
					}
				}
			}

			void ProgressParser::Line(const std::string& line)
			{
				const size_t eq = line.find('=');
				if (eq == std::string::npos || eq == 0)
					return;
				const std::string key = line.substr(0, eq);
				const std::string value = detail::Trim(line.substr(eq + 1));
				char* end = nullptr;
				if (key == "out_time_us" || key == "out_time_ms") // out_time_ms is microseconds too (historic ffmpeg bug)
				{
					if (value.empty() || value == "N/A")
						return;
					const long long v = std::strtoll(value.c_str(), &end, 10);
					if (end == value.c_str() || v < 0) // ffmpeg prints a huge negative number before the first timestamp
						return;
					if (key == "out_time_us" || !m_block.hasTime)
					{
						m_block.hasTime = true;
						m_block.outTimeSeconds = static_cast<double>(v) / 1000000.0;
					}
				}
				else if (key == "frame")
				{
					m_block.frame = std::strtoll(value.c_str(), nullptr, 10);
				}
				else if (key == "fps")
				{
					m_block.fps = std::strtod(value.c_str(), nullptr);
				}
				else if (key == "total_size")
				{
					if (value != "N/A")
						m_block.totalSize = std::strtoll(value.c_str(), nullptr, 10);
				}
				else if (key == "speed")
				{
					m_block.speed = value == "N/A" ? 0.0 : std::strtod(value.c_str(), nullptr); // "1.25x"
				}
				else if (key == "progress")
				{
					m_block.ended = (value == "end");
					const ProgressBlock done = m_block;
					m_block = ProgressBlock();
					if (m_cb)
						m_cb(done);
				}
			}

			double ProgressFraction(double outTimeSeconds, double durationSeconds)
			{
				if (!(durationSeconds > 0.0) || !(outTimeSeconds > 0.0))
					return 0.0;
				const double f = outTimeSeconds / durationSeconds;
				if (f >= 1.0)
					return 0.999;
				return f;
			}
		}
	}
}
