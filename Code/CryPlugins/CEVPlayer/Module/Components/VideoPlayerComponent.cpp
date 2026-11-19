#include "StdAfx.h"
#include "PluginEnv.h"
#include "VideoPlayerComponent.h"

#include <vector>

#include <CrySchematyc/Utils/EnumFlags.h>
#include <CrySchematyc/Env/Elements/EnvComponent.h>
#include <CrySchematyc/Env/Elements/EnvFunction.h>
#include <CrySchematyc/Env/Elements/EnvSignal.h>
#include <CrySchematyc/Env/EnvElementBase.h>
#include <CrySchematyc/Runtime/RuntimeParamMap.h>
#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/IObject.h>

#include <Cry3DEngine/IMaterial.h>

namespace Schematyc
{
	inline void ReflectType(CTypeDesc<CEVPlayer::VideoFileName>& desc)
	{
		desc.SetGUID(CEVPlayer::Api::kVideoFileNameTypeGuid);
		desc.SetLabel("VideoFileName");
		desc.SetDescription("Video file name (.webm or .cevvideo)");
	}
}

namespace CEVPlayer
{
	// ------------------------------------------------------------------------------------------------ enums
	namespace Api
	{
	// (ADL: the function must live in the namespace of the enum)
	static void ReflectType(Schematyc::CTypeDesc<EOutputMode>& desc)
	{
		desc.SetGUID(Api::kOutputModeTypeGuid);
		desc.SetLabel("VideoOutputMode");
		desc.SetDescription("Where the picture of a video player goes");
		desc.SetDefaultValue(Api::EOutputMode::Screen);
		desc.AddConstant(Api::EOutputMode::Surface, "Surface", "Surface (for UIs)");
		desc.AddConstant(Api::EOutputMode::Screen, "Screen", "Screen");
		desc.AddConstant(Api::EOutputMode::Material, "Material", "Material slot");
	}

	static void ReflectType(Schematyc::CTypeDesc<EDecodeMode>& desc)
	{
		desc.SetGUID(Api::kDecodeModeTypeGuid);
		desc.SetLabel("VideoDecode");
		desc.SetDescription("Decoder choice of a video player (this build always decodes on the CPU)");
		desc.SetDefaultValue(Api::EDecodeMode::Auto);
		desc.AddConstant(Api::EDecodeMode::Auto, "Auto", "Auto");
		desc.AddConstant(Api::EDecodeMode::Gpu, "GPU", "GPU");
		desc.AddConstant(Api::EDecodeMode::Cpu, "CPU", "CPU");
	}
	}

	static void ReflectType(Schematyc::CTypeDesc<EVideoFit>& desc)
	{
		desc.SetGUID(Api::kFitTypeGuid);
		desc.SetLabel("VideoFit");
		desc.SetDescription("How a fullscreen video is placed on the screen");
		desc.SetDefaultValue(EVideoFit::Letterbox);
		desc.AddConstant(EVideoFit::Fill, "Fill", "Fill (stretch)");
		desc.AddConstant(EVideoFit::Fit, "Fit", "Fit (keep aspect, scene visible in the bars)");
		desc.AddConstant(EVideoFit::Letterbox, "Letterbox", "Letterbox (keep aspect, black bars)");
	}

	// ------------------------------------------------------------------------------------------------ signals
	namespace
	{
		const Api::SEntry& EventEntry(uint32_t event) { return Api::Events()[event]; }

		template<typename SIGNAL>
		void ReflectSignalBase(Schematyc::CTypeDesc<SIGNAL>& desc, uint32_t event)
		{
			const Api::SEntry& e = EventEntry(event);
			desc.SetGUID(e.guid);
			desc.SetLabel(e.name);
			desc.SetDescription(e.description);
		}
	}

	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SReadySignal>& desc) { ReflectSignalBase(desc, Api::eEvent_Ready); }
	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SStartedSignal>& desc) { ReflectSignalBase(desc, Api::eEvent_Started); }
	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SLoopedSignal>& desc) { ReflectSignalBase(desc, Api::eEvent_Looped); }
	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SEndedSignal>& desc) { ReflectSignalBase(desc, Api::eEvent_Ended); }
	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SSkippedSignal>& desc) { ReflectSignalBase(desc, Api::eEvent_Skipped); }

	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SNewFrameSignal>& desc)
	{
		ReflectSignalBase(desc, Api::eEvent_NewFrame);
		const Api::SParam& p = EventEntry(Api::eEvent_NewFrame).params[0];
		desc.AddMember(&CVideoPlayerComponent::SNewFrameSignal::serial, p.id, p.name, p.name, p.description, Schematyc::CSharedString());
	}

	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SMarkerSignal>& desc)
	{
		ReflectSignalBase(desc, Api::eEvent_Marker);
		const Api::SParam* p = EventEntry(Api::eEvent_Marker).params;
		desc.AddMember(&CVideoPlayerComponent::SMarkerSignal::name, p[0].id, p[0].name, p[0].name, p[0].description, Schematyc::CSharedString());
		desc.AddMember(&CVideoPlayerComponent::SMarkerSignal::time, p[1].id, p[1].name, p[1].name, p[1].description, 0.f);
	}

	static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent::SErrorSignal>& desc)
	{
		ReflectSignalBase(desc, Api::eEvent_Error);
		const Api::SParam* p = EventEntry(Api::eEvent_Error).params;
		desc.AddMember(&CVideoPlayerComponent::SErrorSignal::code, p[0].id, p[0].name, p[0].name, p[0].description, 0);
		desc.AddMember(&CVideoPlayerComponent::SErrorSignal::message, p[1].id, p[1].name, p[1].name, p[1].description, Schematyc::CSharedString());
	}

	// ------------------------------------------------------------------------------------------------ functions
	namespace
	{
		//! A Schematyc member function of CVideoPlayerComponent whose inputs / output come from a table entry.
		//! Execute reads the inputs into SValues and hands them to the component (Api::Call), nothing else.
		class CTableFunction final : public Schematyc::CEnvElementBase<Schematyc::IEnvFunction>
		{
		public:
			explicit CTableFunction(const Api::SEntry& entry)
				: CEnvElementBase(entry.guid, entry.name, SCHEMATYC_SOURCE_FILE_INFO)
				, m_entry(entry)
			{
				SetDescription(entry.description);
				for (uint32_t i = 0; i < entry.paramCount; ++i)
				{
					SPort port;
					port.id = entry.params[i].id; // fixed per parameter (ApiTable), not the position
					port.name = entry.params[i].name;
					port.description = entry.params[i].description;
					port.pData = MakeDefault(entry.params[i]);
					m_inputs.push_back(port);
				}
				if (entry.returnType != Api::EType::Void)
				{
					SPort port;
					port.id = kOutputBase;
					port.name = entry.returnName != nullptr ? entry.returnName : "Result";
					port.description = entry.description;
					port.pData = MakeDefaultOf(entry.returnType);
					m_outputs.push_back(port);
				}
			}

			// IEnvElement
			virtual bool IsValidScope(Schematyc::IEnvElement& scope) const override { return scope.GetType() == Schematyc::EEnvElementType::Component; }

			// IEnvFunction
			virtual bool Validate() const override { return true; }
			virtual Schematyc::EnvFunctionFlags GetFunctionFlags() const override
			{
				Schematyc::EnvFunctionFlags flags = Schematyc::EEnvFunctionFlags::Member;
				if (m_entry.kind == Api::EKind::Query)
					flags.Add(Schematyc::EEnvFunctionFlags::Const);
				return flags;
			}
			virtual const Schematyc::CCommonTypeDesc* GetObjectTypeDesc() const override { return &Schematyc::GetTypeDesc<CVideoPlayerComponent>(); }
			virtual uint32 GetInputCount() const override { return static_cast<uint32>(m_inputs.size()); }
			virtual uint32 GetInputId(uint32 i) const override { return i < m_inputs.size() ? m_inputs[i].id : Schematyc::InvalidId; }
			virtual const char* GetInputName(uint32 i) const override { return i < m_inputs.size() ? m_inputs[i].name : ""; }
			virtual const char* GetInputDescription(uint32 i) const override { return i < m_inputs.size() ? m_inputs[i].description : ""; }
			virtual Schematyc::CAnyConstPtr GetInputData(uint32 i) const override { return i < m_inputs.size() ? m_inputs[i].pData.get() : nullptr; }
			virtual uint32 GetOutputCount() const override { return static_cast<uint32>(m_outputs.size()); }
			virtual uint32 GetOutputId(uint32 i) const override { return i < m_outputs.size() ? m_outputs[i].id : Schematyc::InvalidId; }
			virtual const char* GetOutputName(uint32 i) const override { return i < m_outputs.size() ? m_outputs[i].name : ""; }
			virtual const char* GetOutputDescription(uint32 i) const override { return i < m_outputs.size() ? m_outputs[i].description : ""; }
			virtual Schematyc::CAnyConstPtr GetOutputData(uint32 i) const override { return i < m_outputs.size() ? m_outputs[i].pData.get() : nullptr; }

			virtual void Execute(Schematyc::CRuntimeParamMap& params, void* pObject) const override
			{
				CVideoPlayerComponent* const pComponent = static_cast<CVideoPlayerComponent*>(pObject);
				if (pComponent == nullptr)
					return;

				Api::SValue in[Api::kMaxParams];
				for (uint32_t i = 0; i < m_inputs.size() && i < Api::kMaxParams; ++i)
				{
					const Schematyc::CAnyConstPtr pInput = params.GetInput(Schematyc::CUniqueId::FromUInt32(m_inputs[i].id));
					if (pInput)
						in[i] = Read(*pInput, m_entry.params[i].type);
				}

				Api::SValue result;
				pComponent->CallApi(m_entry, in, m_entry.paramCount, &result);

				if (!m_outputs.empty())
				{
					const Schematyc::CAnyPtr pOutput = params.GetOutput(Schematyc::CUniqueId::FromUInt32(m_outputs[0].id));
					if (pOutput)
						Write(*pOutput, result, m_entry.returnType);
				}
			}

		private:
			static constexpr uint32 kOutputBase = 'out0';

			struct SPort
			{
				uint32 id = 0;
				const char* name = "";
				const char* description = "";
				Schematyc::CAnyValuePtr pData;
			};

			static Schematyc::CAnyValuePtr MakeDefaultOf(Api::EType type)
			{
				switch (type)
				{
				case Api::EType::Bool: return Schematyc::CAnyValue::MakeShared(false);
				case Api::EType::Int: return Schematyc::CAnyValue::MakeShared(int32(0));
				case Api::EType::Float: return Schematyc::CAnyValue::MakeShared(0.f);
				default: return Schematyc::CAnyValue::MakeShared(Schematyc::CSharedString());
				}
			}

			static Schematyc::CAnyValuePtr MakeDefault(const Api::SParam& p)
			{
				switch (p.type)
				{
				case Api::EType::Bool: return Schematyc::CAnyValue::MakeShared(p.defNum != 0.0);
				case Api::EType::Int: return Schematyc::CAnyValue::MakeShared(static_cast<int32>(p.defNum));
				case Api::EType::Float: return Schematyc::CAnyValue::MakeShared(static_cast<float>(p.defNum));
				default: return Schematyc::CAnyValue::MakeShared(Schematyc::CSharedString(p.defStr != nullptr ? p.defStr : ""));
				}
			}

			static Api::SValue Read(const Schematyc::CAnyConstRef& any, Api::EType type)
			{
				switch (type)
				{
				case Api::EType::Bool: return Api::SValue::FromBool(Schematyc::DynamicCast<bool>(any));
				case Api::EType::Int: return Api::SValue::FromInt(Schematyc::DynamicCast<int32>(any));
				case Api::EType::Float: return Api::SValue::FromFloat(Schematyc::DynamicCast<float>(any));
				default: return Api::SValue::FromString(Schematyc::DynamicCast<Schematyc::CSharedString>(any).c_str());
				}
			}

			static void Write(const Schematyc::CAnyRef& any, const Api::SValue& v, Api::EType type)
			{
				switch (type)
				{
				case Api::EType::Bool: Schematyc::DynamicCast<bool>(any) = v.b; break;
				case Api::EType::Int: Schematyc::DynamicCast<int32>(any) = static_cast<int32>(v.i); break;
				case Api::EType::Float: Schematyc::DynamicCast<float>(any) = v.f; break;
				default: Schematyc::DynamicCast<Schematyc::CSharedString>(any) = Schematyc::CSharedString(v.s.c_str()); break;
				}
			}

			const Api::SEntry& m_entry;
			std::vector<SPort> m_inputs;
			std::vector<SPort> m_outputs;
		};

		void RegisterFunctions(Schematyc::CEnvRegistrationScope& scope, Api::SSpan span)
		{
			for (const Api::SEntry& e : span)
			{
				if ((e.flags & Api::eEntry_Standalone) != 0)
					continue; // Create / Destroy: the component owns its player
				scope.Register(std::make_shared<CTableFunction>(e));
			}
		}
	}

	void CVideoPlayerComponent::Register(Schematyc::CEnvRegistrationScope& componentScope)
	{
		RegisterFunctions(componentScope, Api::Controls());
		RegisterFunctions(componentScope, Api::Queries());

		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SReadySignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SStartedSignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SNewFrameSignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SMarkerSignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SLoopedSignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SEndedSignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SSkippedSignal));
		componentScope.Register(SCHEMATYC_MAKE_ENV_SIGNAL(CVideoPlayerComponent::SErrorSignal));
	}

	// ------------------------------------------------------------------------------------------------ component
	void CVideoPlayerComponent::ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent>& desc)
	{
		desc.SetGUID(Api::kSchematycComponentGuid);
		desc.SetEditorCategory("Video");
		desc.SetLabel("Video Player");
		desc.SetDescription("Plays a .webm video or a .cevvideo descriptor into a surface for UIs, to the screen or to a material slot");
		desc.SetIcon("icons:General/Camera.ico");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Attach, IEntityComponent::EFlags::ClientOnly });

		desc.AddMember(&CVideoPlayerComponent::m_video, 'vide', "video", "Video", "Video file (.webm) or runtime descriptor (.cevvideo)", VideoFileName(""));
		desc.AddMember(&CVideoPlayerComponent::m_outputMode, 'omod', "outputmode", "Output", "Surface = a named surface UIs show by its SurfaceID, Screen = fullscreen on the top layer, Material = texture slot of a material", Api::EOutputMode::Screen);
		desc.AddMember(&CVideoPlayerComponent::m_surfaceId, 'surf', "surfaceid", "SurfaceID", "Output Surface: the surface id UIs use (1 to 63 of A-Z a-z 0-9 _ . : -, unique)", Schematyc::CSharedString());
		desc.AddMember(&CVideoPlayerComponent::m_material, 'mtl_', "material", "Material", "Output Material: material name (empty = the material of this entity)", Schematyc::CSharedString());
		desc.AddMember(&CVideoPlayerComponent::m_slot, 'slot', "slot", "Slot", "Output Material: texture slot of the material (0 = diffuse)", 0);
		desc.AddMember(&CVideoPlayerComponent::m_fit, 'fit_', "fit", "Screen Fit", "Screen mode: how the picture is placed", EVideoFit::Letterbox);
		desc.AddMember(&CVideoPlayerComponent::m_language, 'lang', "language", "Language", "Audio language of the .cevvideo descriptor (empty = default choice)", Schematyc::CSharedString());
		desc.AddMember(&CVideoPlayerComponent::m_loop, 'loop', "loop", "Loop", "Restart at 0 when the end is reached", false);
		desc.AddMember(&CVideoPlayerComponent::m_autoplay, 'auto', "autoplay", "Autoplay", "Load and play when the game starts", false);
		desc.AddMember(&CVideoPlayerComponent::m_skippable, 'skip', "skippable", "Skippable", "A key / mouse / gamepad button press stops the video and sends Skipped", false);
		desc.AddMember(&CVideoPlayerComponent::m_decode, 'deco', "decode", "Decode", "Decoder: Auto, GPU or CPU (this build always decodes on the CPU; GPU logs a warning)", Api::EDecodeMode::Auto);
	}

	CVideoPlayerComponent::~CVideoPlayerComponent()
	{
		DestroyPlayer();
	}

	void CVideoPlayerComponent::Initialize()
	{
	}

	void CVideoPlayerComponent::OnShutDown()
	{
		DestroyPlayer();
	}

	Cry::Entity::EventFlags CVideoPlayerComponent::GetEventMask() const
	{
		return EEntityEvent::GameplayStarted | EEntityEvent::Reset;
	}

	void CVideoPlayerComponent::ProcessEvent(const SEntityEvent& event)
	{
		switch (event.event)
		{
		case EEntityEvent::GameplayStarted:
			StartFromProperties();
			break;
		case EEntityEvent::Reset:
			if (event.nParam[0] == 0) // the editor leaves game mode
				DestroyPlayer();
			break;
		default:
			break;
		}
	}

	IMaterial* CVideoPlayerComponent::EntityMaterial() const
	{
		if (m_pEntity == nullptr)
			return nullptr;
		IMaterial* pMaterial = m_pEntity->GetMaterial();
		if (pMaterial == nullptr)
			pMaterial = m_pEntity->GetRenderMaterial(-1);
		return pMaterial;
	}

	bool CVideoPlayerComponent::EnsurePlayer()
	{
		if (mEnv == nullptr || mEnv->pApi == nullptr)
			return false;
		if (m_handle != 0 && mEnv->pApi->Find(m_handle, false) != nullptr)
			return true;

		m_handle = m_outputMode == Api::EOutputMode::Surface ? mEnv->pApi->CreateForSurface(m_surfaceId.c_str(), true) : mEnv->pApi->Create(true);
		if (m_handle == 0)
		{
			LogWarning("<CVideoPlayerComponent> no player: %s", mEnv->pApi->LastErrorMessage());
			return false;
		}
		mEnv->pApi->Subscribe(m_handle, this);
		ApplyProperties();
		return true;
	}

	void CVideoPlayerComponent::DestroyPlayer()
	{
		if (m_handle != 0 && mEnv != nullptr && mEnv->pApi != nullptr)
		{
			mEnv->pApi->Unsubscribe(m_handle, this);
			if (mEnv->pApi->Find(m_handle, false) != nullptr)
				mEnv->pApi->Destroy(m_handle);
		}
		m_handle = 0;
	}

	void CVideoPlayerComponent::ApplyProperties()
	{
		using namespace Api;
		const SValue loop[1] = { SValue::FromBool(m_loop) };
		const SValue language[1] = { SValue::FromString(m_language.c_str()) };
		const SValue skippable[1] = { SValue::FromBool(m_skippable) };
		const SValue decode[1] = { SValue::FromInt(static_cast<int>(m_decode)) };
		const SValue output[5] =
		{
			SValue::FromInt(static_cast<int>(m_outputMode)), SValue::FromString(m_material.c_str()), SValue::FromInt(m_slot),
			SValue::FromInt(-1), SValue::FromInt(static_cast<int>(m_fit))
		};
		Call(*FindByName("SetDecode"), m_handle, decode, 1, nullptr);
		Call(*FindByName("SetLoop"), m_handle, loop, 1, nullptr);
		Call(*FindByName("SetLanguage"), m_handle, language, 1, nullptr);
		Call(*FindByName("SetSkippable"), m_handle, skippable, 1, nullptr);
		Call(*FindByName("SetOutput"), m_handle, output, 5, nullptr, EntityMaterial());
	}

	void CVideoPlayerComponent::StartFromProperties()
	{
		if (gEnv != nullptr && gEnv->IsEditor() && !gEnv->IsEditorGameMode())
			return;
		DestroyPlayer(); // fresh start for every game start
		if (!EnsurePlayer())
			return;
		if (m_video.value.empty())
		{
			if (m_autoplay)
				LogWarning("<CVideoPlayerComponent> autoplay is set but the Video property is empty");
			return;
		}
		const Api::SValue path[1] = { Api::SValue::FromString(m_video.value.c_str()) };
		if (!Api::Call(*Api::FindByName("Load"), m_handle, path, 1, nullptr))
			return;
		if (m_autoplay)
			Api::Call(*Api::FindByName("Play"), m_handle, nullptr, 0, nullptr);
	}

	bool CVideoPlayerComponent::CallApi(const Api::SEntry& entry, const Api::SValue* in, uint32_t inCount, Api::SValue* pResult)
	{
		if (entry.NeedsPlayer() && !EnsurePlayer())
			return false;
		return Api::Call(entry, m_handle, in, inCount, pResult, EntityMaterial());
	}

	void CVideoPlayerComponent::OnApiEvent(int handle, uint32_t event, const Api::SValue* args, uint32_t argCount)
	{
		if (handle != m_handle || m_pEntity == nullptr || (gEnv->pSystem != nullptr && gEnv->pSystem->IsQuitting()))
			return;
		Schematyc::IObject* const pObject = m_pEntity->GetSchematycObject();
		if (pObject == nullptr)
			return;

		switch (event)
		{
		case Api::eEvent_Ready: pObject->ProcessSignal(SReadySignal(), GetGUID()); break;
		case Api::eEvent_Started: pObject->ProcessSignal(SStartedSignal(), GetGUID()); break;
		case Api::eEvent_Looped: pObject->ProcessSignal(SLoopedSignal(), GetGUID()); break;
		case Api::eEvent_Ended: pObject->ProcessSignal(SEndedSignal(), GetGUID()); break;
		case Api::eEvent_Skipped: pObject->ProcessSignal(SSkippedSignal(), GetGUID()); break;
		case Api::eEvent_NewFrame:
		{
			SNewFrameSignal signal;
			if (argCount > 0)
				signal.serial = Schematyc::CSharedString(args[0].s.c_str());
			pObject->ProcessSignal(signal, GetGUID());
			break;
		}
		case Api::eEvent_Marker:
		{
			SMarkerSignal signal;
			if (argCount > 1)
			{
				signal.name = Schematyc::CSharedString(args[0].s.c_str());
				signal.time = args[1].f;
			}
			pObject->ProcessSignal(signal, GetGUID());
			break;
		}
		case Api::eEvent_Error:
		{
			SErrorSignal signal;
			if (argCount > 1)
			{
				signal.code = args[0].i;
				signal.message = Schematyc::CSharedString(args[1].s.c_str());
			}
			pObject->ProcessSignal(signal, GetGUID());
			break;
		}
		default:
			break;
		}
	}
}
