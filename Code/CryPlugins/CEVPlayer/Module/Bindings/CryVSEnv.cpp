// CryVS bindings: the same component / functions / signals as the Schematyc side, generated from the API table.
// Compiled only when CMake found the CryVS headers (CEV_HAVE_CRYVS=1, community tree).
//
// Registration (docs/research/01-open-questions.md, Q5): CryVS has no "register your env" system event and clears its
// whole registry on every CFramework::RefreshEnv(), so we register once at ESYSTEM_EVENT_GAME_POST_INIT (the first
// moment gEnv->pCryVS is not null) and again from the envRefresh signal.
//
// Not done: asynchronous actions. IAction has no completion report (Init / Start / Stop / Shutdown only), so only
// synchronous member functions and signals exist; Load is "synchronous" (it only starts the load) and its result
// arrives as the Ready / Error signals.
#include "StdAfx.h"
#include "PluginEnv.h"
#include "CryVSEnv.h"

#include <CryVS/IFramework.h>
#include <CryVS/Env/IEnvRegistry.h>
#include <CryVS/ComponentFactory.h>
#include <CryVS/IObject.h>
#include <CryVS/ISignal.h>
#include <CryVS/System/ComponentMemberFunction.h>
#include <CryVS/System/Stack.h>
#include <CrySerialization/Enum.h>
#include <CrySerialization/STL.h>
#include <CrySerialization/Decorators/ResourceFilePath.h>
#include <Cry3DEngine/IMaterial.h>
#include <CryEntitySystem/IEntitySystem.h>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"

SERIALIZATION_ENUM_BEGIN_NESTED2(CEVPlayer, Api, EOutputMode, "Output Mode")
	SERIALIZATION_ENUM(CEVPlayer::Api::EOutputMode::Screen, "Screen", "Screen")
	SERIALIZATION_ENUM(CEVPlayer::Api::EOutputMode::Material, "Material", "Material slot")
	SERIALIZATION_ENUM(CEVPlayer::Api::EOutputMode::Texture, "Texture", "Named texture")
SERIALIZATION_ENUM_END()

SERIALIZATION_ENUM_BEGIN_NESTED(CEVPlayer, EVideoFit, "Screen Fit")
	SERIALIZATION_ENUM(CEVPlayer::EVideoFit::Fill, "Fill", "Fill (stretch)")
	SERIALIZATION_ENUM(CEVPlayer::EVideoFit::Fit, "Fit", "Fit (keep aspect)")
	SERIALIZATION_ENUM(CEVPlayer::EVideoFit::Letterbox, "Letterbox", "Letterbox (black bars)")
SERIALIZATION_ENUM_END()

namespace CEVPlayer
{
	namespace CryVSBind
	{
		namespace
		{
			// ---------------------------------------------------------------------------------------- properties
			struct SProperties
			{
				string video;
				Api::EOutputMode outputMode = Api::EOutputMode::Screen;
				string name;                       // Texture: texture name; Material: material name (empty = entity material)
				int32 slot = 0;
				EVideoFit fit = EVideoFit::Letterbox;
				string language;
				bool loop = false;
				bool autoplay = false;
				bool skippable = false;

				void Serialize(Serialization::IArchive& archive)
				{
					archive(Serialization::ResourceFilePath(video, "Video files (*.webm;*.cevvideo)|*.webm;*.cevvideo", "Videos"), "video", "Video");
					archive(outputMode, "outputMode", "Output Mode");
					archive(name, "name", "Name");
					archive(slot, "slot", "Slot Index");
					archive(fit, "fit", "Screen Fit");
					archive(language, "language", "Language");
					archive(loop, "loop", "Loop");
					archive(autoplay, "autoplay", "Autoplay");
					archive(skippable, "skippable", "Skippable");
				}
			};

			// ---------------------------------------------------------------------------------------- component
			class CVideoComponent final : public CryVS::IComponent, public Api::IEventSink
			{
			public:
				// CryVS::IComponent
				virtual bool Init(const CryVS::SComponentParams& params) override
				{
					m_pObject = &params.object;
					m_entityId = params.object.GetEntityId().GetValue();
					if (const SProperties* pProps = params.properties.ToPtr<SProperties>())
						m_props = *pProps;
					return true;
				}

				virtual void Run(CryVS::ESimulationMode simulationMode) override
				{
					if (simulationMode == CryVS::ESimulationMode::Game)
						Start();
				}

				virtual void Shutdown() override { Destroy(); m_pObject = nullptr; }
				virtual CryVS::INetworkSpawnParamsPtr GetNetworkSpawnParams() const override { return CryVS::INetworkSpawnParamsPtr(); }
				// ~CryVS::IComponent

				~CVideoComponent() { Destroy(); }

				bool CallApi(const Api::SEntry& entry, const Api::SValue* in, uint32_t inCount, Api::SValue* pResult)
				{
					if (entry.NeedsPlayer() && !EnsurePlayer())
						return false;
					return Api::Call(entry, m_handle, in, inCount, pResult, EntityMaterial());
				}

				// Api::IEventSink
				virtual void OnApiEvent(int handle, uint32_t event, const Api::SValue* args, uint32_t argCount) override
				{
					if (handle != m_handle || m_pObject == nullptr || event >= Api::eEvent_Count)
						return;
					const Api::SEntry& e = Api::Events()[event];
					CryVS::CInPlaceStack stack;
					for (uint32_t i = 0; i < e.paramCount && i < argCount; ++i)
					{
						switch (e.params[i].type)
						{
						case Api::EType::Bool: stack.Push(CryVS::CVariant(args[i].b)); break;
						case Api::EType::Int: stack.Push(CryVS::CVariant(static_cast<int32>(args[i].i))); break;
						case Api::EType::Float: stack.Push(CryVS::CVariant(args[i].f)); break;
						default: stack.Push(CryVS::CVariant(args[i].s.c_str())); break;
						}
					}
					m_pObject->ProcessSignal(e.guid, stack);
				}

			private:
				IMaterial* EntityMaterial() const
				{
					IEntity* const pEntity = gEnv->pEntitySystem != nullptr ? gEnv->pEntitySystem->GetEntity(m_entityId) : nullptr;
					return pEntity != nullptr ? pEntity->GetMaterial() : nullptr;
				}

				bool EnsurePlayer()
				{
					if (mEnv == nullptr || mEnv->pApi == nullptr)
						return false;
					if (m_handle != 0 && mEnv->pApi->Find(m_handle, false) != nullptr)
						return true;
					m_handle = mEnv->pApi->Create(m_props.outputMode == Api::EOutputMode::Texture ? m_props.name.c_str() : nullptr, true);
					if (m_handle == 0)
						return false;
					mEnv->pApi->Subscribe(m_handle, this);
					using namespace Api;
					const SValue loop[1] = { SValue::FromBool(m_props.loop) };
					const SValue language[1] = { SValue::FromString(m_props.language.c_str()) };
					const SValue skippable[1] = { SValue::FromBool(m_props.skippable) };
					const SValue output[5] = { SValue::FromInt(static_cast<int>(m_props.outputMode)), SValue::FromString(m_props.name.c_str()),
						SValue::FromInt(m_props.slot), SValue::FromInt(-1), SValue::FromInt(static_cast<int>(m_props.fit)) };
					Call(*FindByName("SetLoop"), m_handle, loop, 1, nullptr);
					Call(*FindByName("SetLanguage"), m_handle, language, 1, nullptr);
					Call(*FindByName("SetSkippable"), m_handle, skippable, 1, nullptr);
					Call(*FindByName("SetOutput"), m_handle, output, 5, nullptr, EntityMaterial());
					return true;
				}

				void Start()
				{
					Destroy();
					if (!EnsurePlayer() || m_props.video.empty())
						return;
					const Api::SValue path[1] = { Api::SValue::FromString(m_props.video.c_str()) };
					if (Api::Call(*Api::FindByName("Load"), m_handle, path, 1, nullptr) && m_props.autoplay)
						Api::Call(*Api::FindByName("Play"), m_handle, nullptr, 0, nullptr);
				}

				void Destroy()
				{
					if (m_handle != 0 && mEnv != nullptr && mEnv->pApi != nullptr)
					{
						mEnv->pApi->Unsubscribe(m_handle, this);
						if (mEnv->pApi->Find(m_handle, false) != nullptr)
							mEnv->pApi->Destroy(m_handle);
					}
					m_handle = 0;
				}

				CryVS::IObject* m_pObject = nullptr;
				EntityId m_entityId = 0;
				SProperties m_props;
				int m_handle = 0;
			};

			// ---------------------------------------------------------------------------------------- functions
			//! A component member function whose parameters come from a table entry; Call forwards to the component.
			class CTableFunction final : public CryVS::ComponentMemberFunctionUtils::CBase
			{
			public:
				CTableFunction(const Api::SEntry& entry, const ::CryGUID& componentGuid)
					: CBase(entry.guid, componentGuid, entry.name, __FILE__, "Code")
					, m_entry(entry)
					, m_hasReturn(entry.returnType != Api::EType::Void)
				{
					if (m_hasReturn)
						AddParamOf(entry.returnType, CryVS::ComponentMemberFunctionUtils::EParamFlags::IsReturn);
					for (uint32_t i = 0; i < entry.paramCount; ++i)
						AddParamOf(entry.params[i].type, CryVS::ComponentMemberFunctionUtils::EParamFlags::None);

					CryVS::IComponentMemberFunction& self = *this; // the typed BindInput overloads live on the interface
					self.SetAuthor("CEVPlayer");
					self.SetDescription(entry.description);
					if (m_hasReturn)
						self.BindOutput(0, entry.returnName != nullptr ? entry.returnName : "Result", entry.description);
					for (uint32_t i = 0; i < entry.paramCount; ++i)
					{
						const Api::SParam& p = entry.params[i];
						const uint32 idx = i + (m_hasReturn ? 1u : 0u);
						switch (p.type)
						{
						case Api::EType::Bool: self.BindInput(idx, p.name, p.description, p.defNum != 0.0); break;
						case Api::EType::Int: self.BindInput(idx, p.name, p.description, static_cast<int32>(p.defNum)); break;
						case Api::EType::Float: self.BindInput(idx, p.name, p.description, static_cast<float>(p.defNum)); break;
						default: self.BindInput(idx, p.name, p.description, p.defStr != nullptr ? p.defStr : ""); break;
						}
					}
				}

				virtual void Call(CryVS::IComponent& component, const CryVS::TVariantConstArray& inputs, const CryVS::TVariantArray& outputs) const override
				{
					CryVS::CVariantArrayInputArchive inputsArchive(inputs);
					CryVS::CVariantArrayOutputArchive outputsArchive(outputs);

					const uint32 base = m_hasReturn ? 1u : 0u;
					Api::SValue in[Api::kMaxParams];
					for (uint32_t i = 0; i < m_entry.paramCount && i < Api::kMaxParams; ++i)
					{
						switch (m_entry.params[i].type)
						{
						case Api::EType::Bool: { bool v = false; LoadParam(inputsArchive, base + i, v); in[i] = Api::SValue::FromBool(v); break; }
						case Api::EType::Int: { int32 v = 0; LoadParam(inputsArchive, base + i, v); in[i] = Api::SValue::FromInt(v); break; }
						case Api::EType::Float: { float v = 0.f; LoadParam(inputsArchive, base + i, v); in[i] = Api::SValue::FromFloat(v); break; }
						default: { const char* v = ""; LoadParam(inputsArchive, base + i, v); in[i] = Api::SValue::FromString(v); break; }
						}
					}

					Api::SValue result;
					static_cast<CVideoComponent&>(component).CallApi(m_entry, in, m_entry.paramCount, &result);

					if (m_hasReturn)
					{
						switch (m_entry.returnType)
						{
						case Api::EType::Bool: { bool v = result.b; StoreParam(outputsArchive, 0, v); break; }
						case Api::EType::Int: { int32 v = result.i; StoreParam(outputsArchive, 0, v); break; }
						case Api::EType::Float: { float v = result.f; StoreParam(outputsArchive, 0, v); break; }
						default: { const char* v = result.s.c_str(); StoreParam(outputsArchive, 0, v); break; }
						}
					}
				}

			private:
				void AddParamOf(Api::EType type, CryVS::ComponentMemberFunctionUtils::EParamFlags flags)
				{
					switch (type)
					{
					case Api::EType::Bool: AddParam<bool>(flags); break;
					case Api::EType::Int: AddParam<int32>(flags); break;
					case Api::EType::Float: AddParam<float>(flags); break;
					default: AddParam<const char*>(flags); break;
					}
				}

				const Api::SEntry& m_entry;
				bool m_hasReturn;
			};

			// ---------------------------------------------------------------------------------------- registration
			CryVS::IFramework* s_pFramework = nullptr;
			TemplateUtils::CConnectionScope s_connectionScope;
			bool s_connected = false;
			bool s_shutdown = false;

			void RegisterAll()
			{
				if (s_shutdown || gEnv == nullptr || gEnv->pCryVS == nullptr)
					return;
				CryVS::IEnvRegistry& registry = gEnv->pCryVS->GetEnvRegistry();
				const ::CryGUID componentGuid = Api::kCryVSComponentGuid;
				if (registry.GetComponentFactory(componentGuid))
					return; // already there (the post-init registration and a refresh may meet)

				CryVS::IComponentFactoryPtr pFactory = CRYVS_MAKE_COMPONENT_FACTORY_SHARED(CVideoComponent, SProperties, componentGuid);
				pFactory->SetName("VideoPlayer");
				pFactory->SetNamespace("CEVPlayer");
				pFactory->SetAuthor("CEVPlayer");
				pFactory->SetDescription("Plays a .webm video or .cevvideo descriptor to the screen, a material slot or a named texture");
				registry.RegisterComponentFactory(pFactory);

				uint32_t functions = 0;
				for (const Api::SSpan span : { Api::Controls(), Api::Queries() })
				{
					for (const Api::SEntry& e : span)
					{
						if ((e.flags & Api::eEntry_Standalone) != 0)
							continue; // Create / Destroy: the component owns its player
						registry.RegisterComponentMemberFunction(CryVS::IComponentMemberFunctionPtr(std::make_shared<CTableFunction>(e, componentGuid)));
						++functions;
					}
				}

				uint32_t signals = 0;
				for (const Api::SEntry& e : Api::Events())
				{
					CryVS::ISignalPtr pSignal = registry.CreateSignal(e.guid);
					if (!pSignal)
						continue;
					pSignal->SetSenderGUID(componentGuid);
					pSignal->SetName(e.name);
					pSignal->SetFileName(__FILE__, "Code");
					pSignal->SetAuthor("CEVPlayer");
					pSignal->SetDescription(e.description);
					for (uint32_t i = 0; i < e.paramCount; ++i)
					{
						const Api::SParam& p = e.params[i];
						switch (p.type)
						{
						case Api::EType::Bool: pSignal->AddInput(p.name, p.description, false); break;
						case Api::EType::Int: pSignal->AddInput(p.name, p.description, int32(0)); break;
						case Api::EType::Float: pSignal->AddInput(p.name, p.description, 0.f); break;
						default: pSignal->AddInput(p.name, p.description, ""); break;
						}
					}
					++signals;
				}
				LogAlways("CryVS: registered component 'CEVPlayer::VideoPlayer' (%u functions, %u signals)", functions, signals);
			}

			void OnEnvRefresh()
			{
				RegisterAll();
			}
		}

		void Initialize()
		{
			if (gEnv == nullptr || gEnv->pCryVS == nullptr)
			{
				LogDebug("CryVS: gEnv->pCryVS is null, CryVS bindings skipped");
				return;
			}
			s_shutdown = false;
			s_pFramework = gEnv->pCryVS;
			RegisterAll();
			if (!s_connected)
			{
				gEnv->pCryVS->Signals().envRefresh.Connect(CryVS::EnvRefreshSignal::Delegate::FromGlobalFunction<&OnEnvRefresh>(), s_connectionScope);
				s_connected = true;
			}
		}

		void Shutdown()
		{
			s_shutdown = true;
			if (s_connected)
			{
				s_connectionScope.Release();
				s_connected = false;
			}
			s_pFramework = nullptr;
		}
	}
}
