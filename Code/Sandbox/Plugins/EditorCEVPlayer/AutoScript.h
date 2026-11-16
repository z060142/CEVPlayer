// Automation for tests and smoke runs: a text file of console commands that runs from the editor's idle notification
// (the editor never runs the console's deferred command queue, so `wait_seconds` / `exec` of a normal cfg file do not work
// in Sandbox; docs/research/03-smoke-test.md §2.3). One command per line, `#` starts a comment, `wait_seconds N` waits.
//
// Started by the console command `cev_editor_script <file>` or, without any command, by the environment variable
// CEV_EDITOR_SCRIPT=<file> (read once after the main frame exists; CEV_EDITOR_SCRIPT_DELAY = seconds to wait first, default 8).
#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <IEditor.h>

namespace CEVEditor
{
	class CAutoScript : public IAutoEditorNotifyListener
	{
	public:
		CAutoScript() = default;
		//! Reads `file` (absolute or relative to the current directory / the project). False + log when unreadable.
		bool Start(const std::string& file);
		bool IsRunning() const { return m_running; }

		void OnEditorNotifyEvent(EEditorNotifyEvent event) override;

	private:
		using Clock = std::chrono::steady_clock;
		void Step();

		std::vector<std::string> m_lines;
		size_t m_next = 0;
		bool m_running = false;
		bool m_envChecked = false;
		bool m_mainFrameUp = false;
		Clock::time_point m_mainFrameTime;
		Clock::time_point m_waitUntil;
	};
}
