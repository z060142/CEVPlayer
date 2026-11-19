// libvpx decoder backend (M9): the VideoDecoder behind IVideoDecoderBackend. Decodes synchronously inside Submit() on the
// calling thread and writes the slot at once, so its behaviour equals the pre-M9 decode step.
#include <chrono>
#include <cstring>
#include <new>

#include "CEVCore/VideoDecoder.h"
#include "CEVCore/VideoDecoderBackend.h"

namespace cev
{
	namespace
	{
		using Steady = std::chrono::steady_clock;

		inline float Ms(Steady::time_point a, Steady::time_point b)
		{
			return std::chrono::duration<float, std::milli>(b - a).count();
		}

		class LibvpxBackend final : public IVideoDecoderBackend
		{
		public:
			explicit LibvpxBackend(std::unique_ptr<VideoDecoder> decoder) : m_dec(std::move(decoder)) {}

			void SetOutputFormat(const FrameFormat& format) override { m_format = format; }

			Status Submit(const DecodePacket& color, const DecodePacket* alpha, const DecodeTicket& ticket) override
			{
				if (m_hasDone)
					return Status::Busy; // the caller receives the previous picture first
				const bool withAlpha = alpha != nullptr && alpha->data != nullptr && alpha->size > 0;
				const Steady::time_point t0 = Steady::now();
				const Status st = m_dec->Decode(color.data, color.size, withAlpha ? alpha->data : nullptr, withAlpha ? alpha->size : 0);
				const Steady::time_point t1 = Steady::now();
				if (Failed(st))
					return st;
				if (m_dec->HasFrame())
					m_lastValid = Succeeded(m_dec->GetFrameInfo(m_last));
				if (ticket.slotIndex < 0 || ticket.slot == nullptr)
					return Status::Ok; // skip decode: no picture
				m_done = ticket;
				m_done.decodeMs = Ms(t0, t1);
				m_done.copyMs = 0.0f;
				m_done.readbackMs = 0.0f;
				if (!m_dec->HasFrame())
					m_doneResult = Status::Cancelled;
				else
				{
					m_doneResult = Succeeded(m_dec->WriteFrame(m_format, *ticket.slot)) ? Status::Ok : Status::InvalidData;
					m_done.copyMs = Ms(t1, Steady::now());
				}
				m_hasDone = true;
				return Status::Ok;
			}

			bool Receive(DecodeTicket& out, Status& result) override
			{
				if (!m_hasDone)
					return false;
				out = m_done;
				result = m_doneResult;
				m_hasDone = false;
				return true;
			}

			uint32_t Flush(int* abandoned, uint32_t capacity) override
			{
				uint32_t n = 0;
				if (m_hasDone && capacity > 0 && abandoned != nullptr)
					abandoned[n++] = m_done.slotIndex;
				m_hasDone = false;
				return n;
			}

			void Drain() override {}
			uint32_t MaxInFlight() const override { return 1; }

			DecodeBackendInfo Info() const override
			{
				DecodeBackendInfo info;
				info.kind = DecodeBackendKind::Libvpx;
				return info;
			}

			bool LastFrameInfo(DecodedImageInfo& out) const override
			{
				if (!m_lastValid)
					return false;
				out = m_last;
				return true;
			}

			// ---- load helpers (LibvpxDecodeProbe / LibvpxWriteLastFrame)
			Status Probe(const DecodePacket& color, const DecodePacket* alpha, bool& gotPicture)
			{
				gotPicture = false;
				DecodeTicket skip;
				const Status st = Submit(color, alpha, skip);
				gotPicture = Succeeded(st) && m_dec->HasFrame();
				return st;
			}

			Status WriteLast(const FrameFormat& format, FrameRing::Slot& slot) const { return m_dec->WriteFrame(format, slot); }

		private:
			std::unique_ptr<VideoDecoder> m_dec;
			FrameFormat m_format;
			DecodeTicket m_done;
			Status m_doneResult = Status::Ok;
			bool m_hasDone = false;
			DecodedImageInfo m_last;
			bool m_lastValid = false;
		};
	}

	Status CreateLibvpxBackend(const VideoStreamInfo& stream, uint32_t threads, std::unique_ptr<IVideoDecoderBackend>& out)
	{
		out.reset();
		std::unique_ptr<VideoDecoder> dec;
		const Status st = VideoDecoder::Create(stream, threads, dec);
		if (Failed(st))
			return st;
		LibvpxBackend* b = new (std::nothrow) LibvpxBackend(std::move(dec));
		if (b == nullptr)
			return Status::OutOfMemory;
		out.reset(b);
		return Status::Ok;
	}

	Status LibvpxDecodeProbe(IVideoDecoderBackend& backend, const DecodePacket& color, const DecodePacket* alpha, bool& gotPicture)
	{
		gotPicture = false;
		if (backend.Info().kind != DecodeBackendKind::Libvpx)
			return Status::InvalidArgument;
		return static_cast<LibvpxBackend&>(backend).Probe(color, alpha, gotPicture);
	}

	Status LibvpxWriteLastFrame(IVideoDecoderBackend& backend, const FrameFormat& format, FrameRing::Slot& slot)
	{
		if (backend.Info().kind != DecodeBackendKind::Libvpx)
			return Status::InvalidArgument;
		return static_cast<const LibvpxBackend&>(backend).WriteLast(format, slot);
	}
}
