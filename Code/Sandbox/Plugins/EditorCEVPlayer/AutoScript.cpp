#include "StdAfx.h"
#include "AutoScript.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <CrySystem/IConsole.h>

namespace CEVEditor
{
	namespace
	{
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
	}

	bool CAutoScript::Start(const std::string& file)
	{
		std::ifstream in(std::filesystem::u8path(file));
		if (!in)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] script: cannot open '%s'", file.c_str());
			return false;
		}
		m_lines.clear();
		std::string line;
		while (std::getline(in, line))
		{
			line = Trim(line);
			if (!line.empty() && line[0] != '#')
				m_lines.push_back(line);
		}
		m_next = 0;
		m_waitUntil = Clock::now();
		m_running = true;
		m_stepping = false;
		CryLogAlways("[CEVEditor] script: %u commands from '%s'", static_cast<unsigned>(m_lines.size()), file.c_str());
		return true;
	}

	void CAutoScript::Stop()
	{
		m_running = false;
		m_lines.clear();
		m_next = 0;
	}

	void CAutoScript::OnEditorNotifyEvent(EEditorNotifyEvent event)
	{
		if (event == eNotify_OnQuit)
		{
			Stop(); // nothing may run (and no window open) while the editor shuts down
			m_mainFrameUp = false;
			return;
		}
		if (event == eNotify_OnMainFrameInitialized)
		{
			m_mainFrameUp = true;
			m_mainFrameTime = Clock::now();
			return;
		}
		if (event != eNotify_OnIdleUpdate || !m_mainFrameUp)
			return;

		if (!m_envChecked)
		{
			double delay = 8.0;
			if (const char* const szDelay = std::getenv("CEV_EDITOR_SCRIPT_DELAY"))
				delay = std::atof(szDelay);
			if (std::chrono::duration<double>(Clock::now() - m_mainFrameTime).count() < delay)
				return;
			m_envChecked = true;
			if (const char* const szFile = std::getenv("CEV_EDITOR_SCRIPT"))
			{
				if (szFile[0] != '\0')
					Start(szFile);
			}
		}
		if (m_running)
			Step();
	}

	void CAutoScript::Step()
	{
		// A command can run a nested event loop (modal dialog): its idle notifications must not run the script on.
		if (m_stepping)
			return;
		struct SGuard
		{
			explicit SGuard(bool& flag) : m_flag(flag) { m_flag = true; }
			~SGuard() { m_flag = false; }
			bool& m_flag;
		} guard(m_stepping);

		while (m_running && Clock::now() >= m_waitUntil)
		{
			if (m_next >= m_lines.size())
			{
				m_running = false;
				CryLogAlways("[CEVEditor] script: finished");
				return;
			}
			const std::string line = m_lines[m_next++];
			if (line.compare(0, 12, "wait_seconds") == 0 && (line.size() == 12 || line[12] == ' ' || line[12] == '\t'))
			{
				double seconds = std::atof(line.c_str() + 12);
				if (!(seconds >= 0.0))
					seconds = 0.0; // negative / NaN
				seconds = std::min(seconds, 3600.0);
				m_waitUntil = Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
				return;
			}
			CryLogAlways("[CEVEditor] script: %s", line.c_str());
			if (gEnv != nullptr && gEnv->pConsole != nullptr)
				gEnv->pConsole->ExecuteString(line.c_str());
			return; // one command per idle: a command may open windows that need an event loop turn
		}
	}
}
