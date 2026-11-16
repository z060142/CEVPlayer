#include "StdAfx.h"
#include "Editor/PreviewSession.h"

#include <filesystem>

#include <CrySystem/ICryPluginManager.h>
#include <CEVPlayer/IVideoPlugin.h>

#include "Asset/VideoAssetMeta.h"
#include "Asset/VideoDescriptorIO.h"

namespace CEVEditor
{
	namespace
	{
		CEVPlayer::IVideoPlugin* Plugin()
		{
			if (gEnv == nullptr || gEnv->pSystem == nullptr)
				return nullptr;
			Cry::IPluginManager* const pManager = gEnv->pSystem->GetIPluginManager();
			return pManager != nullptr ? pManager->QueryPlugin<CEVPlayer::IVideoPlugin>() : nullptr;
		}

		int g_tempCounter = 0;
	}

	CPreviewSession::CPreviewSession() = default;

	CPreviewSession::~CPreviewSession()
	{
		Release();
	}

	bool CPreviewSession::RuntimeAvailable()
	{
		CEVPlayer::IVideoPlugin* const pPlugin = Plugin();
		return pPlugin != nullptr && pPlugin->GetVideoPlayerInterfaceVersion() >= 4;
	}

	void CPreviewSession::OnError(CEVPlayer::IVideoPlayer&, int code, const char* message)
	{
		m_error = true;
		m_errorMessage = std::string(message != nullptr ? message : "error") + " (code " + std::to_string(code) + ")";
	}

	bool CPreviewSession::Load(const std::string& descriptorRel, EPreviewAudio audio, const std::string& videoRelPath, double avOffsetMs)
	{
		CEVPlayer::IVideoPlugin* const pPlugin = Plugin();
		if (pPlugin == nullptr)
			return false;
		if (m_pPlayer == nullptr)
		{
			m_pPlayer = pPlugin->CreatePlayer(nullptr); // generated texture name: reused by later players
			if (m_pPlayer == nullptr)
				return false; // the plug-in is shutting down
			m_pPlayer->AddListener(this);
		}

		std::string path = descriptorRel;
		if (audio == EPreviewAudio::EmbeddedTrack)
		{
			// route "embedded" plays the audio track inside the video, whatever the authored route says
			SDescriptor temp;
			temp.route = "embedded";
			temp.video = videoRelPath;
			temp.avOffsetMs = avOffsetMs;
			if (m_tempDescriptorAbs.empty())
			{
				m_tempDescriptorRel = ".cevpreview/preview_" + std::to_string(++g_tempCounter) + ".cevvideo";
				m_tempDescriptorAbs = AssetsAbsolute(m_tempDescriptorRel);
				std::error_code ec;
				std::filesystem::create_directories(std::filesystem::u8path(m_tempDescriptorAbs).parent_path(), ec);
			}
			std::string error;
			if (SaveDescriptor(m_tempDescriptorAbs, temp, error))
				path = m_tempDescriptorRel;
		}

		m_ready = false;
		m_error = false;
		m_pPlayer->SetLoop(false);
		const bool accepted = m_pPlayer->Load(path.c_str());
		m_pPlayer->SetVolume(audio == EPreviewAudio::Muted ? 0.f : 1.f);
		return accepted;
	}

	void CPreviewSession::Release()
	{
		if (m_pPlayer != nullptr)
		{
			m_pPlayer->RemoveListener(this);
			if (CEVPlayer::IVideoPlugin* const pPlugin = Plugin())
				pPlugin->DestroyPlayer(m_pPlayer);
			m_pPlayer = nullptr;
		}
		if (!m_tempDescriptorAbs.empty())
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(m_tempDescriptorAbs), ec);
			m_tempDescriptorAbs.clear();
			m_tempDescriptorRel.clear();
		}
		m_ready = false;
		m_error = false;
	}
}
