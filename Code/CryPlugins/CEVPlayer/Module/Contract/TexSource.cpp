#include "StdAfx.h"
#include "PluginEnv.h"
#include "Contract/TexSource.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "CEVCore/PlayerCore.h"
#include "Player/PlayerManager.h"

#include <CryCore/Platform/CryWindows.h>

// UI-SOURCE.md section 4 (Threading): the contract functions (Query, Hint, OpenSurface, CloseSurface,
// Control) make no engine call. They only touch the table below, guarded by one SRW lock (no constructor or
// destructor, usable from the first call to the end of the process).

namespace CEVPlayer
{
	namespace Contract
	{
		namespace
		{
			constexpr size_t kNameCapacity = 64; // ids are 1..63 characters

			struct SHint
			{
				uint64_t consumer = 0;
				uint32_t flags = 0;
				int32_t pixelWidth = 0;
				int32_t pixelHeight = 0;
				uint32_t acceptLayouts = 0;
				uint32_t lastUpdate = 0; // the surface's update count when this hint arrived
			};

			struct SSurface
			{
				char name[kNameCapacity] = {};
				CryTexSourceInfo info = {};
				uint32_t updates = 0;     // the source's own updates (PublishSnapshot calls)
				std::vector<SHint> hints;
				bool ownedByOpen = false; // made by `open` (M7): `close` destroys it
				bool closing = false;     // `close` queued: query returns 0, a new `open` starts over
				std::string openSrc;      // src of the last accepted `open`
				char decode[64] = {};     // M9: decoder in use (cev_surfaces only)
			};

			SRWLOCK s_lock = SRWLOCK_INIT;
			volatile LONG s_closed = 1;           // until Open(): nothing is ours
			std::vector<SSurface>* s_pSurfaces = nullptr; // allocated once, never freed (callable until process exit)
			std::vector<SCommand>* s_pCommands = nullptr; // queue of open / close / control, same lock

			struct SExclusive
			{
				SExclusive() { ::AcquireSRWLockExclusive(&s_lock); }
				~SExclusive() { ::ReleaseSRWLockExclusive(&s_lock); }
			};

			bool Closed() { return ::InterlockedCompareExchange(&s_closed, 0, 0) != 0; }

			int CompareNoCase(const char* a, const char* b)
			{
				for (;; ++a, ++b)
				{
					unsigned char ca = static_cast<unsigned char>(*a);
					unsigned char cb = static_cast<unsigned char>(*b);
					if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
					if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
					if (ca != cb || ca == 0)
						return static_cast<int>(ca) - static_cast<int>(cb);
				}
			}

			//! Caller holds the lock.
			SSurface* FindLocked(const char* name)
			{
				if (s_pSurfaces == nullptr || name == nullptr || name[0] == '\0')
					return nullptr;
				for (SSurface& s : *s_pSurfaces)
				{
					if (CompareNoCase(s.name, name) == 0)
						return &s;
				}
				return nullptr;
			}

			SHintSummary SummarizeLocked(const SSurface& s)
			{
				SHintSummary sum;
				sum.acceptLayouts = s.hints.empty() ? 1u : 0xFFFFFFFFu; // no consumer: RGBA8 only
				for (const SHint& h : s.hints)
				{
					++sum.consumers;
					if ((h.flags & 1u) != 0)
						sum.visible = true;
					if (h.pixelWidth > sum.pixelWidth) sum.pixelWidth = h.pixelWidth;
					if (h.pixelHeight > sum.pixelHeight) sum.pixelHeight = h.pixelHeight;
					// consumer 0 (anonymous v1) and acceptLayouts 0 mean RGBA8 only
					const uint32_t accept = h.consumer == 0 || h.acceptLayouts == 0 ? 1u : h.acceptLayouts;
					sum.acceptLayouts &= accept;
				}
				return sum;
			}

			// ---------------------------------------------------------------------------------- contract functions
			int Query(const char* name, CryTexSourceInfo* out)
			{
				if (out == nullptr || Closed())
					return 0;
				CryTexSourceInfo copy;
				{
					SExclusive lock;
					const SSurface* const s = FindLocked(name);
					if (s == nullptr || s->closing)
						return 0;
					copy = s->info;
				}
				// Only the fields inside the caller's size are written; `size` itself is the caller's.
				const size_t limit = out->size < sizeof(CryTexSourceInfo) ? out->size : sizeof(CryTexSourceInfo);
				constexpr size_t first = offsetof(CryTexSourceInfo, textureId);
				if (limit > first)
					std::memcpy(reinterpret_cast<char*>(out) + first, reinterpret_cast<const char*>(&copy) + first, limit - first);
				return 1;
			}

			void Hint(const char* name, const CryTexSourceHint* hint)
			{
				if (hint == nullptr || Closed())
					return;
				// Read only the fields inside the caller's size, the rest stays zero.
				CryTexSourceHint h = {};
				const size_t limit = hint->size < sizeof(CryTexSourceHint) ? hint->size : sizeof(CryTexSourceHint);
				if (limit > sizeof(uint32_t))
					std::memcpy(reinterpret_cast<char*>(&h) + sizeof(uint32_t), reinterpret_cast<const char*>(hint) + sizeof(uint32_t), limit - sizeof(uint32_t));

				SExclusive lock;
				SSurface* const s = FindLocked(name);
				if (s == nullptr)
					return; // not ours
				for (size_t i = 0; i < s->hints.size(); ++i)
				{
					if (s->hints[i].consumer != h.consumer)
						continue;
					if ((h.flags & 2u) != 0)
					{
						s->hints.erase(s->hints.begin() + static_cast<std::ptrdiff_t>(i)); // release
						return;
					}
					SHint& e = s->hints[i];
					e.flags = h.flags;
					e.pixelWidth = h.pixelWidth;
					e.pixelHeight = h.pixelHeight;
					e.acceptLayouts = h.acceptLayouts;
					e.lastUpdate = s->updates;
					return;
				}
				if ((h.flags & 2u) != 0)
					return; // release of a consumer we do not know
				SHint e;
				e.consumer = h.consumer;
				e.flags = h.flags;
				e.pixelWidth = h.pixelWidth;
				e.pixelHeight = h.pixelHeight;
				e.acceptLayouts = h.acceptLayouts;
				e.lastUpdate = s->updates;
				s->hints.push_back(e);
			}

			bool EqualsNoCase(const std::string& a, const char* b) { return CompareNoCase(a.c_str(), b) == 0; }

			//! The module is built with fast floating point (std::isfinite may fold to true): test the exponent bits.
			bool IsFinite(double v)
			{
				uint64_t bits = 0;
				std::memcpy(&bits, &v, sizeof(bits));
				return ((bits >> 52) & 0x7FFu) != 0x7FFu;
			}
			bool IsFinite(float v) { return IsFinite(static_cast<double>(v)); }

			int OpenSurface(const char* name, const char* src, const CryTexSourceOpen* options)
			{
				if (Closed() || name == nullptr || src == nullptr || !CPlayerManager::IsValidSurfaceId(name) || !IsOpenableSource(src))
					return 0;
				// Only the fields inside the caller's size are read; the rest keeps the defaults (no flags, volume 1).
				CryTexSourceOpen o = {};
				o.volume = 1.f;
				if (options != nullptr)
				{
					const size_t limit = options->size < sizeof(CryTexSourceOpen) ? options->size : sizeof(CryTexSourceOpen);
					if (limit > sizeof(uint32_t))
						std::memcpy(reinterpret_cast<char*>(&o) + sizeof(uint32_t), reinterpret_cast<const char*>(options) + sizeof(uint32_t), limit - sizeof(uint32_t));
				}
				SExclusive lock;
				if (s_pSurfaces == nullptr || s_pCommands == nullptr)
					return 0;
				SSurface* s = FindLocked(name);
				if (s != nullptr && !s->closing && !s->ownedByOpen)
					return 2; // level logic made it: attach by query, do not close
				if (s != nullptr && !s->closing && EqualsNoCase(s->openSrc, src))
					return 1; // already open with this src: nothing to do
				if (s == nullptr)
				{
					s_pSurfaces->push_back(SSurface());
					s = &s_pSurfaces->back();
					std::memcpy(s->name, name, std::strlen(name) + 1);
					s->info.size = sizeof(CryTexSourceInfo);
					s->info.layout = CTS_LAYOUT_RGBA8;
					s->info.flags = 1u;
				}
				s->ownedByOpen = true;
				s->closing = false;
				s->openSrc = src;
				s->info.state = 1; // loading until the player's own snapshot replaces it
				s->info.errorCode = 0;
				SCommand c;
				c.type = SCommand::EType::Open;
				c.name = name;
				c.src = src;
				c.openFlags = o.flags;
				c.volume = IsFinite(o.volume) ? (o.volume < 0.f ? 0.f : (o.volume > 1.f ? 1.f : o.volume)) : 1.f;
				s_pCommands->push_back(c);
				return 1;
			}

			void CloseSurface(const char* name)
			{
				if (Closed())
					return;
				SExclusive lock;
				SSurface* const s = FindLocked(name);
				if (s == nullptr || s->closing || !s->ownedByOpen || s_pCommands == nullptr)
					return; // only what `open` made
				s->closing = true;
				s->openSrc.clear();
				SCommand c;
				c.type = SCommand::EType::Close;
				c.name = s->name;
				s_pCommands->push_back(c);
			}

			int Control(const char* name, uint32_t verb, double value)
			{
				if (Closed())
					return 0;
				switch (verb)
				{
				case CTS_VERB_PLAY:
				case CTS_VERB_PAUSE:
					break;
				case CTS_VERB_SEEK:
					if (!IsFinite(value) || value < 0.0)
						return 0;
					break;
				case CTS_VERB_RATE:
					if (!IsFinite(value) || value < cev::PlayerCore::kMinRate || value > cev::PlayerCore::kMaxRate)
						return 0;
					break;
				case CTS_VERB_VOLUME:
					if (!IsFinite(value) || value < 0.0 || value > 1.0)
						return 0;
					break;
				case CTS_VERB_LOOP:
				case CTS_VERB_MUTE:
					if (value != 0.0 && value != 1.0)
						return 0;
					break;
				default:
					return 0;
				}
				SExclusive lock;
				SSurface* const s = FindLocked(name);
				if (s == nullptr || s->closing || s_pCommands == nullptr)
					return 0;
				SCommand c;
				c.type = SCommand::EType::Control;
				c.name = s->name;
				c.verb = verb;
				c.value = value;
				s_pCommands->push_back(c);
				return 1;
			}

			const CryTexSourceApi s_api = { 2u, &Query, &Hint, &OpenSurface, &CloseSurface, &Control };
		}

		void Open()
		{
			SExclusive lock;
			if (s_pSurfaces == nullptr)
				s_pSurfaces = new std::vector<SSurface>();
			if (s_pCommands == nullptr)
				s_pCommands = new std::vector<SCommand>();
			s_pSurfaces->clear();
			s_pCommands->clear();
			::InterlockedExchange(&s_closed, 0);
		}

		void Close()
		{
			SExclusive lock;
			::InterlockedExchange(&s_closed, 1);
			if (s_pSurfaces != nullptr)
				s_pSurfaces->clear();
			if (s_pCommands != nullptr)
				s_pCommands->clear();
		}

		void TakeCommands(std::vector<SCommand>& out)
		{
			out.clear();
			SExclusive lock;
			if (s_pCommands != nullptr)
				out.swap(*s_pCommands);
		}

		bool HasPendingCommands()
		{
			SExclusive lock;
			return s_pCommands != nullptr && !s_pCommands->empty();
		}

		void SetOwnedByOpen(const char* name, bool owned, const char* src)
		{
			SExclusive lock;
			SSurface* const s = FindLocked(name);
			if (s == nullptr)
				return;
			s->ownedByOpen = owned;
			if (!s->closing)
				s->openSrc = owned && src != nullptr ? src : "";
		}

		void PublishOpenError(const char* name, int32_t errorCode)
		{
			SExclusive lock;
			SSurface* const s = FindLocked(name);
			if (s == nullptr)
				return;
			s->info.state = 6;
			s->info.errorCode = errorCode;
		}

		bool IsOpenableSource(const char* src)
		{
			if (src == nullptr)
				return false;
			const size_t n = std::strlen(src);
			const auto endsWith = [&](const char* ext)
			{
				const size_t m = std::strlen(ext);
				return n > m && CompareNoCase(src + n - m, ext) == 0;
			};
			return endsWith(".webm") || endsWith(".cevvideo");
		}

		void RegisterSurface(const char* name)
		{
			if (name == nullptr || name[0] == '\0' || std::strlen(name) >= kNameCapacity)
				return;
			SExclusive lock;
			if (s_pSurfaces == nullptr || FindLocked(name) != nullptr)
				return;
			SSurface s;
			std::memcpy(s.name, name, std::strlen(name) + 1);
			s.info.size = sizeof(CryTexSourceInfo);
			s.info.layout = CTS_LAYOUT_RGBA8;
			s.info.flags = 1u; // premultiplied RGBA8
			s_pSurfaces->push_back(s);
		}

		void UnregisterSurface(const char* name)
		{
			SExclusive lock;
			if (s_pSurfaces == nullptr)
				return;
			for (size_t i = 0; i < s_pSurfaces->size(); ++i)
			{
				if (CompareNoCase((*s_pSurfaces)[i].name, name) == 0)
				{
					s_pSurfaces->erase(s_pSurfaces->begin() + static_cast<std::ptrdiff_t>(i));
					return;
				}
			}
		}

		void PublishSnapshot(const char* name, const CryTexSourceInfo& info, const char* decode)
		{
			SExclusive lock;
			SSurface* const s = FindLocked(name);
			if (s == nullptr)
				return;
			cry_strcpy(s->decode, decode != nullptr ? decode : "");
			s->info = info;
			s->info.size = sizeof(CryTexSourceInfo);
			++s->updates;
			// rule 5: a consumer that sent no hint during kHintExpiryUpdates of our own updates is gone
			for (size_t i = 0; i < s->hints.size();)
			{
				if (s->updates - s->hints[i].lastUpdate >= kHintExpiryUpdates)
					s->hints.erase(s->hints.begin() + static_cast<std::ptrdiff_t>(i));
				else
					++i;
			}
		}

		bool GetHintSummary(const char* name, SHintSummary& out)
		{
			SExclusive lock;
			const SSurface* const s = FindLocked(name);
			if (s == nullptr)
				return false;
			out = SummarizeLocked(*s);
			return true;
		}

		uint32_t SurfaceCount()
		{
			SExclusive lock;
			return s_pSurfaces != nullptr ? static_cast<uint32_t>(s_pSurfaces->size()) : 0u;
		}

		uint32_t ApiVersion()
		{
			return s_api.version;
		}

		void LogSurfaces()
		{
			std::vector<SSurface> copy;
			{
				SExclusive lock;
				if (s_pSurfaces != nullptr)
					copy = *s_pSurfaces;
			}
			LogAlways("cev_surfaces: %u surface(s)%s", static_cast<unsigned>(copy.size()), Closed() ? " (contract closed)" : "");
			for (const SSurface& s : copy)
			{
				const CryTexSourceInfo& i = s.info;
				const SHintSummary h = SummarizeLocked(s);
				LogAlways("  <%s> tex=%d %dx%d flags=0x%x serial=%llu gen=%u layout=%u state=%u err=%d t=%.2f/%.2f loops=%u updates=%u owner=%s%s decode=%s | hints: consumers=%u visible=%d px=%dx%d accept=0x%x",
					s.name, i.textureId, i.width, i.height, i.flags, static_cast<unsigned long long>(i.contentSerial), i.generation, i.layout, i.state,
					i.errorCode, i.timeSeconds, i.durationSeconds, i.loopCount, s.updates, s.ownedByOpen ? "open" : "level", s.closing ? " (closing)" : "",
					s.decode[0] != 0 ? s.decode : "-",
					h.consumers, h.visible ? 1 : 0, h.pixelWidth, h.pixelHeight, h.acceptLayouts);
				if (i.layout != CTS_LAYOUT_RGBA8)
					LogAlways("     planes Y(%d,%d %dx%d) U(%d,%d %dx%d) V(%d,%d %dx%d) A(%d,%d %dx%d) matrix=%u range=%u siting=%u",
						i.planeY.x, i.planeY.y, i.planeY.w, i.planeY.h, i.planeU.x, i.planeU.y, i.planeU.w, i.planeU.h,
						i.planeV.x, i.planeV.y, i.planeV.w, i.planeV.h, i.planeA.x, i.planeA.y, i.planeA.w, i.planeA.h, i.colorMatrix, i.colorRange, i.chromaSiting);
			}
		}
	}
}

extern "C" __declspec(dllexport) const CryTexSourceApi* CryTexSource_v1(void)
{
	return &CEVPlayer::Contract::s_api;
}
