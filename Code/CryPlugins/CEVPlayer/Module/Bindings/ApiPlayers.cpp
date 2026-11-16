#include "StdAfx.h"
#include "PluginEnv.h"
#include "ApiPlayers.h"

#include <algorithm>

#include "Player/PlayerManager.h"

namespace CEVPlayer
{
	namespace Api
	{
		// ------------------------------------------------------------------------------------------------ records
		void CApiPlayers::SRecord::OnReady(IVideoPlayer&)   { pOwner->Dispatch(*this, eEvent_Ready, nullptr, 0); }
		void CApiPlayers::SRecord::OnStarted(IVideoPlayer&) { pOwner->Dispatch(*this, eEvent_Started, nullptr, 0); }
		void CApiPlayers::SRecord::OnLooped(IVideoPlayer&)  { pOwner->Dispatch(*this, eEvent_Looped, nullptr, 0); }
		void CApiPlayers::SRecord::OnEnded(IVideoPlayer&)   { pOwner->Dispatch(*this, eEvent_Ended, nullptr, 0); }

		void CApiPlayers::SRecord::OnNewFrame(IVideoPlayer&, uint64_t frameSerial)
		{
			const SValue args[1] = { SValue::FromInt(static_cast<int>(frameSerial & 0x7fffffff)) };
			pOwner->Dispatch(*this, eEvent_NewFrame, args, 1);
		}

		void CApiPlayers::SRecord::OnError(IVideoPlayer&, int errorCode, const char* message)
		{
			const SValue args[2] = { SValue::FromInt(errorCode), SValue::FromString(message) };
			pOwner->Dispatch(*this, eEvent_Error, args, 2);
		}

		void CApiPlayers::SRecord::OnMarker(IVideoPlayer&, const char* name, double time)
		{
			const SValue args[2] = { SValue::FromString(name), SValue::FromFloat(static_cast<float>(time)) };
			pOwner->Dispatch(*this, eEvent_Marker, args, 2);
		}

		// ------------------------------------------------------------------------------------------------ registry
		CApiPlayers::CApiPlayers(CPlayerManager& manager)
			: m_manager(manager)
		{
		}

		CApiPlayers::~CApiPlayers()
		{
			Shutdown();
		}

		int CApiPlayers::Create(const char* textureName, bool componentOwned)
		{
			if (m_shutdown)
				return 0;
			IVideoPlayer* const pPlayer = m_manager.CreatePlayer(textureName);
			if (pPlayer == nullptr)
				return 0;
			std::unique_ptr<SRecord> pRecord = std::make_unique<SRecord>();
			pRecord->pOwner = this;
			pRecord->handle = m_nextHandle++;
			pRecord->pPlayer = pPlayer;
			pRecord->componentOwned = componentOwned;
			pPlayer->AddListener(pRecord.get());
			const int handle = pRecord->handle;
			m_records[handle] = std::move(pRecord);
			LogDebug("api: player handle %d created (%s)", handle, pPlayer->GetTextureName());
			return handle;
		}

		CApiPlayers::SRecord* CApiPlayers::FindRecord(int handle, bool warn)
		{
			const std::map<int, std::unique_ptr<SRecord>>::iterator it = m_records.find(handle);
			if (it == m_records.end())
			{
				if (warn)
					LogWarning("api: invalid player handle %d (never created or already destroyed), call ignored", handle);
				return nullptr;
			}
			if (!m_manager.IsPlayer(it->second->pPlayer))
			{
				// destroyed behind our back (cannot happen through the handle API): the player is gone with its listeners
				std::unique_ptr<SRecord> pDead = std::move(it->second);
				m_records.erase(it);
				pDead->sinks.clear();
				Retire(std::move(pDead));
				UpdateInputListener();
				if (warn)
					LogWarning("api: player of handle %d no longer exists, call ignored", handle);
				return nullptr;
			}
			return it->second.get();
		}

		IVideoPlayer* CApiPlayers::Find(int handle, bool warn)
		{
			SRecord* const pRecord = FindRecord(handle, warn);
			return pRecord != nullptr ? pRecord->pPlayer : nullptr;
		}

		void CApiPlayers::Retire(std::unique_ptr<SRecord> pRecord)
		{
			m_retired.push_back(std::move(pRecord)); // a callback of this record may still be on the stack
		}

		bool CApiPlayers::Destroy(int handle)
		{
			const std::map<int, std::unique_ptr<SRecord>>::iterator it = m_records.find(handle);
			if (it == m_records.end())
			{
				LogWarning("api: invalid player handle %d (never created or already destroyed), Destroy ignored", handle);
				return false;
			}
			std::unique_ptr<SRecord> pRecord = std::move(it->second);
			m_records.erase(it);
			if (m_manager.IsPlayer(pRecord->pPlayer))
			{
				pRecord->pPlayer->RemoveListener(pRecord.get());
				m_manager.DestroyPlayer(pRecord->pPlayer);
			}
			pRecord->sinks.clear();
			pRecord->skipPending = false;
			Retire(std::move(pRecord));
			UpdateInputListener();
			LogDebug("api: player handle %d destroyed", handle);
			return true;
		}

		uint32_t CApiPlayers::DestroyScriptOwned()
		{
			if (m_shutdown)
				return 0;
			std::vector<int> handles;
			for (const std::pair<const int, std::unique_ptr<SRecord>>& entry : m_records)
			{
				if (!entry.second->componentOwned)
					handles.push_back(entry.first);
			}
			uint32_t count = 0;
			for (const int handle : handles)
			{
				if (Destroy(handle))
					++count;
			}
			if (count != 0)
				Log("api: %u script owned player(s) destroyed (game mode ended / level unloaded)", count);
			return count;
		}

		// ------------------------------------------------------------------------------------------------ skippable
		bool CApiPlayers::AnySkippable() const
		{
			for (const std::pair<const int, std::unique_ptr<SRecord>>& entry : m_records)
			{
				if (entry.second->skippable)
					return true;
			}
			return false;
		}

		void CApiPlayers::UpdateInputListener()
		{
			const bool want = !m_shutdown && AnySkippable() && gEnv != nullptr && gEnv->pInput != nullptr;
			if (want == m_inputRegistered)
				return;
			if (want)
				gEnv->pInput->AddEventListener(this);
			else if (gEnv != nullptr && gEnv->pInput != nullptr)
				gEnv->pInput->RemoveEventListener(this);
			m_inputRegistered = want;
			LogDebug("api: skip input listener %s", want ? "registered" : "removed");
		}

		void CApiPlayers::SetSkippable(int handle, bool skippable)
		{
			SRecord* const pRecord = FindRecord(handle, true);
			if (pRecord == nullptr)
				return;
			pRecord->skippable = skippable;
			UpdateInputListener();
		}

		bool CApiPlayers::IsSkippable(int handle) const
		{
			const std::map<int, std::unique_ptr<SRecord>>::const_iterator it = m_records.find(handle);
			return it != m_records.end() && it->second->skippable;
		}

		bool CApiPlayers::OnInputEvent(const SInputEvent& event)
		{
			if (event.state != eIS_Pressed)
				return false;
			switch (event.deviceType)
			{
			case eIDT_Keyboard:
			case eIDT_Mouse:
			case eIDT_Gamepad:
			case eIDT_Joystick:
				break;
			default:
				return false;
			}
			if (event.keyId == eKI_Tilde) // the console key is not a skip request
				return false;

			for (std::pair<const int, std::unique_ptr<SRecord>>& entry : m_records)
			{
				SRecord& record = *entry.second;
				if (!record.skippable || record.skipPending || !m_manager.IsPlayer(record.pPlayer))
					continue;
				if (record.pPlayer->GetState() != EVideoPlayerState::Playing)
					continue;
				LogDebug("api: player handle %d skipped by input <%s>", record.handle, event.keyName.c_str());
				record.pPlayer->Stop();
				record.skipPending = true; // the Skipped event goes out at the next Update()
			}
			return false; // never swallows input
		}

		// ------------------------------------------------------------------------------------------------ events
		void CApiPlayers::Subscribe(int handle, IEventSink* pSink)
		{
			SRecord* const pRecord = FindRecord(handle, true);
			if (pRecord == nullptr || pSink == nullptr)
				return;
			if (std::find(pRecord->sinks.begin(), pRecord->sinks.end(), pSink) == pRecord->sinks.end())
				pRecord->sinks.push_back(pSink);
		}

		void CApiPlayers::Unsubscribe(int handle, IEventSink* pSink)
		{
			SRecord* const pRecord = FindRecord(handle, false);
			if (pRecord == nullptr)
				return;
			pRecord->sinks.erase(std::remove(pRecord->sinks.begin(), pRecord->sinks.end(), pSink), pRecord->sinks.end());
		}

		void CApiPlayers::UnsubscribeAll(IEventSink* pSink)
		{
			for (std::pair<const int, std::unique_ptr<SRecord>>& entry : m_records)
			{
				std::vector<IEventSink*>& sinks = entry.second->sinks;
				sinks.erase(std::remove(sinks.begin(), sinks.end(), pSink), sinks.end());
			}
		}

		void CApiPlayers::Dispatch(SRecord& record, uint32_t event, const SValue* args, uint32_t argCount)
		{
			if (m_shutdown || record.sinks.empty())
				return;
			const std::vector<IEventSink*> snapshot = record.sinks;
			for (IEventSink* const pSink : snapshot)
			{
				// a sink that unsubscribed (or whose player was destroyed) from inside an earlier callback is skipped
				if (std::find(record.sinks.begin(), record.sinks.end(), pSink) == record.sinks.end())
					continue;
				pSink->OnApiEvent(record.handle, event, args, argCount);
			}
		}

		void CApiPlayers::Update()
		{
			if (m_shutdown)
				return;
			m_retired.clear(); // no callback is running at the plug-in's update point

			std::vector<int> skipped;
			for (const std::pair<const int, std::unique_ptr<SRecord>>& entry : m_records)
			{
				if (entry.second->skipPending)
					skipped.push_back(entry.first);
			}
			for (const int handle : skipped)
			{
				const std::map<int, std::unique_ptr<SRecord>>::iterator it = m_records.find(handle);
				if (it == m_records.end())
					continue;
				it->second->skipPending = false;
				Dispatch(*it->second, eEvent_Skipped, nullptr, 0);
			}
		}

		void CApiPlayers::Shutdown()
		{
			if (m_shutdown)
				return;
			m_shutdown = true;
			if (m_inputRegistered)
			{
				if (gEnv != nullptr && gEnv->pInput != nullptr)
					gEnv->pInput->RemoveEventListener(this);
				m_inputRegistered = false;
			}
			for (std::pair<const int, std::unique_ptr<SRecord>>& entry : m_records)
			{
				SRecord& record = *entry.second;
				if (m_manager.IsPlayer(record.pPlayer))
					record.pPlayer->RemoveListener(&record); // the manager destroys the player itself right after
				record.sinks.clear();
			}
			m_records.clear();
			m_retired.clear();
			LogDebug("api: handle registry shut down");
		}
	}
}
