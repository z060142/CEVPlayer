#include "StdAfx.h"
#include "Editor/PreviewPanel.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <CrySystem/ISystem.h>

#include "Editor/VideoPreviewWidget.h"

namespace CEVEditor
{
	namespace
	{
		constexpr int kTickMs = 16;            // ~60 Hz: the Core presents the frame that is due
		constexpr int kMinimizedPollMs = 250;  // main window minimized: only watch for the restore
		std::vector<const CVideoPreviewPanel*> g_timerPanels;

		void NoteTimer(const CVideoPreviewPanel* pPanel, bool running)
		{
			const auto it = std::find(g_timerPanels.begin(), g_timerPanels.end(), pPanel);
			if (running && it == g_timerPanels.end())
				g_timerPanels.push_back(pPanel);
			else if (!running && it != g_timerPanels.end())
				g_timerPanels.erase(it);
		}
	}

	int CVideoPreviewPanel::ActiveTimers()
	{
		return static_cast<int>(g_timerPanels.size());
	}

	CVideoPreviewPanel::CVideoPreviewPanel(QWidget* pParent)
		: QWidget(pParent)
	{
		setWindowTitle(tr("Preview"));
		m_timer.setTimerType(Qt::PreciseTimer);
		m_timer.setInterval(kTickMs);
		connect(&m_timer, &QTimer::timeout, this, &CVideoPreviewPanel::OnTimer);

		QVBoxLayout* const pMain = new QVBoxLayout(this);
		pMain->setContentsMargins(4, 4, 4, 4);

		m_pPicture = new CVideoPreviewWidget(this);
		m_pPicture->SetSession(&m_session);
		pMain->addWidget(m_pPicture, 1);

		m_pStatus = new QLabel(this);
		m_pStatus->setWordWrap(true);
		pMain->addWidget(m_pStatus);

		m_pSlider = new QSlider(Qt::Horizontal, this);
		m_pSlider->setRange(0, 10000);
		m_pSlider->setEnabled(false);
		pMain->addWidget(m_pSlider);

		QHBoxLayout* const pBar = new QHBoxLayout();
		m_pStepBack = new QToolButton(this);
		m_pStepBack->setText(tr("|<"));
		m_pStepBack->setToolTip(tr("Step back one frame"));
		m_pPlay = new QToolButton(this);
		m_pPlay->setText(tr("Play"));
		m_pStop = new QToolButton(this);
		m_pStop->setText(tr("Stop"));
		m_pStepForward = new QToolButton(this);
		m_pStepForward->setText(tr(">|"));
		m_pStepForward->setToolTip(tr("Step forward one frame"));
		m_pLoop = new QCheckBox(tr("Loop"), this);
		m_pTime = new QLabel(QStringLiteral("00:00:00:00"), this);
		m_pTime->setMinimumWidth(110);
		m_pAudio = new QComboBox(this);
		m_pAudio->addItem(tr("Audio: as authored"), static_cast<int>(EPreviewAudio::AsAuthored));
		m_pAudio->addItem(tr("Audio: embedded track"), static_cast<int>(EPreviewAudio::EmbeddedTrack));
		m_pAudio->addItem(tr("Audio: muted"), static_cast<int>(EPreviewAudio::Muted));
		m_pTrack = new QComboBox(this);
		m_pTrack->setToolTip(tr("Audio track to audition (switches in place while playing). The game language is not followed in the preview."));
		m_pTrack->addItem(tr("Track: automatic"), QString());
		m_pTrack->setEnabled(false);
		m_pVolume = new QSlider(Qt::Horizontal, this);
		m_pVolume->setRange(0, 100);
		m_pVolume->setValue(100);
		m_pVolume->setMaximumWidth(80);
		m_pVolume->setToolTip(tr("Preview volume"));
		m_pFit = new QComboBox(this);
		m_pFit->addItem(tr("Fit"));
		m_pFit->addItem(tr("100%"));
		m_pBackground = new QComboBox(this);
		m_pBackground->addItem(tr("Checker"), static_cast<int>(CVideoPreviewWidget::EBackground::Checker));
		m_pBackground->addItem(tr("Black"), static_cast<int>(CVideoPreviewWidget::EBackground::Black));
		m_pBackground->addItem(tr("White"), static_cast<int>(CVideoPreviewWidget::EBackground::White));
		m_pBackground->setToolTip(tr("Background behind transparent pixels"));
		pBar->addWidget(m_pStepBack);
		pBar->addWidget(m_pPlay);
		pBar->addWidget(m_pStop);
		pBar->addWidget(m_pStepForward);
		pBar->addWidget(m_pLoop);
		pBar->addWidget(m_pTime);
		pBar->addStretch(1);
		pBar->addWidget(m_pAudio);
		pBar->addWidget(m_pTrack);
		pBar->addWidget(m_pVolume);
		pBar->addWidget(m_pFit);
		pBar->addWidget(m_pBackground);
		pMain->addLayout(pBar);

		connect(m_pPlay, &QToolButton::clicked, this, &CVideoPreviewPanel::OnPlayPause);
		connect(m_pStop, &QToolButton::clicked, this, &CVideoPreviewPanel::OnStop);
		connect(m_pStepBack, &QToolButton::clicked, this, [this]() { OnStep(-1); });
		connect(m_pStepForward, &QToolButton::clicked, this, [this]() { OnStep(+1); });
		connect(m_pSlider, &QSlider::valueChanged, this, &CVideoPreviewPanel::OnSliderMoved);
		connect(m_pLoop, &QCheckBox::toggled, this, &CVideoPreviewPanel::OnLoopToggled);
		connect(m_pAudio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CVideoPreviewPanel::OnAudioModeChanged);
		connect(m_pTrack, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CVideoPreviewPanel::OnAudioTrackChanged);
		connect(m_pVolume, &QSlider::valueChanged, this, &CVideoPreviewPanel::OnVolumeChanged);
		connect(m_pFit, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) { m_pPicture->SetFit(index == 0); });
		connect(m_pBackground, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int)
		{
			m_pPicture->SetBackground(static_cast<CVideoPreviewWidget::EBackground>(m_pBackground->currentData().toInt()));
		});

		m_inGameMode = GetIEditor()->IsInGameMode();
		SetStatus(tr("No video loaded."));
		GetIEditor()->RegisterNotifyListener(this);
		UpdateTransport();
	}

	CVideoPreviewPanel::~CVideoPreviewPanel()
	{
		GetIEditor()->UnregisterNotifyListener(this);
		m_timer.stop();
		NoteTimer(this, false);
		m_pPicture->SetSession(nullptr);
		m_session.Release(); // audio device first, then the cores and their decode threads
	}

	void CVideoPreviewPanel::showEvent(QShowEvent* pEvent)
	{
		QWidget::showEvent(pEvent);
		UpdateTimer();
	}

	void CVideoPreviewPanel::hideEvent(QHideEvent* pEvent)
	{
		QWidget::hideEvent(pEvent);
		UpdateTimer();
	}

	//! The timer runs while there is media and the panel is visible; otherwise the playback is suspended.
	void CVideoPreviewPanel::UpdateTimer()
	{
		const bool visible = isVisible() && !m_shutdown;
		const bool run = visible && m_session.HasMedia();
		if (run && !m_timer.isActive())
			m_timer.start();
		else if (!run && m_timer.isActive())
			m_timer.stop();
		NoteTimer(this, m_timer.isActive());
		m_session.Suspend(!run || m_inGameMode || m_minimized);
	}

	void CVideoPreviewPanel::OnEditorNotifyEvent(EEditorNotifyEvent event)
	{
		switch (event)
		{
		case eNotify_OnBeginGameMode:
			m_inGameMode = true;
			m_session.Suspend(true);
			SetStatus(tr("Game mode is running: the preview is paused."));
			break;
		case eNotify_OnEndGameMode:
			m_inGameMode = false;
			UpdateTimer();
			if (m_session.IsReady())
				SetStatus(ReadyStatus());
			break;
		case eNotify_OnQuit:
			// the editor goes down after this: stop the sound and the decode threads now
			m_shutdown = true;
			m_timer.stop();
			NoteTimer(this, false);
			m_session.Release();
			break;
		default:
			break;
		}
	}

	void CVideoPreviewPanel::SetStatus(const QString& text)
	{
		m_pStatus->setText(text);
	}

	double CVideoPreviewPanel::Fps() const
	{
		if (m_session.IsReady() && m_session.Info().frameRate > 1.0)
			return m_session.Info().frameRate;
		return m_source.fps > 1.0 ? m_source.fps : 30.0;
	}

	QString CVideoPreviewPanel::Timecode(double seconds) const
	{
		if (seconds < 0.0)
			seconds = 0.0;
		const double fps = Fps();
		const long long total = static_cast<long long>(seconds);
		const int frames = static_cast<int>((seconds - static_cast<double>(total)) * fps);
		return QString("%1:%2:%3:%4").arg(static_cast<int>(total / 3600), 2, 10, QLatin1Char('0'))
			.arg(static_cast<int>((total / 60) % 60), 2, 10, QLatin1Char('0'))
			.arg(static_cast<int>(total % 60), 2, 10, QLatin1Char('0'))
			.arg(frames, 2, 10, QLatin1Char('0'));
	}

	QString CVideoPreviewPanel::ReadyStatus() const
	{
		const cev::MediaInfo& info = m_session.Info();
		return tr("%1 x %2, %3 s, %4%5%6")
			.arg(info.width).arg(info.height).arg(info.durationSeconds, 0, 'f', 2)
			.arg(info.hasAlpha ? tr("alpha") : tr("opaque"))
			.arg(m_session.HasAudio() ? tr(", audio") : tr(", no audio"))
			.arg(QString::fromUtf8(m_session.Notes().c_str()));
	}

	float CVideoPreviewPanel::Volume() const
	{
		const EPreviewAudio audio = static_cast<EPreviewAudio>(m_pAudio->currentData().toInt());
		return audio == EPreviewAudio::Muted ? 0.f : static_cast<float>(m_pVolume->value()) / 100.f;
	}

	void CVideoPreviewPanel::SetSource(const SSource& source)
	{
		if (source.descriptorRel != m_source.descriptorRel)
			m_trackChoice.clear(); // another asset: back to the automatic choice
		m_source = source;
		m_hasSource = !source.descriptorRel.empty();
		if (!m_hasSource)
		{
			Clear();
			return;
		}
		Reload();
	}

	void CVideoPreviewPanel::Reload()
	{
		if (!m_hasSource || m_shutdown)
			return;
		SPreviewRequest request;
		request.descriptorRel = m_source.descriptorRel;
		request.videoRel = m_source.videoRel;
		request.avOffsetMs = m_source.avOffsetMs;
		request.audio = static_cast<EPreviewAudio>(m_pAudio->currentData().toInt());
		request.audioTrack = m_trackChoice;
		request.volume = Volume();
		std::string error;
		m_lastSerial = 0;
		m_loggedFirstFrame = false;
		if (!m_session.Load(request, error))
		{
			FillTrackList();
			SetStatus(tr("Cannot preview %1: %2").arg(QString::fromUtf8(m_source.descriptorRel.c_str()), QString::fromUtf8(error.c_str())));
			UpdateTimer();
			UpdateTransport();
			m_pPicture->update();
			return;
		}
		m_session.SetLoop(m_pLoop->isChecked());
		FillTrackList();
		SetStatus(tr("Loading %1 ...").arg(QString::fromUtf8(m_source.descriptorRel.c_str())));
		UpdateTimer();
		UpdateTransport();
		m_pPicture->update();
	}

	void CVideoPreviewPanel::Clear()
	{
		m_hasSource = false;
		m_source = SSource();
		m_session.Release();
		SetStatus(tr("No video loaded."));
		UpdateTimer();
		UpdateTransport();
		m_pPicture->update();
	}

	void CVideoPreviewPanel::ReleasePlayer()
	{
		m_session.Release();
		UpdateTimer();
		UpdateTransport();
		m_pPicture->update();
	}

	void CVideoPreviewPanel::OnPlayPause()
	{
		if (!m_session.IsReady() && !m_session.IsLoading())
			return;
		switch (m_session.State())
		{
		case cev::PlayerState::Playing: m_session.Pause(); break;
		case cev::PlayerState::Paused: m_session.Resume(); break;
		default: m_session.Play(); break;
		}
		UpdateTransport();
	}

	void CVideoPreviewPanel::OnStop()
	{
		m_session.Stop();
		UpdateTransport();
	}

	void CVideoPreviewPanel::OnStep(int direction)
	{
		if (!m_session.IsReady())
			return;
		if (m_session.State() == cev::PlayerState::Playing)
			m_session.Pause();
		// pause and seek one frame period away (the seek presents the frame while paused)
		const double duration = m_session.Duration();
		double t = m_session.Time() + direction / Fps();
		t = std::max(0.0, duration > 0.0 ? std::min(t, duration) : t);
		m_session.Seek(t);
		UpdateTransport();
	}

	void CVideoPreviewPanel::OnSliderMoved(int value)
	{
		if (m_sliderUpdating || !m_session.IsReady() || m_session.Duration() <= 0.0)
			return;
		m_session.Seek(m_session.Duration() * (static_cast<double>(value) / 10000.0));
	}

	void CVideoPreviewPanel::OnLoopToggled(bool on)
	{
		m_session.SetLoop(on);
	}

	void CVideoPreviewPanel::OnAudioModeChanged(int)
	{
		Reload();
	}

	void CVideoPreviewPanel::FillTrackList()
	{
		m_trackListUpdating = true;
		m_pTrack->clear();
		m_pTrack->addItem(tr("Track: automatic"), QString());
		const bool embeddedMode = static_cast<EPreviewAudio>(m_pAudio->currentData().toInt()) == EPreviewAudio::EmbeddedTrack;
		for (const std::string& name : m_session.AudioTrackNames())
		{
			m_pTrack->addItem(tr("Track: %1").arg(QString::fromUtf8(name.c_str())), QString::fromUtf8(name.c_str()));
			if (!m_trackChoice.empty() && _stricmp(name.c_str(), m_trackChoice.c_str()) == 0)
				m_pTrack->setCurrentIndex(m_pTrack->count() - 1);
		}
		m_pTrack->setEnabled(!embeddedMode && !m_session.AudioTrackNames().empty());
		m_trackListUpdating = false;
	}

	bool CVideoPreviewPanel::SelectAudioTrack(const std::string& name)
	{
		for (int i = 1; i < m_pTrack->count(); ++i)
		{
			if (_stricmp(m_pTrack->itemData(i).toString().toUtf8().constData(), name.c_str()) == 0)
			{
				if (m_pTrack->currentIndex() == i)
					return true;
				m_pTrack->setCurrentIndex(i); // OnAudioTrackChanged
				return true;
			}
		}
		return false;
	}

	void CVideoPreviewPanel::OnAudioTrackChanged(int index)
	{
		if (m_trackListUpdating || index < 0)
			return;
		m_trackChoice = m_pTrack->itemData(index).toString().toUtf8().constData();
		// switch in place, like the runtime's ReloadInPlace: same position, same transport state
		const bool ready = m_session.IsReady();
		const double time = ready ? m_session.Time() : 0.0;
		const bool playing = ready && m_session.State() == cev::PlayerState::Playing;
		Reload();
		if (!ready || !m_session.HasMedia())
			return;
		m_session.Seek(time); // queued while loading
		if (playing)
			m_session.Play();
		UpdateTransport();
	}

	void CVideoPreviewPanel::OnVolumeChanged(int)
	{
		m_session.SetVolume(Volume());
	}

	void CVideoPreviewPanel::UpdateTransport()
	{
		const bool loaded = m_session.IsReady();
		m_pPlay->setEnabled(loaded || m_session.IsLoading());
		m_pStop->setEnabled(loaded);
		m_pStepBack->setEnabled(loaded);
		m_pStepForward->setEnabled(loaded);
		m_pSlider->setEnabled(loaded);
		const bool playing = loaded && m_session.State() == cev::PlayerState::Playing;
		const QString playText = playing ? tr("Pause") : tr("Play");
		if (m_pPlay->text() != playText)
			m_pPlay->setText(playText);
		m_pPicture->SetSmooth(!playing); // fast scaling while playing, smooth while paused
		if (!loaded)
		{
			m_sliderUpdating = true;
			m_pSlider->setValue(0);
			m_sliderUpdating = false;
			m_pTime->setText(Timecode(0.0));
			return;
		}
		const double duration = m_session.Duration();
		const double t = m_session.Time();
		const QString timeText = Timecode(t) + " / " + Timecode(duration);
		if (m_pTime->text() != timeText)
			m_pTime->setText(timeText);
		if (duration > 0.0 && !m_pSlider->isSliderDown())
		{
			m_sliderUpdating = true;
			m_pSlider->setValue(static_cast<int>(std::min(1.0, t / duration) * 10000.0));
			m_sliderUpdating = false;
		}
	}

	void CVideoPreviewPanel::OnTimer()
	{
		if (m_shutdown)
			return;
		// minimized main window: pause and poll slowly for the restore (child widgets get no reliable hide event)
		const bool minimized = window() != nullptr && window()->isMinimized();
		if (minimized != m_minimized)
		{
			m_minimized = minimized;
			m_timer.setInterval(minimized ? kMinimizedPollMs : kTickMs);
			m_session.Suspend(m_minimized || m_inGameMode);
		}
		if (m_minimized || m_inGameMode)
			return;

		const bool fresh = m_session.Tick();
		std::string error;
		if (m_session.TakeError(error))
			SetStatus(tr("Playback error: %1").arg(QString::fromUtf8(error.c_str())));
		if (m_session.TakeReady())
		{
			SetStatus(ReadyStatus());
			CryLogAlways("[CEVEditor] preview ready: %s", ReadyStatus().toUtf8().constData());
		}
		UpdateTransport();

		const uint64_t serial = m_session.FrameSerial();
		if (fresh || serial != m_lastSerial)
		{
			m_lastSerial = serial;
			m_pPicture->update(); // only for a new frame (playing, or a seek while paused)
			if (serial != 0 && !m_loggedFirstFrame)
			{
				m_loggedFirstFrame = true;
				CryLogAlways("[CEVEditor] preview shows its first frame (%dx%d, alpha %d, widget %dx%d)", m_session.Info().width, m_session.Info().height,
					m_session.Info().hasAlpha ? 1 : 0, m_pPicture->width(), m_pPicture->height());
			}
		}
	}
}
