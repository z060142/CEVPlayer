// FlowGraph nodes: ten hand described composite nodes in three categories (docs/design/M6-ui-source.md section 4.2).
// Registered through the engine's auto registration list (CAutoRegFlowNodeBase), so CVideoPlugin's
// PLUGIN_FLOWNODE_REGISTER picks them up.
//
//   Video:PlayOnScreen, Video:PlayOnMaterial          ready to use: the node owns its player
//   VideoUI:PlayToSurface, SurfaceInfo, SurfaceEvents for UIs: a named surface (surface id), no handle needed
//   VideoAdvanced:Open, Transport, Settings, Info, Events   handle based building blocks
//
// The nodes contain no playback logic: every action and every value goes through the API table (Api::Call on a table
// entry, by name). Players are addressed by the integer handles of CApiPlayers; the nodes never touch IVideoPlayer.
#include "StdAfx.h"
#include "PluginEnv.h"

#include <initializer_list>
#include <string>

#include <CryFlowGraph/IFlowBaseNode.h>
#include <Cry3DEngine/IMaterial.h>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"
#include "CEVCore/Status.h"

namespace CEVPlayer
{
	namespace Api
	{
		namespace
		{
			// ------------------------------------------------------------------------------------------ table access
			const SEntry& E(const char* name)
			{
				const SEntry* const pEntry = FindByName(name);
				CRY_ASSERT(pEntry != nullptr, "API table entry <%s> is missing", name);
				return *pEntry;
			}

			bool Run(const char* name, int handle, std::initializer_list<SValue> args = {}, SValue* pResult = nullptr, IMaterial* pMaterial = nullptr)
			{
				SValue in[kMaxParams];
				uint32_t n = 0;
				for (const SValue& v : args)
				{
					if (n < kMaxParams)
						in[n++] = v;
				}
				return Call(E(name), handle, in, n, pResult, pMaterial);
			}

			SValue Q(const char* name, int handle)
			{
				SValue v;
				Call(E(name), handle, nullptr, 0, &v);
				return v;
			}

			SValue I(int v) { return SValue::FromInt(v); }
			SValue B(bool v) { return SValue::FromBool(v); }
			SValue F(float v) { return SValue::FromFloat(v); }
			SValue S(const char* v) { return SValue::FromString(v); }

			bool ApiAvailable() { return mEnv != nullptr && mEnv->pApi != nullptr; }
			bool HandleAlive(int handle) { return handle != 0 && ApiAvailable() && mEnv->pApi->Find(handle, false) != nullptr; }

			//! Autoplay: only while the game runs (not while the editor is in edit mode).
			bool GameRunning() { return gEnv != nullptr && (!gEnv->IsEditor() || gEnv->IsEditorGameMode()); }

			IMaterial* EntityMaterial(IEntity* pEntity)
			{
				if (pEntity == nullptr)
					return nullptr;
				IMaterial* pMaterial = pEntity->GetMaterial();
				if (pMaterial == nullptr)
					pMaterial = pEntity->GetRenderMaterial(-1);
				return pMaterial;
			}

			const char* const kFitEnum = "enum_int:Fill=0,Fit=1,Letterbox=2";
			const char* const kOutputEnum = "enum_int:Surface=0,Screen=1,Material=2";
			const char* const kDecodeEnum = "enum_int:Auto=0,GPU=1,CPU=2";

			//! Where the events of a node go: the graph / node that activated it last. Events arrive later than
			//! the call that caused them and a graph may die in between, so the graph is only used while the flow
			//! system still knows it under the id it had then.
			class CGraphRef
			{
			public:
				void Set(const IFlowNode::SActivationInfo* pActInfo)
				{
					m_actInfo = *pActInfo;
					m_graphId = pActInfo->pGraph != nullptr ? pActInfo->pGraph->GetGraphId() : InvalidFlowGraphId;
				}
				bool IsValid() const
				{
					return m_graphId != InvalidFlowGraphId && m_actInfo.pGraph != nullptr && gEnv != nullptr && gEnv->pFlowSystem != nullptr
						&& gEnv->pFlowSystem->GetGraphById(m_graphId) == m_actInfo.pGraph;
				}
				IFlowNode::SActivationInfo* Info() { return &m_actInfo; }
			private:
				IFlowNode::SActivationInfo m_actInfo;
				TFlowGraphId m_graphId = InvalidFlowGraphId;
			};

			// ------------------------------------------------------------------------------------------ base
			//! Common part: the graph reference, one subscribed handle, an optionally owned player that dies with the
			//! node (Stop, node / graph destruction).
			class CNodeBase : public CFlowBaseNode<eNCT_Instanced>, public IEventSink
			{
			public:
				virtual ~CNodeBase()
				{
					ReleasePlayer();
				}

				virtual void GetMemoryUsage(ICrySizer* pSizer) const override { pSizer->Add(*this); }

				// IEventSink
				virtual void OnApiEvent(int handle, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					if (handle == 0 || handle != m_subscribed || event >= eEvent_Count || !m_graph.IsValid())
						return;
					OnPlayerEvent(m_graph.Info(), event, args, argCount);
				}

			protected:
				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) {}

				void Subscribe(int handle)
				{
					Unsubscribe();
					if (handle != 0 && ApiAvailable())
					{
						mEnv->pApi->Subscribe(handle, this);
						m_subscribed = handle;
					}
				}

				void Unsubscribe()
				{
					if (ApiAvailable())
						mEnv->pApi->UnsubscribeAll(this);
					m_subscribed = 0;
				}

				//! Destroys the player this node made (if it still exists) and forgets it.
				void ReleasePlayer()
				{
					Unsubscribe();
					if (HandleAlive(m_owned))
						Run("Destroy", m_owned);
					m_owned = 0;
				}

				bool HasPlayer() const { return HandleAlive(m_owned); }

				//! Error payload first, then the trigger.
				static void EmitError(SActivationInfo* pInfo, int outError, int outCode, int outMessage, int code, const char* message)
				{
					ActivateOutput(pInfo, outCode, code);
					ActivateOutput(pInfo, outMessage, string(message != nullptr ? message : ""));
					ActivateOutput(pInfo, outError, true);
				}

				//! Loads `video` (unless it is already the loaded one), then plays from the beginning. False when the load
				//! was refused right away (the Error event of the player follows).
				bool PlayFromStart(const string& video)
				{
					const bool sameVideo = stricmp(Q("Path", m_owned).s.c_str(), video.c_str()) == 0
						&& Q("State", m_owned).i != static_cast<int>(EVideoPlayerState::Error);
					if (sameVideo)
						Run("Stop", m_owned);
					else if (!Run("Load", m_owned, { S(video.c_str()) }))
						return false;
					return Run("Play", m_owned);
				}

				CGraphRef m_graph;
				int m_owned = 0;
				int m_subscribed = 0;
			};

			// ========================================================================================== Video
			class CPlayOnScreenNode final : public CNodeBase
			{
				enum EIn { eI_Play, eI_Stop, eI_Video, eI_Fit, eI_Loop, eI_Skippable, eI_Decode };
				enum EOut { eO_Started, eO_Ended, eO_Skipped, eO_Error, eO_ErrorCode, eO_ErrorMessage };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CPlayOnScreenNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig_Void("Play", _HELP("Loads Video and plays it fullscreen from the beginning (again from the beginning when it already plays).")),
						InputPortConfig_Void("Stop", _HELP("Stops the video and destroys the player of this node.")),
						InputPortConfig<string>("Video", string(), _HELP("Video file (.webm) or runtime descriptor (.cevvideo), for example Videos/intro.webm.")),
						InputPortConfig<int>("Fit", 2, _HELP("How the picture is placed: Fill stretches it, Fit keeps the aspect and shows the scene in the bars, Letterbox adds black bars."), nullptr, _UICONFIG(kFitEnum)),
						InputPortConfig<bool>("Loop", false, _HELP("Restart at the beginning when the end is reached.")),
						InputPortConfig<bool>("Skippable", false, _HELP("A key, mouse or gamepad button press stops the video and triggers Skipped.")),
						InputPortConfig<int>("Decode", 0, _HELP("Decoder: Auto, GPU or CPU (this build always decodes on the CPU)."), nullptr, _UICONFIG(kDecodeEnum)),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Started", _HELP("Playback started.")),
						OutputPortConfig_Void("Ended", _HELP("The end of a non-looping video was reached.")),
						OutputPortConfig_Void("Skipped", _HELP("A button press stopped the skippable video.")),
						OutputPortConfig_Void("Error", _HELP("Loading or playback failed; ErrorCode and ErrorMessage tell why.")),
						OutputPortConfig<int>("ErrorCode", _HELP("Status code of the last failure.")),
						OutputPortConfig<string>("ErrorMessage", _HELP("Short text of the last failure.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Plays a video fullscreen on top of the game. The node owns its player: Stop, or the end of the graph, destroys it.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate)
						return;
					if (IsPortActive(pActInfo, eI_Stop))
					{
						m_graph.Set(pActInfo);
						ReleasePlayer();
					}
					if (IsPortActive(pActInfo, eI_Play))
						Play(pActInfo);
				}

			private:
				void Play(SActivationInfo* pActInfo)
				{
					m_graph.Set(pActInfo);
					const string video = GetPortString(pActInfo, eI_Video);
					if (video.empty())
					{
						EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, static_cast<int>(cev::Status::InvalidArgument), "Video is empty");
						return;
					}
					if (!HasPlayer())
					{
						SValue handle;
						if (!Run("Create", 0, {}, &handle))
						{
							EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, ApiAvailable() ? mEnv->pApi->LastErrorCode() : 0, ApiAvailable() ? mEnv->pApi->LastErrorMessage() : "plug-in not available");
							return;
						}
						m_owned = handle.i;
						Subscribe(m_owned);
					}
					Run("SetDecode", m_owned, { I(GetPortInt(pActInfo, eI_Decode)) });
					Run("SetOutput", m_owned, { I(static_cast<int>(EOutputMode::Screen)), S(""), I(0), I(-1), I(GetPortInt(pActInfo, eI_Fit)) });
					Run("SetLoop", m_owned, { B(GetPortBool(pActInfo, eI_Loop)) });
					Run("SetSkippable", m_owned, { B(GetPortBool(pActInfo, eI_Skippable)) });
					PlayFromStart(video);
				}

				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					switch (event)
					{
					case eEvent_Started: ActivateOutput(pInfo, eO_Started, true); break;
					case eEvent_Ended: ActivateOutput(pInfo, eO_Ended, true); break;
					case eEvent_Skipped: ActivateOutput(pInfo, eO_Skipped, true); break;
					case eEvent_Error: EmitError(pInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, argCount > 0 ? args[0].i : 0, argCount > 1 ? args[1].s.c_str() : ""); break;
					default: break;
					}
				}
			};

			class CPlayOnMaterialNode final : public CNodeBase
			{
				enum EIn { eI_Play, eI_Stop, eI_Video, eI_Material, eI_Slot, eI_SubMaterial, eI_Loop, eI_Autoplay, eI_Decode };
				enum EOut { eO_Started, eO_Ended, eO_Error, eO_ErrorCode, eO_ErrorMessage };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CPlayOnMaterialNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig_Void("Play", _HELP("Loads Video and plays it on the material from the beginning (again from the beginning when it already plays).")),
						InputPortConfig_Void("Stop", _HELP("Stops the video, removes it from the material and destroys the player of this node.")),
						InputPortConfig<string>("Video", string(), _HELP("Video file (.webm) or runtime descriptor (.cevvideo), for example Videos/screen.webm.")),
						InputPortConfig<string>("Material", string(), _HELP("Material that shows the video; empty uses the material of the node's entity.")),
						InputPortConfig<int>("Slot", 0, _HELP("Texture slot of the material that gets the video (0 = diffuse).")),
						InputPortConfig<int>("SubMaterial", -1, _HELP("Sub-material index; -1 uses the material itself.")),
						InputPortConfig<bool>("Loop", false, _HELP("Restart at the beginning when the end is reached.")),
						InputPortConfig<bool>("Autoplay", false, _HELP("Play as soon as the game starts, without a Play trigger.")),
						InputPortConfig<int>("Decode", 0, _HELP("Decoder: Auto, GPU or CPU (this build always decodes on the CPU)."), nullptr, _UICONFIG(kDecodeEnum)),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Started", _HELP("Playback started.")),
						OutputPortConfig_Void("Ended", _HELP("The end of a non-looping video was reached.")),
						OutputPortConfig_Void("Error", _HELP("Loading, playback or the material binding failed; ErrorCode and ErrorMessage tell why.")),
						OutputPortConfig<int>("ErrorCode", _HELP("Status code of the last failure.")),
						OutputPortConfig<string>("ErrorMessage", _HELP("Short text of the last failure.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Plays a video on a texture slot of a material (a screen in the level). The node owns its player: Stop, or the end of the graph, destroys it.");
					config.SetCategory(EFLN_APPROVED);
					config.nFlags |= EFLN_TARGET_ENTITY;
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event == eFE_Initialize)
					{
						if (GetPortBool(pActInfo, eI_Autoplay) && GameRunning())
							Play(pActInfo);
						return;
					}
					if (event != eFE_Activate)
						return;
					if (IsPortActive(pActInfo, eI_Stop))
					{
						m_graph.Set(pActInfo);
						ReleasePlayer();
					}
					if (IsPortActive(pActInfo, eI_Play))
						Play(pActInfo);
				}

			private:
				void Play(SActivationInfo* pActInfo)
				{
					m_graph.Set(pActInfo);
					const string video = GetPortString(pActInfo, eI_Video);
					if (video.empty())
					{
						EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, static_cast<int>(cev::Status::InvalidArgument), "Video is empty");
						return;
					}
					if (!HasPlayer())
					{
						SValue handle;
						if (!Run("Create", 0, {}, &handle))
						{
							EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, ApiAvailable() ? mEnv->pApi->LastErrorCode() : 0, ApiAvailable() ? mEnv->pApi->LastErrorMessage() : "plug-in not available");
							return;
						}
						m_owned = handle.i;
						Subscribe(m_owned);
					}
					Run("SetDecode", m_owned, { I(GetPortInt(pActInfo, eI_Decode)) });
					const string material = GetPortString(pActInfo, eI_Material);
					if (!Run("SetOutput", m_owned, { I(static_cast<int>(EOutputMode::Material)), S(material.c_str()), I(GetPortInt(pActInfo, eI_Slot)), I(GetPortInt(pActInfo, eI_SubMaterial)), I(2) }, nullptr, EntityMaterial(pActInfo->pEntity)))
					{
						ReleasePlayer();
						EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, static_cast<int>(cev::Status::InvalidArgument),
							"material binding failed: no material (name empty and no entity), material not found or bad slot");
						return;
					}
					Run("SetLoop", m_owned, { B(GetPortBool(pActInfo, eI_Loop)) });
					PlayFromStart(video);
				}

				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					switch (event)
					{
					case eEvent_Started: ActivateOutput(pInfo, eO_Started, true); break;
					case eEvent_Ended: ActivateOutput(pInfo, eO_Ended, true); break;
					case eEvent_Error: EmitError(pInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, argCount > 0 ? args[0].i : 0, argCount > 1 ? args[1].s.c_str() : ""); break;
					default: break;
					}
				}
			};

			// ========================================================================================== VideoUI
			class CPlayToSurfaceNode final : public CNodeBase
			{
				enum EIn { eI_Play, eI_Stop, eI_Video, eI_SurfaceId, eI_Loop, eI_Autoplay, eI_Decode, eI_PauseDecodeWhenHidden };
				enum EOut { eO_Ready, eO_TextureId, eO_Width, eO_Height, eO_Duration, eO_HasAlpha, eO_Ended, eO_Error, eO_ErrorCode, eO_ErrorMessage };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CPlayToSurfaceNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig_Void("Play", _HELP("Creates the surface, loads Video into it and plays it from the beginning (again from the beginning when it already plays).")),
						InputPortConfig_Void("Stop", _HELP("Stops the video and destroys the surface (UIs showing it go transparent).")),
						InputPortConfig<string>("Video", string(), _HELP("Video file (.webm) or runtime descriptor (.cevvideo), for example Videos/menu_bg.webm.")),
						InputPortConfig<string>("SurfaceID", string(), _HELP("Name UIs use to show the video: 1 to 63 of A-Z a-z 0-9 _ . : -, case-insensitive, unique.")),
						InputPortConfig<bool>("Loop", false, _HELP("Restart at the beginning when the end is reached.")),
						InputPortConfig<bool>("Autoplay", false, _HELP("Play as soon as the game starts, without a Play trigger.")),
						InputPortConfig<int>("Decode", 0, _HELP("Decoder: Auto, GPU or CPU (this build always decodes on the CPU)."), nullptr, _UICONFIG(kDecodeEnum)),
						InputPortConfig<bool>("PauseDecodeWhenHidden", true, _HELP("While no UI shows the surface: only decode, and stop decoding after cev_hiddenStopSeconds (time, sound and events go on).")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Ready", _HELP("The video is loaded; TextureId, Width, Height, Duration and HasAlpha are set just before.")),
						OutputPortConfig<int>("TextureId", _HELP("Renderer texture id of the surface (RGBA8, premultiplied alpha).")),
						OutputPortConfig<int>("Width", _HELP("Picture width in pixels.")),
						OutputPortConfig<int>("Height", _HELP("Picture height in pixels.")),
						OutputPortConfig<float>("Duration", _HELP("Duration of the video in seconds.")),
						OutputPortConfig<bool>("HasAlpha", _HELP("The video has an alpha channel.")),
						OutputPortConfig_Void("Ended", _HELP("The end of a non-looping video was reached.")),
						OutputPortConfig_Void("Error", _HELP("The surface could not be created (invalid or taken SurfaceID) or loading / playback failed.")),
						OutputPortConfig<int>("ErrorCode", _HELP("Status code of the last failure (1 = invalid SurfaceID, 2 = SurfaceID already in use, others: loading).")),
						OutputPortConfig<string>("ErrorMessage", _HELP("Short text of the last failure.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Plays a video into a named surface that any UI can show by its SurfaceID. The node owns the surface: Stop, or the end of the graph, destroys it.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event == eFE_Initialize)
					{
						if (GetPortBool(pActInfo, eI_Autoplay) && GameRunning())
							Play(pActInfo);
						return;
					}
					if (event != eFE_Activate)
						return;
					if (IsPortActive(pActInfo, eI_Stop))
					{
						m_graph.Set(pActInfo);
						ReleasePlayer();
					}
					if (IsPortActive(pActInfo, eI_Play))
						Play(pActInfo);
				}

			private:
				void Play(SActivationInfo* pActInfo)
				{
					m_graph.Set(pActInfo);
					const string video = GetPortString(pActInfo, eI_Video);
					const string surfaceId = GetPortString(pActInfo, eI_SurfaceId);
					if (video.empty())
					{
						EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, static_cast<int>(cev::Status::InvalidArgument), "Video is empty");
						return;
					}
					if (HasPlayer() && stricmp(Q("SurfaceId", m_owned).s.c_str(), surfaceId.c_str()) != 0)
						ReleasePlayer(); // another SurfaceID: the old surface goes away
					if (!HasPlayer())
					{
						SValue handle;
						if (!Run("CreateForSurface", 0, { S(surfaceId.c_str()) }, &handle))
						{
							EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, ApiAvailable() ? mEnv->pApi->LastErrorCode() : 0, ApiAvailable() ? mEnv->pApi->LastErrorMessage() : "plug-in not available");
							return;
						}
						m_owned = handle.i;
						Subscribe(m_owned);
					}
					Run("SetDecode", m_owned, { I(GetPortInt(pActInfo, eI_Decode)) });
					Run("SetLoop", m_owned, { B(GetPortBool(pActInfo, eI_Loop)) });
					Run("SetPauseDecodeWhenHidden", m_owned, { B(GetPortBool(pActInfo, eI_PauseDecodeWhenHidden)) });
					PlayFromStart(video);
				}

				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					switch (event)
					{
					case eEvent_Ready:
						ActivateOutput(pInfo, eO_TextureId, Q("TextureId", m_owned).i);
						ActivateOutput(pInfo, eO_Width, Q("Width", m_owned).i);
						ActivateOutput(pInfo, eO_Height, Q("Height", m_owned).i);
						ActivateOutput(pInfo, eO_Duration, Q("Duration", m_owned).f);
						ActivateOutput(pInfo, eO_HasAlpha, Q("HasAlpha", m_owned).b);
						ActivateOutput(pInfo, eO_Ready, true);
						break;
					case eEvent_Ended: ActivateOutput(pInfo, eO_Ended, true); break;
					case eEvent_Error: EmitError(pInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, argCount > 0 ? args[0].i : 0, argCount > 1 ? args[1].s.c_str() : ""); break;
					default: break;
					}
				}
			};

			class CSurfaceInfoNode final : public CNodeBase
			{
				enum EIn { eI_Get, eI_SurfaceId };
				enum EOut { eO_Exists, eO_TextureId, eO_Width, eO_Height, eO_PixelLayout, eO_Premultiplied, eO_HasAlpha, eO_FrameSerial, eO_State };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CSurfaceInfoNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig_Void("Get", _HELP("Reads the values of the surface and sends them to the outputs.")),
						InputPortConfig<string>("SurfaceID", string(), _HELP("Surface to read (made by PlayToSurface, a Video Player component or another plug-in).")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig<bool>("Exists", _HELP("The surface exists; when false every other output is 0 or empty.")),
						OutputPortConfig<int>("TextureId", _HELP("Renderer texture id of the surface, 0 while there is none.")),
						OutputPortConfig<int>("Width", _HELP("Picture width in pixels, 0 until loaded.")),
						OutputPortConfig<int>("Height", _HELP("Picture height in pixels, 0 until loaded.")),
						OutputPortConfig<int>("PixelLayout", _HELP("Pixel layout of the texture: 0 = RGBA8, 1 = packed YUV420 (R8), 2 = packed YUVA420 (R8).")),
						OutputPortConfig<bool>("Premultiplied", _HELP("The texture holds premultiplied alpha (RGBA8 layout).")),
						OutputPortConfig<bool>("HasAlpha", _HELP("The video has an alpha channel.")),
						OutputPortConfig<string>("FrameSerial", _HELP("Number of uploaded pictures as decimal text; changes whenever the picture changed.")),
						OutputPortConfig<int>("State", _HELP("0 Stopped, 1 Playing, 2 Paused, 3 Ended, 4 Loading, 5 Error.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Reads a surface by its SurfaceID: texture id, size and layout for UI systems that use the texture directly.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate || !IsPortActive(pActInfo, eI_Get))
						return;
					const string surfaceId = GetPortString(pActInfo, eI_SurfaceId);
					const int handle = ApiAvailable() ? mEnv->pApi->AttachSurface(surfaceId.c_str()) : 0;
					if (handle == 0)
					{
						ActivateOutput(pActInfo, eO_TextureId, 0);
						ActivateOutput(pActInfo, eO_Width, 0);
						ActivateOutput(pActInfo, eO_Height, 0);
						ActivateOutput(pActInfo, eO_PixelLayout, 0);
						ActivateOutput(pActInfo, eO_Premultiplied, false);
						ActivateOutput(pActInfo, eO_HasAlpha, false);
						ActivateOutput(pActInfo, eO_FrameSerial, string());
						ActivateOutput(pActInfo, eO_State, 0);
						ActivateOutput(pActInfo, eO_Exists, false);
						return;
					}
					ActivateOutput(pActInfo, eO_TextureId, Q("TextureId", handle).i);
					ActivateOutput(pActInfo, eO_Width, Q("Width", handle).i);
					ActivateOutput(pActInfo, eO_Height, Q("Height", handle).i);
					ActivateOutput(pActInfo, eO_PixelLayout, Q("PixelLayout", handle).i);
					ActivateOutput(pActInfo, eO_Premultiplied, Q("IsPremultiplied", handle).b);
					ActivateOutput(pActInfo, eO_HasAlpha, Q("HasAlpha", handle).b);
					ActivateOutput(pActInfo, eO_FrameSerial, string(Q("FrameSerial", handle).s.c_str()));
					ActivateOutput(pActInfo, eO_State, Q("State", handle).i);
					ActivateOutput(pActInfo, eO_Exists, true);
				}
			};

			class CSurfaceEventsNode final : public CNodeBase
			{
				enum EIn { eI_SurfaceId };
				enum EOut { eO_Ready, eO_NewFrame, eO_FrameSerial, eO_Resized, eO_TextureId, eO_Width, eO_Height, eO_Closed };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CSurfaceEventsNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig<string>("SurfaceID", string(), _HELP("Surface to follow; it may appear later, the node waits for it (empty = stop following).")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Ready", _HELP("The surface's video is loaded (also sent at once when it is already loaded when the node starts following it).")),
						OutputPortConfig_Void("NewFrame", _HELP("A new picture was uploaded; FrameSerial is set just before.")),
						OutputPortConfig<string>("FrameSerial", _HELP("Number of the uploaded picture as decimal text.")),
						OutputPortConfig_Void("Resized", _HELP("The texture changed (size, texture id or layout); TextureId, Width and Height are set just before.")),
						OutputPortConfig<int>("TextureId", _HELP("Renderer texture id of the surface, sent with Ready and Resized.")),
						OutputPortConfig<int>("Width", _HELP("Picture width in pixels, sent with Ready and Resized.")),
						OutputPortConfig<int>("Height", _HELP("Picture height in pixels, sent with Ready and Resized.")),
						OutputPortConfig_Void("Closed", _HELP("The surface was destroyed; the node waits for it to appear again.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Follows a surface by its SurfaceID without a handle: Ready, NewFrame, Resized and Closed. The surface may be created after the node started.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					switch (event)
					{
					case eFE_Initialize:
						Follow(pActInfo);
						break;
					case eFE_Activate:
						if (IsPortActive(pActInfo, eI_SurfaceId))
							Follow(pActInfo);
						break;
					case eFE_Update:
						Poll(pActInfo);
						break;
					default:
						break;
					}
				}

			private:
				void Follow(SActivationInfo* pActInfo)
				{
					m_graph.Set(pActInfo);
					Unsubscribe(); // never owns: only forget the handle
					m_handle = 0;
					m_ready = false;
					m_surfaceId = GetPortString(pActInfo, eI_SurfaceId);
					if (pActInfo->pGraph != nullptr)
						pActInfo->pGraph->SetRegularlyUpdated(pActInfo->myID, !m_surfaceId.empty());
					Poll(pActInfo);
				}

				void SendTexture(SActivationInfo* pInfo)
				{
					m_generation = Q("Generation", m_handle).i;
					ActivateOutput(pInfo, eO_TextureId, Q("TextureId", m_handle).i);
					ActivateOutput(pInfo, eO_Width, Q("Width", m_handle).i);
					ActivateOutput(pInfo, eO_Height, Q("Height", m_handle).i);
				}

				void Poll(SActivationInfo* pActInfo)
				{
					if (m_surfaceId.empty() || !ApiAvailable())
						return;
					m_graph.Set(pActInfo);
					if (m_handle != 0 && !HandleAlive(m_handle))
					{
						// the surface was destroyed by its owner
						m_handle = 0;
						m_subscribed = 0;
						m_ready = false;
						ActivateOutput(pActInfo, eO_Closed, true);
					}
					if (m_handle == 0)
					{
						m_handle = mEnv->pApi->AttachSurface(m_surfaceId.c_str());
						if (m_handle == 0)
							return; // not there yet
						Subscribe(m_handle);
						if (Q("IsLoaded", m_handle).b)
						{
							// it was loaded before we followed it: its Ready event is gone
							m_ready = true;
							SendTexture(pActInfo);
							ActivateOutput(pActInfo, eO_Ready, true);
						}
						return;
					}
					if (m_ready && Q("Generation", m_handle).i != m_generation)
					{
						SendTexture(pActInfo);
						ActivateOutput(pActInfo, eO_Resized, true);
					}
				}

				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					switch (event)
					{
					case eEvent_Ready:
					{
						// a reload of the surface (another video) sends Ready again: when its texture changed, that is a Resized too
						const bool changed = m_ready && Q("Generation", m_handle).i != m_generation;
						m_ready = true;
						SendTexture(pInfo);
						if (changed)
							ActivateOutput(pInfo, eO_Resized, true);
						ActivateOutput(pInfo, eO_Ready, true);
						break;
					}
					case eEvent_NewFrame:
						ActivateOutput(pInfo, eO_FrameSerial, string(argCount > 0 ? args[0].s.c_str() : ""));
						ActivateOutput(pInfo, eO_NewFrame, true);
						break;
					default:
						break;
					}
				}

				string m_surfaceId;
				int m_handle = 0;
				int m_generation = 0;
				bool m_ready = false;
			};

			// ========================================================================================== VideoAdvanced
			class COpenNode final : public CNodeBase
			{
				enum EIn { eI_Open, eI_Close, eI_Video, eI_Output, eI_SurfaceId, eI_Fit, eI_Material, eI_Slot, eI_SubMaterial, eI_Decode };
				enum EOut { eO_Handle, eO_Ready, eO_Error, eO_ErrorCode, eO_ErrorMessage };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new COpenNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig_Void("Open", _HELP("Creates a player for the chosen Output and starts loading Video (a player opened before is destroyed first).")),
						InputPortConfig_Void("Close", _HELP("Destroys the player of this node; Handle becomes 0.")),
						InputPortConfig<string>("Video", string(), _HELP("Video file (.webm) or runtime descriptor (.cevvideo).")),
						InputPortConfig<int>("Output", 1, _HELP("Where the picture goes: Surface (a named surface for UIs), Screen (fullscreen) or Material (a texture slot)."), nullptr, _UICONFIG(kOutputEnum)),
						InputPortConfig<string>("SurfaceID", string(), _HELP("Output Surface: the surface name UIs use (1 to 63 of A-Z a-z 0-9 _ . : -).")),
						InputPortConfig<int>("Fit", 2, _HELP("Output Screen: Fill stretches, Fit keeps the aspect with the scene in the bars, Letterbox adds black bars."), nullptr, _UICONFIG(kFitEnum)),
						InputPortConfig<string>("Material", string(), _HELP("Output Material: material name; empty uses the material of the node's entity.")),
						InputPortConfig<int>("Slot", 0, _HELP("Output Material: texture slot (0 = diffuse).")),
						InputPortConfig<int>("SubMaterial", -1, _HELP("Output Material: sub-material index, -1 = the material itself.")),
						InputPortConfig<int>("Decode", 0, _HELP("Decoder: Auto, GPU or CPU (this build always decodes on the CPU)."), nullptr, _UICONFIG(kDecodeEnum)),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig<int>("Handle", _HELP("Handle of the opened player for Transport, Settings, Info and Events (0 after Close).")),
						OutputPortConfig_Void("Ready", _HELP("The video is loaded: size, duration and texture are valid.")),
						OutputPortConfig_Void("Error", _HELP("Open failed (bad SurfaceID, material) or loading / playback failed; ErrorCode and ErrorMessage tell why.")),
						OutputPortConfig<int>("ErrorCode", _HELP("Status code of the last failure.")),
						OutputPortConfig<string>("ErrorMessage", _HELP("Short text of the last failure.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Opens a player with its output and returns its handle for the other VideoAdvanced nodes. The node owns the player: Close, or the end of the graph, destroys it.");
					config.SetCategory(EFLN_APPROVED);
					config.nFlags |= EFLN_TARGET_ENTITY;
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate)
						return;
					if (IsPortActive(pActInfo, eI_Close))
					{
						m_graph.Set(pActInfo);
						ReleasePlayer();
						ActivateOutput(pActInfo, eO_Handle, 0);
					}
					if (IsPortActive(pActInfo, eI_Open))
						Open(pActInfo);
				}

			private:
				void Fail(SActivationInfo* pActInfo, int code, const char* message)
				{
					ReleasePlayer();
					ActivateOutput(pActInfo, eO_Handle, 0);
					EmitError(pActInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, code, message);
				}

				void Open(SActivationInfo* pActInfo)
				{
					m_graph.Set(pActInfo);
					ReleasePlayer();
					const string video = GetPortString(pActInfo, eI_Video);
					if (video.empty())
					{
						Fail(pActInfo, static_cast<int>(cev::Status::InvalidArgument), "Video is empty");
						return;
					}
					const int output = GetPortInt(pActInfo, eI_Output);
					SValue handle;
					const bool created = output == static_cast<int>(EOutputMode::Surface)
						? Run("CreateForSurface", 0, { S(GetPortString(pActInfo, eI_SurfaceId).c_str()) }, &handle)
						: Run("Create", 0, {}, &handle);
					if (!created)
					{
						Fail(pActInfo, ApiAvailable() ? mEnv->pApi->LastErrorCode() : 0, ApiAvailable() ? mEnv->pApi->LastErrorMessage() : "plug-in not available");
						return;
					}
					m_owned = handle.i;
					Subscribe(m_owned);
					Run("SetDecode", m_owned, { I(GetPortInt(pActInfo, eI_Decode)) });
					const string material = GetPortString(pActInfo, eI_Material);
					if (!Run("SetOutput", m_owned, { I(output), S(material.c_str()), I(GetPortInt(pActInfo, eI_Slot)), I(GetPortInt(pActInfo, eI_SubMaterial)), I(GetPortInt(pActInfo, eI_Fit)) }, nullptr, EntityMaterial(pActInfo->pEntity)))
					{
						Fail(pActInfo, static_cast<int>(cev::Status::InvalidArgument), "SetOutput failed: unknown Output, or the material is missing / not found / bad slot (see the log)");
						return;
					}
					ActivateOutput(pActInfo, eO_Handle, m_owned);
					Run("Load", m_owned, { S(video.c_str()) }); // a refused load sends the Error event
				}

				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					switch (event)
					{
					case eEvent_Ready: ActivateOutput(pInfo, eO_Ready, true); break;
					case eEvent_Error: EmitError(pInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, argCount > 0 ? args[0].i : 0, argCount > 1 ? args[1].s.c_str() : ""); break;
					default: break;
					}
				}
			};

			class CTransportNode final : public CNodeBase
			{
				enum EIn { eI_Handle, eI_Play, eI_Pause, eI_Stop, eI_Seek, eI_Seconds };
				enum EOut { eO_Done, eO_Error };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CTransportNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig<int>("Handle", 0, _HELP("Player handle (from VideoAdvanced:Open).")),
						InputPortConfig_Void("Play", _HELP("Plays from the start, or continues a paused video.")),
						InputPortConfig_Void("Pause", _HELP("Pauses playback.")),
						InputPortConfig_Void("Stop", _HELP("Stops and goes back to the beginning.")),
						InputPortConfig_Void("Seek", _HELP("Jumps to Seconds.")),
						InputPortConfig<float>("Seconds", 0.f, _HELP("Target time of Seek in seconds.")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Done", _HELP("The request was accepted.")),
						OutputPortConfig_Void("Error", _HELP("The request was rejected (invalid handle); see the log.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Play, Pause, Stop and Seek of a player handle.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate)
						return;
					const int handle = GetPortInt(pActInfo, eI_Handle);
					if (IsPortActive(pActInfo, eI_Play))
						Result(pActInfo, Run("Play", handle)); // Play also continues a paused video
					if (IsPortActive(pActInfo, eI_Pause))
						Result(pActInfo, Run("Pause", handle));
					if (IsPortActive(pActInfo, eI_Stop))
						Result(pActInfo, Run("Stop", handle));
					if (IsPortActive(pActInfo, eI_Seek))
						Result(pActInfo, Run("Seek", handle, { F(GetPortFloat(pActInfo, eI_Seconds)) }));
				}

			private:
				static void Result(SActivationInfo* pActInfo, bool ok) { ActivateOutput(pActInfo, ok ? eO_Done : eO_Error, true); }
			};

			class CSettingsNode final : public CNodeBase
			{
				enum EIn { eI_Handle, eI_Loop, eI_Rate, eI_Volume, eI_Language, eI_Variant, eI_Skippable, eI_PauseDecodeWhenHidden };
				enum EOut { eO_Done, eO_Error };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CSettingsNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig<int>("Handle", 0, _HELP("Player handle (from VideoAdvanced:Open).")),
						InputPortConfig<bool>("Loop", false, _HELP("Applied when triggered: restart at the beginning when the end is reached.")),
						InputPortConfig<float>("Rate", 1.f, _HELP("Applied when triggered: playback speed 0.25 to 4 (1 = normal).")),
						InputPortConfig<float>("Volume", 1.f, _HELP("Applied when triggered: player volume 0 to 1.")),
						InputPortConfig<string>("Language", string(), _HELP("Applied when triggered: audio language of the .cevvideo descriptor (empty = default choice).")),
						InputPortConfig<int>("Variant", -2, _HELP("Applied when triggered: -2 follows cev_variant, -1 automatic, 0 main video, n the n-th variant.")),
						InputPortConfig<bool>("Skippable", false, _HELP("Applied when triggered: a button press stops the playing video and sends Skipped.")),
						InputPortConfig<bool>("PauseDecodeWhenHidden", true, _HELP("Applied when triggered: a surface no UI shows only decodes, and stops decoding after cev_hiddenStopSeconds.")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Done", _HELP("The setting was applied.")),
						OutputPortConfig_Void("Error", _HELP("The setting was rejected (invalid handle or value); see the log.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Settings of a player handle: each input is applied when that input is triggered, the others stay as they are.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate)
						return;
					const int handle = GetPortInt(pActInfo, eI_Handle);
					if (IsPortActive(pActInfo, eI_Loop))
						Result(pActInfo, Run("SetLoop", handle, { B(GetPortBool(pActInfo, eI_Loop)) }));
					if (IsPortActive(pActInfo, eI_Rate))
						Result(pActInfo, Run("SetRate", handle, { F(GetPortFloat(pActInfo, eI_Rate)) }));
					if (IsPortActive(pActInfo, eI_Volume))
						Result(pActInfo, Run("SetVolume", handle, { F(GetPortFloat(pActInfo, eI_Volume)) }));
					if (IsPortActive(pActInfo, eI_Language))
						Result(pActInfo, Run("SetLanguage", handle, { S(GetPortString(pActInfo, eI_Language).c_str()) }));
					if (IsPortActive(pActInfo, eI_Variant))
						Result(pActInfo, Run("SetVariant", handle, { I(GetPortInt(pActInfo, eI_Variant)) }));
					if (IsPortActive(pActInfo, eI_Skippable))
						Result(pActInfo, Run("SetSkippable", handle, { B(GetPortBool(pActInfo, eI_Skippable)) }));
					if (IsPortActive(pActInfo, eI_PauseDecodeWhenHidden))
						Result(pActInfo, Run("SetPauseDecodeWhenHidden", handle, { B(GetPortBool(pActInfo, eI_PauseDecodeWhenHidden)) }));
				}

			private:
				static void Result(SActivationInfo* pActInfo, bool ok) { ActivateOutput(pActInfo, ok ? eO_Done : eO_Error, true); }
			};

			class CInfoNode final : public CNodeBase
			{
				enum EIn { eI_Get, eI_Handle, eI_EveryFrame };
				enum EOut { eO_State, eO_Time, eO_Duration, eO_Width, eO_Height, eO_HasAlpha, eO_Language, eO_Variant, eO_DecodeBackend, eO_FrameSerial, eO_SurfaceId, eO_Error };

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CInfoNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig_Void("Get", _HELP("Reads the values of the player and sends them to the outputs.")),
						InputPortConfig<int>("Handle", 0, _HELP("Player handle (from VideoAdvanced:Open).")),
						InputPortConfig<bool>("EveryFrame", false, _HELP("Also send the values every frame while the handle is valid.")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig<int>("State", _HELP("0 Stopped, 1 Playing, 2 Paused, 3 Ended, 4 Loading, 5 Error.")),
						OutputPortConfig<float>("Time", _HELP("Current playback time in seconds.")),
						OutputPortConfig<float>("Duration", _HELP("Duration in seconds, 0 until loaded.")),
						OutputPortConfig<int>("Width", _HELP("Picture width in pixels, 0 until loaded.")),
						OutputPortConfig<int>("Height", _HELP("Picture height in pixels, 0 until loaded.")),
						OutputPortConfig<bool>("HasAlpha", _HELP("The video has an alpha channel.")),
						OutputPortConfig<string>("Language", _HELP("Audio language in use, empty without languages.")),
						OutputPortConfig<int>("Variant", _HELP("Variant in use: 0 = main video, n = n-th variant.")),
						OutputPortConfig<int>("DecodeBackend", _HELP("Decoder in use: 0 = CPU.")),
						OutputPortConfig<string>("FrameSerial", _HELP("Number of uploaded pictures as decimal text.")),
						OutputPortConfig<string>("SurfaceId", _HELP("Surface id of the player, empty for Screen / Material players.")),
						OutputPortConfig_Void("Error", _HELP("Get was rejected (invalid handle); see the log.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("Reads the state, time, size, language, variant, decoder, frame serial and surface id of a player handle.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					switch (event)
					{
					case eFE_Initialize:
					case eFE_Activate:
						if (pActInfo->pGraph != nullptr && (event == eFE_Initialize || IsPortActive(pActInfo, eI_EveryFrame)))
							pActInfo->pGraph->SetRegularlyUpdated(pActInfo->myID, GetPortBool(pActInfo, eI_EveryFrame));
						if (event == eFE_Activate && IsPortActive(pActInfo, eI_Get))
						{
							const int handle = GetPortInt(pActInfo, eI_Handle);
							SValue state;
							if (!Call(E("State"), handle, nullptr, 0, &state)) // warns for an invalid handle
								ActivateOutput(pActInfo, eO_Error, true);
							else
								Send(pActInfo, handle);
						}
						break;
					case eFE_Update:
					{
						const int handle = GetPortInt(pActInfo, eI_Handle);
						if (HandleAlive(handle)) // every frame: no warning spam for an invalid handle
							Send(pActInfo, handle);
						break;
					}
					default:
						break;
					}
				}

			private:
				static void Send(SActivationInfo* pActInfo, int handle)
				{
					ActivateOutput(pActInfo, eO_State, Q("State", handle).i);
					ActivateOutput(pActInfo, eO_Time, Q("Time", handle).f);
					ActivateOutput(pActInfo, eO_Duration, Q("Duration", handle).f);
					ActivateOutput(pActInfo, eO_Width, Q("Width", handle).i);
					ActivateOutput(pActInfo, eO_Height, Q("Height", handle).i);
					ActivateOutput(pActInfo, eO_HasAlpha, Q("HasAlpha", handle).b);
					ActivateOutput(pActInfo, eO_Language, string(Q("Language", handle).s.c_str()));
					ActivateOutput(pActInfo, eO_Variant, Q("Variant", handle).i);
					ActivateOutput(pActInfo, eO_DecodeBackend, Q("DecodeBackend", handle).i);
					ActivateOutput(pActInfo, eO_FrameSerial, string(Q("FrameSerial", handle).s.c_str()));
					ActivateOutput(pActInfo, eO_SurfaceId, string(Q("SurfaceId", handle).s.c_str()));
				}
			};

			class CEventsNode final : public CNodeBase
			{
				enum EIn { eI_Handle };
				enum EOut
				{
					eO_Ready, eO_Started, eO_NewFrame, eO_NewFrameSerial, eO_Marker, eO_MarkerName, eO_MarkerTime,
					eO_Looped, eO_Ended, eO_Skipped, eO_Error, eO_ErrorCode, eO_ErrorMessage
				};

			public:
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CEventsNode(); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const SInputPortConfig in[] =
					{
						InputPortConfig<int>("Handle", 0, _HELP("Player handle to listen to (from VideoAdvanced:Open); 0 stops listening.")),
						{ 0 }
					};
					static const SOutputPortConfig out[] =
					{
						OutputPortConfig_Void("Ready", _HELP("The video is loaded: size, duration and texture are valid.")),
						OutputPortConfig_Void("Started", _HELP("Playback started.")),
						OutputPortConfig_Void("NewFrame", _HELP("A new picture was uploaded (at most once per frame); NewFrameSerial is set just before.")),
						OutputPortConfig<string>("NewFrameSerial", _HELP("Number of the uploaded picture as decimal text (never wraps).")),
						OutputPortConfig_Void("Marker", _HELP("The position reached a marker of the .cevvideo descriptor; MarkerName and MarkerTime are set just before.")),
						OutputPortConfig<string>("MarkerName", _HELP("Name of the marker.")),
						OutputPortConfig<float>("MarkerTime", _HELP("Media time of the marker in seconds.")),
						OutputPortConfig_Void("Looped", _HELP("A loop iteration completed and playback restarted at the beginning.")),
						OutputPortConfig_Void("Ended", _HELP("The end of a non-looping video was reached.")),
						OutputPortConfig_Void("Skipped", _HELP("A button press stopped the skippable video.")),
						OutputPortConfig_Void("Error", _HELP("Loading or playback failed; ErrorCode and ErrorMessage are set just before.")),
						OutputPortConfig<int>("ErrorCode", _HELP("Status code of the failure.")),
						OutputPortConfig<string>("ErrorMessage", _HELP("Short text of the failure.")),
						{ 0 }
					};
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = _HELP("The eight events of a player handle.");
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate || !IsPortActive(pActInfo, eI_Handle))
						return;
					m_graph.Set(pActInfo);
					Subscribe(GetPortInt(pActInfo, eI_Handle)); // 0 = stop listening; an invalid handle only warns
				}

			private:
				virtual void OnPlayerEvent(SActivationInfo* pInfo, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					switch (event)
					{
					case eEvent_Ready: ActivateOutput(pInfo, eO_Ready, true); break;
					case eEvent_Started: ActivateOutput(pInfo, eO_Started, true); break;
					case eEvent_NewFrame:
						ActivateOutput(pInfo, eO_NewFrameSerial, string(argCount > 0 ? args[0].s.c_str() : ""));
						ActivateOutput(pInfo, eO_NewFrame, true);
						break;
					case eEvent_Marker:
						ActivateOutput(pInfo, eO_MarkerName, string(argCount > 0 ? args[0].s.c_str() : ""));
						ActivateOutput(pInfo, eO_MarkerTime, argCount > 1 ? args[1].f : 0.f);
						ActivateOutput(pInfo, eO_Marker, true);
						break;
					case eEvent_Looped: ActivateOutput(pInfo, eO_Looped, true); break;
					case eEvent_Ended: ActivateOutput(pInfo, eO_Ended, true); break;
					case eEvent_Skipped: ActivateOutput(pInfo, eO_Skipped, true); break;
					case eEvent_Error: EmitError(pInfo, eO_Error, eO_ErrorCode, eO_ErrorMessage, argCount > 0 ? args[0].i : 0, argCount > 1 ? args[1].s.c_str() : ""); break;
					default: break;
					}
				}
			};

			// ------------------------------------------------------------------------------------------ registration
			template<class TNode>
			class CNodeFactory final : public CAutoRegFlowNodeBase
			{
			public:
				explicit CNodeFactory(const char* className) : CAutoRegFlowNodeBase(className) {}
				virtual IFlowNodePtr Create(IFlowNode::SActivationInfo*) override { return new TNode(); }
			};

			// The class names are what saved flow graphs store: never rename one.
			CNodeFactory<CPlayOnScreenNode> s_playOnScreen("Video:PlayOnScreen");
			CNodeFactory<CPlayOnMaterialNode> s_playOnMaterial("Video:PlayOnMaterial");
			CNodeFactory<CPlayToSurfaceNode> s_playToSurface("VideoUI:PlayToSurface");
			CNodeFactory<CSurfaceInfoNode> s_surfaceInfo("VideoUI:SurfaceInfo");
			CNodeFactory<CSurfaceEventsNode> s_surfaceEvents("VideoUI:SurfaceEvents");
			CNodeFactory<COpenNode> s_open("VideoAdvanced:Open");
			CNodeFactory<CTransportNode> s_transport("VideoAdvanced:Transport");
			CNodeFactory<CSettingsNode> s_settings("VideoAdvanced:Settings");
			CNodeFactory<CInfoNode> s_info("VideoAdvanced:Info");
			CNodeFactory<CEventsNode> s_events("VideoAdvanced:Events");

			const char* const kClassNames[] =
			{
				"Video:PlayOnScreen", "Video:PlayOnMaterial",
				"VideoUI:PlayToSurface", "VideoUI:SurfaceInfo", "VideoUI:SurfaceEvents",
				"VideoAdvanced:Open", "VideoAdvanced:Transport", "VideoAdvanced:Settings", "VideoAdvanced:Info", "VideoAdvanced:Events",
			};
		}

		uint32_t FlowNodeCount() { return static_cast<uint32_t>(sizeof(kClassNames) / sizeof(kClassNames[0])); }
		const char* FlowNodeClassName(uint32_t index) { return index < FlowNodeCount() ? kClassNames[index] : nullptr; }
	}
}
