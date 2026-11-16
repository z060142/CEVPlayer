// Handle registry of the script API: integer handle <-> IVideoPlayer*, the "skippable" flag with its input listener
// and the per-player event fan-out (IVideoPlayerListener -> table events with SValue payloads).
// Main thread only. Players made here are owned by the plug-in's CPlayerManager like every other player.
#pragma once

#include <map>
#include <memory>
#include <vector>

#include <CryInput/IInput.h>

#include <CEVPlayer/IVideoPlayer.h>

#include "Bindings/ApiTable.h"

namespace CEVPlayer
{
	class CPlayerManager;

	namespace Api
	{
		//! Receives the table events of the players it subscribed to. Events are dispatched at the plug-in's fixed
		//! point of the frame (never from inside a call you make); it is legal to call the API, Unsubscribe or
		//! Destroy from inside OnApiEvent.
		struct IEventSink
		{
			virtual ~IEventSink() {}
			//! `args` are the payload values of Events()[event] (declared types, in order).
			virtual void OnApiEvent(int handle, uint32_t event, const SValue* args, uint32_t argCount) = 0;
		};

		class CApiPlayers final : public IInputEventListener
		{
		public:
			explicit CApiPlayers(CPlayerManager& manager);
			~CApiPlayers();
			CApiPlayers(const CApiPlayers&) = delete;
			CApiPlayers& operator=(const CApiPlayers&) = delete;

			//! Creates a player (nullptr / empty = generated texture name). Returns its handle, 0 on failure.
			int Create(const char* textureName);
			//! Destroys the player and invalidates the handle. False (with a warning) for an invalid handle.
			bool Destroy(int handle);
			//! The player behind a handle, nullptr (and a warning when `warn`) for an invalid one.
			IVideoPlayer* Find(int handle, bool warn = true);
			uint32_t Count() const { return static_cast<uint32_t>(m_records.size()); }

			//! A skippable player that is playing is stopped by any key / mouse / gamepad button press; the event
			//! Skipped follows.
			void SetSkippable(int handle, bool skippable);
			bool IsSkippable(int handle) const;

			void Subscribe(int handle, IEventSink* pSink);
			void Unsubscribe(int handle, IEventSink* pSink);
			//! Removes the sink from every player (call from the destructor of a sink).
			void UnsubscribeAll(IEventSink* pSink);

			//! Plug-in UpdateBeforeRender, after the manager dispatched its listener events: sends the Skipped
			//! events and releases records that were destroyed from inside a callback.
			void Update();
			//! Plug-in shutdown, BEFORE the manager shuts down (the players are still valid): removes the player
			//! listeners and the input listener, forgets every handle. Idempotent.
			void Shutdown();

			// IInputEventListener
			virtual bool OnInputEvent(const SInputEvent& event) override;
			virtual int GetPriority() const override { return 0; }

		private:
			struct SRecord final : public IVideoPlayerListener
			{
				CApiPlayers* pOwner = nullptr;
				int handle = 0;
				IVideoPlayer* pPlayer = nullptr;
				bool skippable = false;
				bool skipPending = false;
				std::vector<IEventSink*> sinks;

				void OnReady(IVideoPlayer& player) override;
				void OnStarted(IVideoPlayer& player) override;
				void OnNewFrame(IVideoPlayer& player, uint64_t frameSerial) override;
				void OnLooped(IVideoPlayer& player) override;
				void OnEnded(IVideoPlayer& player) override;
				void OnError(IVideoPlayer& player, int errorCode, const char* message) override;
				void OnMarker(IVideoPlayer& player, const char* name, double time) override;
			};

			SRecord* FindRecord(int handle, bool warn);
			void Dispatch(SRecord& record, uint32_t event, const SValue* args, uint32_t argCount);
			void Retire(std::unique_ptr<SRecord> pRecord);
			void UpdateInputListener();
			bool AnySkippable() const;

			CPlayerManager& m_manager;
			std::map<int, std::unique_ptr<SRecord>> m_records;
			std::vector<std::unique_ptr<SRecord>> m_retired; // destroyed from inside a callback: freed in Update()
			int m_nextHandle = 1;
			bool m_inputRegistered = false;
			bool m_shutdown = false;
		};
	}
}
