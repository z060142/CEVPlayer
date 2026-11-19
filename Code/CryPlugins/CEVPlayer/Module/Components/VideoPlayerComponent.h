// Schematyc entity component "Video Player": one player per entity, configured by properties, driven by functions
// and signals that are generated from the API table (Bindings/ApiTable.h). Output Surface makes a surface player
// (UIs show it by its SurfaceID), Screen plays fullscreen, Material binds the texture to a material slot.
#pragma once

#include <string>

#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>
#include <CrySchematyc/ResourceTypes.h>
#include <CrySchematyc/Utils/SharedString.h>
#include <CryEntitySystem/IEntityComponent.h>
#include <CryEntitySystem/IEntity.h>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"

namespace CEVPlayer
{
	//! File picker for the video property: .webm videos and .cevvideo descriptors.
	inline Serialization::ResourceFilePath VideoFilePicker(string& path)
	{
		return Serialization::ResourceFilePath(path, "Video files (*.webm;*.cevvideo)|*.webm;*.cevvideo|WebM video (*.webm)|*.webm|CEVPlayer descriptor (*.cevvideo)|*.cevvideo", "Videos");
	}
	typedef Schematyc::SerializationUtils::SResourceNameSerializer<&VideoFilePicker> VideoFileName;

	class CVideoPlayerComponent final
		: public IEntityComponent
		, public Api::IEventSink
	{
	public:
		CVideoPlayerComponent() = default;
		virtual ~CVideoPlayerComponent();
		static void ReflectType(Schematyc::CTypeDesc<CVideoPlayerComponent>& desc);
		//! Registers the functions and signals generated from the table (called with the component's scope).
		static void Register(Schematyc::CEnvRegistrationScope& componentScope);

		// IEntityComponent
		virtual void Initialize() override;
		virtual void OnShutDown() override;
		virtual Cry::Entity::EventFlags GetEventMask() const override;
		virtual void ProcessEvent(const SEntityEvent& event) override;
		// ~IEntityComponent

		// Api::IEventSink
		virtual void OnApiEvent(int handle, uint32_t event, const Api::SValue* args, uint32_t argCount) override;

		//! What every generated function does: makes sure the player exists, then runs the table entry on it.
		bool CallApi(const Api::SEntry& entry, const Api::SValue* in, uint32_t inCount, Api::SValue* pResult);

		// signals (one type per table event, GUIDs and payload names come from the table)
		struct SReadySignal { SReadySignal() = default; };
		struct SStartedSignal { SStartedSignal() = default; };
		struct SNewFrameSignal { SNewFrameSignal() = default; Schematyc::CSharedString serial; };
		struct SMarkerSignal { SMarkerSignal() = default; Schematyc::CSharedString name; float time = 0.f; };
		struct SLoopedSignal { SLoopedSignal() = default; };
		struct SEndedSignal { SEndedSignal() = default; };
		struct SSkippedSignal { SSkippedSignal() = default; };
		struct SErrorSignal { SErrorSignal() = default; int32 code = 0; Schematyc::CSharedString message; };

		// properties
		VideoFileName            m_video;
		Api::EOutputMode         m_outputMode = Api::EOutputMode::Screen;
		Schematyc::CSharedString m_surfaceId;  // Surface: the surface id UIs use
		Schematyc::CSharedString m_material;   // Material: material name (empty = this entity's material)
		int32                    m_slot = 0;   // Material: texture slot (0 = diffuse)
		EVideoFit                m_fit = EVideoFit::Letterbox; // Screen
		Schematyc::CSharedString m_audioTrack;
		bool                     m_loop = false;
		bool                     m_autoplay = false;
		bool                     m_skippable = false;
		Api::EDecodeMode         m_decode = Api::EDecodeMode::Auto;
		bool                     m_pauseDecodeWhenHidden = true;

	private:
		bool EnsurePlayer();
		void DestroyPlayer();
		void ApplyProperties();
		void StartFromProperties();
		IMaterial* EntityMaterial() const;

		int m_handle = 0;
	};
}
