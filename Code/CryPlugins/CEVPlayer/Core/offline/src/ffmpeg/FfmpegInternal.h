// CEVPlayerFFmpeg internals shared by the .cpp files (not part of the public interface).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "CEVOffline/Offline.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			namespace detail
			{
				inline Result Res(Status status, std::string message)
				{
					Result r;
					r.status = status;
					r.message = std::move(message);
					return r;
				}
				inline Result Okay() { return Result(); }

				// Captured process output with a size cap: the first part and the last part are kept, the middle is
				// dropped (a marker line tells how much).
				class TextCapture
				{
				public:
					explicit TextCapture(size_t cap = 4u << 20)
						: m_headMax(std::min<size_t>(cap / 4, 256u << 10)), m_tailMax(cap > m_headMax ? cap - m_headMax : 0) {}

					void Append(const char* data, size_t size)
					{
						if (m_head.size() < m_headMax)
						{
							const size_t n = std::min(size, m_headMax - m_head.size());
							m_head.append(data, n);
							data += n;
							size -= n;
						}
						if (size == 0)
							return;
						m_tail.append(data, size);
						if (m_tail.size() > 2 * m_tailMax + 1)
						{
							const size_t drop = m_tail.size() - m_tailMax;
							m_tail.erase(0, drop);
							m_dropped += drop;
						}
					}

					std::string Take()
					{
						if (m_tail.size() > m_tailMax)
						{
							const size_t drop = m_tail.size() - m_tailMax;
							m_tail.erase(0, drop);
							m_dropped += drop;
						}
						std::string out = std::move(m_head);
						if (m_dropped != 0)
							out += "\n[... " + std::to_string(m_dropped) + " bytes dropped ...]\n";
						out += m_tail;
						m_head.clear();
						m_tail.clear();
						return out;
					}

				private:
					size_t m_headMax;
					size_t m_tailMax;
					size_t m_dropped = 0;
					std::string m_head;
					std::string m_tail;
				};

				// "a\r\nb\n" -> lines (no terminators); empty lines kept.
				std::vector<std::string> SplitLines(const std::string& text);
				std::string Trim(const std::string& s);
				std::string ToLower(std::string s);

#if defined(_WIN32)
				std::wstring Utf8ToWide(const std::string& utf8, bool* ok = nullptr);
				std::string WideToUtf8(const std::wstring& wide);
#endif
			}
		}
	}
}
