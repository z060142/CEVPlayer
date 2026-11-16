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
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <QViewport.h>
#include <CryRenderer/IRenderAuxGeom.h>

#include "Editor/VideoViewport.h"

namespace CEVEditor
{
	namespace
	{
		// ---- checkerboard texture (shared by all preview panels, created with the first, released with the last) ----
		constexpr int kCheckerSize = 256;
		constexpr int kCheckerCell = 16;
		int g_checkerUsers = 0;
		int g_checkerId = 0;
		ITexture* g_pChecker = nullptr;

		void AcquireChecker()
		{
			if (g_checkerUsers++ > 0 || gEnv == nullptr || gEnv->pRenderer == nullptr)
				return;
			std::vector<uint8> pixels(static_cast<size_t>(kCheckerSize) * kCheckerSize * 4);
			for (int y = 0; y < kCheckerSize; ++y)
			{
				for (int x = 0; x < kCheckerSize; ++x)
				{
					const uint8 v = (((x / kCheckerCell) + (y / kCheckerCell)) & 1) ? 150 : 204;
					uint8* const p = &pixels[(static_cast<size_t>(y) * kCheckerSize + x) * 4];
					p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
				}
			}
			g_checkerId = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(pixels.data(), kCheckerSize, kCheckerSize, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_POINT, 0, "$cev_editor_checker", FT_NOMIPS));
			g_pChecker = g_checkerId > 0 ? gEnv->pRenderer->EF_GetTextureByID(g_checkerId) : nullptr;
			CryLogAlways("[CEVEditor] checkerboard texture id %d", g_checkerId);
		}

		void ReleaseChecker()
		{
			if (--g_checkerUsers > 0)
				return;
			g_checkerUsers = 0;
			if (g_pChecker != nullptr && gEnv != nullptr && gEnv->pRenderer != nullptr) // the renderer owns the texture: nothing to release once it is gone
				g_pChecker->Release();
			g_pChecker = nullptr;
			g_checkerId = 0;
		}

		bool g_editorReady = false;

		struct SRectF { float x, y, w, h; };

		void DrawChecker(IRenderAuxGeom* aux, const SRectF& r)
		{
			if (g_checkerId <= 0)
				return;
			SRender2DImageDescription img;
			img.z = 1.0f;
			img.textureId = static_cast<uint32>(g_checkerId);
			img.color = ColorB(255, 255, 255, 255);
			img.renderFlags = e_Mode2D | e_DepthTestOff | e_AlphaNone | e_CullModeNone | e_FillModeSolid;
			for (float ty = 0.f; ty < r.h; ty += kCheckerSize)
			{
				for (float tx = 0.f; tx < r.w; tx += kCheckerSize)
				{
					const float tw = std::min(static_cast<float>(kCheckerSize), r.w - tx);
					const float th = std::min(static_cast<float>(kCheckerSize), r.h - ty);
					img.x = r.x + tx;
					img.y = r.y + ty;
					img.w = tw;
					img.h = th;
					// the aux quad is flipped vertically: t0 = 1 at the top (see FullscreenOutput.cpp of the runtime plug-in)
					img.uv[0] = Vec2(0.f, 1.f);
					img.uv[1] = Vec2(tw / kCheckerSize, 1.f - th / kCheckerSize);
					aux->PushImage(img);
				}
			}
		}

		void DrawVideo(IRenderAuxGeom* aux, int textureId, const SRectF& r, bool hasAlpha)
		{
			SRender2DImageDescription img;
			img.x = r.x; img.y = r.y; img.w = r.w; img.h = r.h; img.z = 1.0f;
			img.textureId = static_cast<uint32>(textureId);
			img.uv[0] = Vec2(0.f, 1.f);
			img.uv[1] = Vec2(1.f, 0.f);
			if (!hasAlpha)
			{
				img.color = ColorB(255, 255, 255, 255);
				img.renderFlags = e_Mode2D | e_DepthTestOff | e_AlphaNone | e_CullModeNone | e_FillModeSolid;
				aux->PushImage(img);
				return;
			}
			// premultiplied "over" in two passes (the result is dst * (1 - a) + rgb)
			img.color = ColorB(0, 0, 0, 255);
			img.renderFlags = e_Mode2D | e_DepthTestOff | e_AlphaBlended | e_CullModeNone | e_FillModeSolid;
			aux->PushImage(img);
			img.color = ColorB(255, 255, 255, 255);
			img.renderFlags = e_Mode2D | e_DepthTestOff | e_AlphaAdditive | e_CullModeNone | e_FillModeSolid;
			aux->PushImage(img);
		}
	}

	bool IsEditorReady() { return g_editorReady; }
	void SetEditorReady(bool ready) { g_editorReady = ready; }

	CVideoPreviewPanel::CVideoPreviewPanel(QWidget* pParent)
		: QWidget(pParent)
	{
		setWindowTitle(tr("Preview"));
		// the checkerboard texture needs the renderer: not while a restored pane is created before the main frame is up
		if (IsEditorReady())
		{
			AcquireChecker();
			m_checkerHeld = true;
		}

		QVBoxLayout* const pMain = new QVBoxLayout(this);
		pMain->setContentsMargins(4, 4, 4, 4);

		m_pViewportHost = new QWidget(this);
		m_pViewportHost->setMinimumSize(160, 90);
		QVBoxLayout* const pHostLayout = new QVBoxLayout(m_pViewportHost);
		pHostLayout->setContentsMargins(0, 0, 0, 0);
		pMain->addWidget(m_pViewportHost, 1);

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
		m_pFit = new QComboBox(this);
		m_pFit->addItem(tr("Fit"));
		m_pFit->addItem(tr("100%"));
		pBar->addWidget(m_pStepBack);
		pBar->addWidget(m_pPlay);
		pBar->addWidget(m_pStop);
		pBar->addWidget(m_pStepForward);
		pBar->addWidget(m_pLoop);
		pBar->addWidget(m_pTime);
		pBar->addStretch(1);
		pBar->addWidget(m_pAudio);
		pBar->addWidget(m_pFit);
		pMain->addLayout(pBar);

		connect(m_pPlay, &QToolButton::clicked, this, &CVideoPreviewPanel::OnPlayPause);
		connect(m_pStop, &QToolButton::clicked, this, &CVideoPreviewPanel::OnStop);
		connect(m_pStepBack, &QToolButton::clicked, this, [this]() { OnStep(-1); });
		connect(m_pStepForward, &QToolButton::clicked, this, [this]() { OnStep(+1); });
		connect(m_pSlider, &QSlider::valueChanged, this, &CVideoPreviewPanel::OnSliderMoved);
		connect(m_pLoop, &QCheckBox::toggled, this, &CVideoPreviewPanel::OnLoopToggled);
		connect(m_pAudio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CVideoPreviewPanel::OnAudioModeChanged);
		connect(m_pFit, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) { m_fit = index == 0; });

		if (!CPreviewSession::RuntimeAvailable())
			SetStatus(tr("The CEVPlayer runtime plug-in is not loaded: enable it in the .cryproject to preview. Assets can still be imported."));
		else
			SetStatus(tr("No video loaded."));

		GetIEditor()->RegisterNotifyListener(this);
		UpdateTransport();
	}

	CVideoPreviewPanel::~CVideoPreviewPanel()
	{
		GetIEditor()->UnregisterNotifyListener(this);
		// order of docs/research/04 §4.7: player first, then the shared texture, then the viewport (its destructor flushes the render thread)
		m_session.Release();
		ReleaseCheckerOnce();
		DestroyViewport();
	}

	void CVideoPreviewPanel::ReleaseCheckerOnce()
	{
		if (!m_checkerHeld)
			return;
		m_checkerHeld = false;
		ReleaseChecker();
	}

	void CVideoPreviewPanel::showEvent(QShowEvent* pEvent)
	{
		QWidget::showEvent(pEvent);
		if (!m_pViewport && !m_createScheduled && !m_shutdown)
		{
			// wait until the panel sits in its final window: the swap chain binds to the native window of the viewport
			m_createScheduled = true;
			QTimer::singleShot(0, this, [this]()
			{
				m_createScheduled = false;
				if (m_pViewport || m_shutdown || !isVisible())
					return;
				if (!IsEditorReady())
				{
					m_waitingForMainFrame = true; // restored pane: wait for eNotify_OnMainFrameInitialized
					return;
				}
				CreateViewport();
			});
		}
	}

	void CVideoPreviewPanel::CreateViewport()
	{
		if (gEnv == nullptr || gEnv->pRenderer == nullptr || m_shutdown || !IsEditorReady())
			return;
		if (!m_checkerHeld)
		{
			AcquireChecker();
			m_checkerHeld = true;
		}
		m_pViewport = new CVideoViewport(m_pViewportHost);
		m_pViewportHost->layout()->addWidget(m_pViewport);
		connect(m_pViewport, &QViewport::SignalRender, this, [this](const SRenderContext& rc) { Render(rc); });
		// queued: the viewport must be deleted from the event loop, not from inside its own event
		connect(m_pViewport, &CVideoViewport::signalNeedsRecreate, this, &CVideoPreviewPanel::OnViewportNeedsRecreate, Qt::QueuedConnection);
		m_pViewport->show();
	}

	void CVideoPreviewPanel::DestroyViewport()
	{
		if (m_pViewport)
		{
			delete m_pViewport.data();
			m_pViewport = nullptr;
		}
	}

	void CVideoPreviewPanel::OnViewportNeedsRecreate()
	{
		if (m_shutdown || m_recreatePending)
			return;
		m_recreatePending = true;
		// one more turn of the event loop so that Qt has finished re-creating the native windows of the dock
		QTimer::singleShot(0, this, [this]()
		{
			m_recreatePending = false;
			if (m_shutdown)
				return;
			DestroyViewport();
			CreateViewport();
		});
	}

	void CVideoPreviewPanel::OnEditorNotifyEvent(EEditorNotifyEvent event)
	{
		if (event == eNotify_OnMainFrameInitialized)
		{
			SetEditorReady(true);
			if (m_waitingForMainFrame && !m_pViewport && !m_shutdown)
			{
				m_waitingForMainFrame = false;
				QTimer::singleShot(0, this, [this]()
				{
					if (!m_pViewport && !m_shutdown && isVisible())
						CreateViewport();
				});
			}
			return;
		}
		if (event == eNotify_OnQuit)
		{
			// The engine shuts down after this (renderer and runtime plug-in go): release everything that needs them now,
			// in the order player, shared texture, viewport. The panel itself may live on until Qt deletes it.
			SetEditorReady(false);
			m_shutdown = true;
			m_session.Release();
			ReleaseCheckerOnce();
			DestroyViewport();
			return;
		}
		if (event == eNotify_OnIdleUpdate)
			Tick();
	}

	void CVideoPreviewPanel::SetStatus(const QString& text)
	{
		m_pStatus->setText(text);
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

	void CVideoPreviewPanel::SetSource(const SSource& source)
	{
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
		if (!CPreviewSession::RuntimeAvailable())
		{
			SetStatus(tr("The CEVPlayer runtime plug-in is not loaded: enable it in the .cryproject to preview."));
			return;
		}
		const EPreviewAudio audio = static_cast<EPreviewAudio>(m_pAudio->currentData().toInt());
		if (!m_session.Load(m_source.descriptorRel, audio, m_source.videoRel, m_source.avOffsetMs))
		{
			SetStatus(tr("The preview player rejected %1.").arg(QString::fromUtf8(m_source.descriptorRel.c_str())));
			return;
		}
		m_session.Player()->SetLoop(m_pLoop->isChecked());
		SetStatus(tr("Loading %1 ...").arg(QString::fromUtf8(m_source.descriptorRel.c_str())));
		UpdateTransport();
	}

	void CVideoPreviewPanel::Clear()
	{
		m_hasSource = false;
		m_source = SSource();
		m_session.Release();
		SetStatus(CPreviewSession::RuntimeAvailable() ? tr("No video loaded.") : tr("The CEVPlayer runtime plug-in is not loaded."));
		UpdateTransport();
	}

	void CVideoPreviewPanel::ReleasePlayer()
	{
		m_session.Release();
		UpdateTransport();
	}

	void CVideoPreviewPanel::OnPlayPause()
	{
		CEVPlayer::IVideoPlayer* const p = m_session.Player();
		if (p == nullptr || !p->IsLoaded())
			return;
		switch (p->GetState())
		{
		case CEVPlayer::EVideoPlayerState::Playing: p->Pause(); break;
		case CEVPlayer::EVideoPlayerState::Paused: p->Resume(); break;
		default: p->Play(); break;
		}
	}

	void CVideoPreviewPanel::OnStop()
	{
		if (CEVPlayer::IVideoPlayer* const p = m_session.Player())
			p->Stop();
	}

	void CVideoPreviewPanel::OnStep(int direction)
	{
		CEVPlayer::IVideoPlayer* const p = m_session.Player();
		if (p == nullptr || !p->IsLoaded())
			return;
		if (p->GetState() == CEVPlayer::EVideoPlayerState::Playing)
			p->Pause();
		// the player has no frame step: pause and seek one frame period away (the seek shows the frame while paused)
		const double duration = p->GetDuration();
		double t = p->GetTime() + direction / Fps();
		t = std::max(0.0, duration > 0.0 ? std::min(t, duration) : t);
		p->Seek(t);
	}

	void CVideoPreviewPanel::OnSliderMoved(int value)
	{
		if (m_sliderUpdating)
			return;
		CEVPlayer::IVideoPlayer* const p = m_session.Player();
		if (p == nullptr || !p->IsLoaded() || p->GetDuration() <= 0.0)
			return;
		p->Seek(p->GetDuration() * (static_cast<double>(value) / 10000.0));
	}

	void CVideoPreviewPanel::OnLoopToggled(bool on)
	{
		if (CEVPlayer::IVideoPlayer* const p = m_session.Player())
			p->SetLoop(on);
	}

	void CVideoPreviewPanel::OnAudioModeChanged(int)
	{
		Reload();
	}

	void CVideoPreviewPanel::UpdateTransport()
	{
		CEVPlayer::IVideoPlayer* const p = m_session.Player();
		const bool loaded = p != nullptr && p->IsLoaded();
		m_pPlay->setEnabled(loaded);
		m_pStop->setEnabled(loaded);
		m_pStepBack->setEnabled(loaded);
		m_pStepForward->setEnabled(loaded);
		m_pSlider->setEnabled(loaded);
		m_pPlay->setText(loaded && p->GetState() == CEVPlayer::EVideoPlayerState::Playing ? tr("Pause") : tr("Play"));
		if (!loaded)
		{
			m_sliderUpdating = true;
			m_pSlider->setValue(0);
			m_sliderUpdating = false;
			m_pTime->setText(Timecode(0.0));
		}
	}

	void CVideoPreviewPanel::Tick()
	{
		if (m_shutdown || !isVisible())
			return;
		if (GetIEditor()->IsInGameMode())
		{
			SetStatus(tr("Game mode is running: the preview is paused."));
			return;
		}

		std::string error;
		if (m_session.TakeError(error))
			SetStatus(tr("Playback error: %1").arg(QString::fromUtf8(error.c_str())));
		if (m_session.TakeReady())
		{
			if (CEVPlayer::IVideoPlayer* const p = m_session.Player())
			{
				p->SetLoop(m_pLoop->isChecked());
				SetStatus(tr("%1 x %2, %3 s, %4%5")
					.arg(p->GetWidth()).arg(p->GetHeight()).arg(p->GetDuration(), 0, 'f', 2)
					.arg(p->HasAlpha() ? tr("alpha") : tr("opaque"))
					.arg(p->HasAudio() ? tr(", audio") : tr(", no audio")));
			}
		}

		if (CEVPlayer::IVideoPlayer* const p = m_session.Player())
		{
			if (p->IsLoaded())
			{
				const double duration = p->GetDuration();
				const double t = p->GetTime();
				m_pTime->setText(Timecode(t) + " / " + Timecode(duration));
				if (duration > 0.0 && !m_pSlider->isSliderDown())
				{
					m_sliderUpdating = true;
					m_pSlider->setValue(static_cast<int>(std::min(1.0, t / duration) * 10000.0));
					m_sliderUpdating = false;
				}
				m_pPlay->setText(p->GetState() == CEVPlayer::EVideoPlayerState::Playing ? tr("Pause") : tr("Play"));
				m_pPlay->setEnabled(true);
				m_pStop->setEnabled(true);
				m_pStepBack->setEnabled(true);
				m_pStepForward->setEnabled(true);
				m_pSlider->setEnabled(true);
			}
		}

		if (m_pViewport && !m_recreatePending && !m_pViewport->IsStale() && m_pViewport->isVisible())
			m_pViewport->Update(); // renders through Render() below (every idle: a swap chain that is not drawn goes stale)
	}

	void CVideoPreviewPanel::Render(const SRenderContext& rc)
	{
		IRenderAuxGeom* const aux = rc.pAuxGeom;
		if (aux == nullptr || rc.viewport == nullptr)
			return;
		const float W = static_cast<float>(rc.viewport->Width());
		const float H = static_cast<float>(rc.viewport->Height());
		if (W < 2.f || H < 2.f)
			return;

		CEVPlayer::IVideoPlayer* const p = m_session.Player();
		const bool loaded = p != nullptr && p->IsLoaded();
		const int textureId = loaded ? p->GetTextureId() : 0; // every frame: Load / a variant change re-creates the texture
		float vw = loaded ? static_cast<float>(p->GetWidth()) : 16.f;
		float vh = loaded ? static_cast<float>(p->GetHeight()) : 9.f;
		if (vw < 1.f || vh < 1.f)
		{
			vw = 16.f;
			vh = 9.f;
		}

		// destination rectangle in device pixels: fit with a margin, or 1:1 centred
		const float margin = 8.f;
		float scale = std::min((W - 2.f * margin) / vw, (H - 2.f * margin) / vh);
		if (!m_fit && loaded && vw <= W && vh <= H)
			scale = 1.f; // 100% only while it fits; a larger video stays fitted (no panning yet)
		scale = std::max(scale, 0.01f);
		SRectF dst;
		dst.w = std::floor(vw * scale);
		dst.h = std::floor(vh * scale);
		dst.x = std::floor(0.5f * (W - dst.w));
		dst.y = std::floor(0.5f * (H - dst.h));

		DrawChecker(aux, dst);
		if (textureId > 0)
		{
			DrawVideo(aux, textureId, dst, p->HasAlpha());
			if (!m_loggedFirstDraw)
			{
				m_loggedFirstDraw = true;
				CryLogAlways("[CEVEditor] preview draws texture id %d (%dx%d, alpha %d), viewport %dx%d", textureId, p->GetWidth(), p->GetHeight(), p->HasAlpha() ? 1 : 0, static_cast<int>(W), static_cast<int>(H));
			}
		}
	}
}
