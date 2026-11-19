#include "StdAfx.h"
#include "PluginEnv.h"
#include "FlowTest.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <CryFlowGraph/IFlowBaseNode.h>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"
#include "CEVCore/Status.h"
#include "Contract/TexSource.h"
#include "Player/PlayerManager.h"

namespace CEVPlayer
{
	namespace Api
	{
		namespace
		{
			const char* const kProbeType = "CEVPlayerTest:Probe";
			constexpr int kProbePorts = 16;

			//! What a probe saw on its inputs: activation count and last value (as text) per port.
			struct SProbeData
			{
				int count[kProbePorts] = {};
				std::string last[kProbePorts];
			};

			SProbeData* s_pNextProbeData = nullptr; // handed to the probe node made by the next CreateNode

			class CProbeNode final : public CFlowBaseNode<eNCT_Instanced>
			{
			public:
				explicit CProbeNode(SProbeData* pData) : m_pData(pData) {}
				virtual IFlowNodePtr Clone(SActivationInfo*) override { return new CProbeNode(m_pData); }
				virtual void GetMemoryUsage(ICrySizer* pSizer) const override { pSizer->Add(*this); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					static const char* const inNames[kProbePorts] = { "i0", "i1", "i2", "i3", "i4", "i5", "i6", "i7", "i8", "i9", "i10", "i11", "i12", "i13", "i14", "i15" };
					static const char* const outNames[kProbePorts] = { "o0", "o1", "o2", "o3", "o4", "o5", "o6", "o7", "o8", "o9", "o10", "o11", "o12", "o13", "o14", "o15" };
					static SInputPortConfig in[kProbePorts + 1];
					static SOutputPortConfig out[kProbePorts + 1];
					static bool built = false;
					if (!built)
					{
						for (int i = 0; i < kProbePorts; ++i)
						{
							in[i] = InputPortConfig_AnyType(inNames[i], "test input");
							out[i] = OutputPortConfig_AnyType(outNames[i], "test output");
						}
						in[kProbePorts] = SInputPortConfig{};
						out[kProbePorts] = SOutputPortConfig{};
						built = true;
					}
					config.pInputPorts = in;
					config.pOutputPorts = out;
					config.sDescription = "cev_flowtest probe (exists only while the test runs)";
					config.SetCategory(EFLN_DEBUG);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate || m_pData == nullptr)
						return;
					for (int i = 0; i < kProbePorts; ++i)
					{
						if (!IsPortActive(pActInfo, i))
							continue;
						++m_pData->count[i];
						string text;
						GetPortAny(pActInfo, i).GetValueWithConversion(text);
						m_pData->last[i] = text.c_str();
					}
				}

			private:
				SProbeData* m_pData;
			};

			class CProbeFactory final : public IFlowNodeFactory
			{
			public:
				virtual IFlowNodePtr Create(IFlowNode::SActivationInfo*) override { return new CProbeNode(s_pNextProbeData); }
				virtual void GetMemoryUsage(ICrySizer* s) const override {}
				virtual void Reset() override {}
			};

			//! One node under test with its own probe: probe outputs drive the node's inputs, the node's outputs go
			//! into probe inputs.
			struct STestNode
			{
				std::string className;
				TFlowNodeId id = InvalidFlowNodeId;
				TFlowNodeId probe = InvalidFlowNodeId;
				SProbeData data;
				std::map<std::string, int> drive;  // node input name -> probe output index
				std::map<std::string, int> watch;  // node output name -> probe input index
			};

			struct SFlowTest
			{
				enum class EStep { Idle, WaitReady, Transport1, Transport2, Transport3, WaitSurfaceB, WaitResized, WaitClosed, WaitRelease };
				EStep step = EStep::Idle;
				double stepTime = 0.0;
				int checks = 0;
				int failed = 0;
				std::string pathA, pathB;
				IFlowGraphPtr pGraph;
				bool typeRegistered = false;
				std::vector<std::unique_ptr<STestNode>> nodes;
				uint32_t basePlayers = 0;
				uint32_t baseSurfaces = 0;
				int handle = 0;          // of the Open node
				float timeBeforePause = 0.f;

				void Check(bool ok, const std::string& what)
				{
					++checks;
					if (!ok)
						++failed;
					LogAlways("cev_flowtest: %s %s", ok ? "ok  " : "FAIL", what.c_str());
				}

				double Now() const { return gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.0; }
				double Elapsed() const { return Now() - stepTime; }
				void Go(EStep next) { step = next; stepTime = Now(); }
			};

			SFlowTest s_test;

			TFlowPortId FindPort(IFlowGraph& graph, TFlowNodeId node, const char* name, bool output)
			{
				IFlowNodeData* const pData = graph.GetNodeData(node);
				if (pData == nullptr)
					return InvalidFlowPortId;
				SFlowNodeConfig config;
				pData->GetConfiguration(config); // includes the entity port of EFLN_TARGET_ENTITY nodes
				if (output)
				{
					for (int i = 0; config.pOutputPorts != nullptr && config.pOutputPorts[i].name != nullptr; ++i)
					{
						if (strcmp(config.pOutputPorts[i].name, name) == 0)
							return static_cast<TFlowPortId>(i);
					}
				}
				else
				{
					for (int i = 0; config.pInputPorts != nullptr && config.pInputPorts[i].name != nullptr; ++i)
					{
						if (strcmp(config.pInputPorts[i].name, name) == 0)
							return static_cast<TFlowPortId>(i);
					}
				}
				return InvalidFlowPortId;
			}

			STestNode* AddNode(SFlowTest& t, const char* className, std::initializer_list<const char*> inputs, std::initializer_list<const char*> outputs)
			{
				std::unique_ptr<STestNode> pNode = std::make_unique<STestNode>();
				STestNode& n = *pNode;
				n.className = className;
				const std::string index = std::to_string(t.nodes.size());
				n.id = t.pGraph->CreateNode(className, ("node" + index).c_str());
				s_pNextProbeData = &n.data;
				n.probe = t.pGraph->CreateNode(kProbeType, ("probe" + index).c_str());
				s_pNextProbeData = nullptr;
				bool ok = n.id != InvalidFlowNodeId && n.probe != InvalidFlowNodeId;
				int k = 0;
				for (const char* name : inputs)
				{
					const TFlowPortId port = ok ? FindPort(*t.pGraph, n.id, name, false) : InvalidFlowPortId;
					ok = ok && port != InvalidFlowPortId && t.pGraph->LinkNodes(SFlowAddress(n.probe, static_cast<TFlowPortId>(k), true), SFlowAddress(n.id, port, false));
					n.drive[name] = k++;
				}
				k = 0;
				for (const char* name : outputs)
				{
					const TFlowPortId port = ok ? FindPort(*t.pGraph, n.id, name, true) : InvalidFlowPortId;
					ok = ok && port != InvalidFlowPortId && t.pGraph->LinkNodes(SFlowAddress(n.id, port, true), SFlowAddress(n.probe, static_cast<TFlowPortId>(k), false));
					n.watch[name] = k++;
				}
				t.Check(ok, std::string("node ") + className + " created and linked");
				t.nodes.push_back(std::move(pNode));
				return t.nodes.back().get();
			}

			template<typename T>
			void Set(SFlowTest& t, STestNode* pNode, const char* input, const T& value)
			{
				const std::map<std::string, int>::const_iterator it = pNode->drive.find(input);
				if (it == pNode->drive.end() || !t.pGraph)
				{
					t.Check(false, std::string("test bug: input <") + input + "> of " + pNode->className + " is not driven");
					return;
				}
				t.pGraph->ActivatePortAny(SFlowAddress(pNode->probe, static_cast<TFlowPortId>(it->second), true), TFlowInputData(value));
			}

			void Trigger(SFlowTest& t, STestNode* pNode, const char* input) { Set(t, pNode, input, SFlowSystemVoid()); }

			int Count(STestNode* pNode, const char* output)
			{
				const std::map<std::string, int>::const_iterator it = pNode->watch.find(output);
				return it != pNode->watch.end() ? pNode->data.count[it->second] : -1;
			}

			std::string Last(STestNode* pNode, const char* output)
			{
				const std::map<std::string, int>::const_iterator it = pNode->watch.find(output);
				return it != pNode->watch.end() ? pNode->data.last[it->second] : std::string("<not watched>");
			}

			int LastInt(STestNode* pNode, const char* output) { return std::atoi(Last(pNode, output).c_str()); }
			float LastFloat(STestNode* pNode, const char* output) { return static_cast<float>(std::atof(Last(pNode, output).c_str())); }
			bool LastBool(STestNode* pNode, const char* output) { const std::string s = Last(pNode, output); return s == "1" || s == "true"; }

			void Update(SFlowTest& t)
			{
				if (t.pGraph)
					t.pGraph->Update();
			}

			SValue Query(const char* name, int handle)
			{
				SValue v;
				Call(*FindByName(name), handle, nullptr, 0, &v);
				return v;
			}

			// node indices in s_test.nodes
			enum ENode { eN_SurfaceA, eN_SurfaceDup, eN_Info, eN_Events, eN_SurfaceB, eN_Open, eN_Transport, eN_Settings, eN_AdvInfo, eN_AdvEvents, eN_Screen, eN_Material, eN_Count };
			STestNode* N(ENode n) { return s_test.nodes[n].get(); }

			void Finish(SFlowTest& t, const char* why)
			{
				if (why != nullptr)
					t.Check(false, why);
				t.nodes.clear();
				t.pGraph = nullptr;
				if (t.typeRegistered && gEnv->pFlowSystem != nullptr)
					gEnv->pFlowSystem->UnregisterType(kProbeType);
				t.typeRegistered = false;
				t.step = SFlowTest::EStep::Idle;
				LogAlways("cev_flowtest: %s (%d checks, %d failed)", t.failed == 0 ? "PASS" : "FAIL", t.checks, t.failed);
			}

			bool Timeout(SFlowTest& t, double seconds, const char* what)
			{
				if (t.Elapsed() < seconds)
					return false;
				Finish(t, (std::string("timeout: ") + what).c_str());
				return true;
			}

			void Build(SFlowTest& t)
			{
				AddNode(t, "VideoUI:PlayToSurface", { "Play", "Stop", "Video", "SurfaceID", "Loop", "Decode" }, { "Ready", "TextureId", "Width", "Height", "Duration", "HasAlpha", "Ended", "Error", "ErrorCode", "ErrorMessage" });
				AddNode(t, "VideoUI:PlayToSurface", { "Play", "Video", "SurfaceID" }, { "Ready", "Error", "ErrorCode", "ErrorMessage" });
				AddNode(t, "VideoUI:SurfaceInfo", { "Get", "SurfaceID" }, { "Exists", "TextureId", "Width", "Height", "PixelLayout", "Premultiplied", "HasAlpha", "FrameSerial", "State" });
				AddNode(t, "VideoUI:SurfaceEvents", { "SurfaceID" }, { "Ready", "NewFrame", "FrameSerial", "Resized", "TextureId", "Width", "Height", "Closed" });
				AddNode(t, "VideoUI:PlayToSurface", { "Play", "Stop", "Video", "SurfaceID" }, { "Ready", "Error" });
				AddNode(t, "VideoAdvanced:Open", { "Open", "Close", "Video", "Output", "Fit", "Decode" }, { "Handle", "Ready", "Error", "ErrorCode", "ErrorMessage" });
				AddNode(t, "VideoAdvanced:Transport", { "Handle", "Play", "Pause", "Stop", "Seek", "Seconds" }, { "Done", "Error" });
				AddNode(t, "VideoAdvanced:Settings", { "Handle", "Loop", "Rate", "Volume", "Language", "Variant", "Skippable" }, { "Done", "Error" });
				AddNode(t, "VideoAdvanced:Info", { "Get", "Handle" }, { "State", "Time", "Duration", "Width", "Height", "HasAlpha", "Language", "Variant", "DecodeBackend", "FrameSerial", "SurfaceId", "Error" });
				AddNode(t, "VideoAdvanced:Events", { "Handle" }, { "Ready", "Started", "NewFrame", "NewFrameSerial", "Marker", "Looped", "Ended", "Skipped", "Error" });
				AddNode(t, "Video:PlayOnScreen", { "Play", "Stop", "Video", "Fit", "Loop", "Skippable", "Decode" }, { "Started", "Ended", "Skipped", "Error", "ErrorCode", "ErrorMessage" });
				AddNode(t, "Video:PlayOnMaterial", { "Play", "Stop", "Video", "Material", "Slot" }, { "Started", "Ended", "Error", "ErrorCode", "ErrorMessage" });
			}
		}

		void StartFlowTest(const char* pathA, const char* pathB)
		{
			SFlowTest& t = s_test;
			if (t.step != SFlowTest::EStep::Idle)
			{
				LogWarning("cev_flowtest is already running");
				return;
			}
			if (mEnv == nullptr || mEnv->pApi == nullptr || mEnv->pManager == nullptr || gEnv->pFlowSystem == nullptr)
			{
				LogWarning("cev_flowtest: no flow system / plug-in not available");
				return;
			}
			t.checks = 0;
			t.failed = 0;
			t.handle = 0;
			t.nodes.clear();
			t.pathA = pathA != nullptr && pathA[0] != '\0' ? pathA : "Videos/vp9_alpha.webm";
			t.pathB = pathB != nullptr && pathB[0] != '\0' ? pathB : "Videos/vp9_odd_321x181.webm";
			LogAlways("cev_flowtest: start (%s, %s)", t.pathA.c_str(), t.pathB.c_str());

			// node classes registered under the documented names, the old ones gone
			bool classes = FlowNodeCount() == 10;
			for (uint32_t i = 0; i < FlowNodeCount(); ++i)
				classes = classes && gEnv->pFlowSystem->GetTypeId(FlowNodeClassName(i)) != InvalidFlowNodeTypeId;
			t.Check(classes, "the 10 node classes are registered (" + std::to_string(FlowNodeCount()) + ")");
			bool oldGone = true;
			for (const char* old : { "CEVPlayer:2DVideoPlayer", "CEVPlayer:TextureVideoPlayer", "CEVPlayer:FlashVideoPlayer", "CEVPlayer:Video:Control:Create", "CEVPlayer:Video:Query:State", "CEVPlayer:Video:Events" })
				oldGone = oldGone && gEnv->pFlowSystem->GetTypeId(old) == InvalidFlowNodeTypeId;
			t.Check(oldGone, "the legacy and the generated CEVPlayer:Video:* node classes are not registered");

			t.typeRegistered = gEnv->pFlowSystem->RegisterType(kProbeType, IFlowNodeFactoryPtr(new CProbeFactory())) != InvalidFlowNodeTypeId;
			if (!t.typeRegistered)
			{
				Finish(t, "probe node type could not be registered");
				return;
			}
			t.pGraph = gEnv->pFlowSystem->CreateFlowGraph();
			if (!t.pGraph)
			{
				Finish(t, "CreateFlowGraph failed");
				return;
			}
			t.pGraph->SetEnabled(true);
			t.pGraph->SetActive(true);

			t.basePlayers = mEnv->pManager->GetPlayerCount();
			t.baseSurfaces = Contract::SurfaceCount();
			Build(t);
			if (t.failed != 0)
			{
				Finish(t, nullptr);
				return;
			}
			Update(t); // initialises the graph (eFE_Initialize); activations made before would be flushed

			// SurfaceEvents follows a surface that does not exist yet
			Set(t, N(eN_Events), "SurfaceID", string("flowtest_b"));
			// PlayToSurface A
			Set(t, N(eN_SurfaceA), "Video", string(t.pathA.c_str()));
			Set(t, N(eN_SurfaceA), "SurfaceID", string("flowtest_a"));
			Set(t, N(eN_SurfaceA), "Loop", true);
			Set(t, N(eN_SurfaceA), "Decode", 1); // GPU: recorded, a warning is expected
			Trigger(t, N(eN_SurfaceA), "Play");
			// Open (Screen)
			Set(t, N(eN_Open), "Video", string(t.pathA.c_str()));
			Set(t, N(eN_Open), "Output", static_cast<int>(EOutputMode::Screen));
			Set(t, N(eN_Open), "Fit", 1);
			Trigger(t, N(eN_Open), "Open");
			// PlayOnScreen
			Set(t, N(eN_Screen), "Video", string(t.pathA.c_str()));
			Set(t, N(eN_Screen), "Loop", true);
			Trigger(t, N(eN_Screen), "Play");
			// PlayOnMaterial with a material that does not exist: Error
			Set(t, N(eN_Material), "Video", string(t.pathA.c_str()));
			Set(t, N(eN_Material), "Material", string("cev_flowtest/no_such_material"));
			Trigger(t, N(eN_Material), "Play");
			Update(t);
			t.Go(SFlowTest::EStep::WaitReady);
		}

		void TickFlowTest()
		{
			SFlowTest& t = s_test;
			if (t.step == SFlowTest::EStep::Idle)
				return;
			if (mEnv == nullptr || mEnv->pApi == nullptr || gEnv->pFlowSystem == nullptr)
			{
				ShutdownFlowTest();
				return;
			}
			Update(t);

			switch (t.step)
			{
			case SFlowTest::EStep::WaitReady:
			{
				if (Count(N(eN_SurfaceA), "Ready") < 1 || Count(N(eN_Open), "Ready") < 1 || Count(N(eN_Screen), "Started") < 1)
				{
					Timeout(t, 8.0, "PlayToSurface Ready / Open Ready / PlayOnScreen Started");
					return;
				}
				// PlayToSurface -> Ready with its values
				STestNode* const a = N(eN_SurfaceA);
				const int ha = mEnv->pApi->AttachSurface("flowtest_a");
				t.Check(ha != 0, "surface flowtest_a exists");
				t.Check(Count(a, "Ready") == 1, "PlayToSurface Ready once (" + std::to_string(Count(a, "Ready")) + ")");
				t.Check(LastInt(a, "TextureId") > 0 && LastInt(a, "TextureId") == Query("TextureId", ha).i, "Ready TextureId " + Last(a, "TextureId") + " = surface texture");
				t.Check(LastInt(a, "Width") > 0 && LastInt(a, "Width") == Query("Width", ha).i && LastInt(a, "Height") == Query("Height", ha).i, "Ready Width x Height " + Last(a, "Width") + "x" + Last(a, "Height"));
				t.Check(LastFloat(a, "Duration") > 0.f, "Ready Duration " + Last(a, "Duration"));
				t.Check(LastBool(a, "HasAlpha") == Query("HasAlpha", ha).b, "Ready HasAlpha " + Last(a, "HasAlpha"));
				t.Check(Count(a, "Error") == 0, "PlayToSurface no Error");
				t.Check(mEnv->pApi->GetDecodeMode(ha) == static_cast<int>(EDecodeMode::Gpu) && Query("DecodeBackend", ha).i == 0, "Decode GPU recorded, backend CPU");
				t.Check(Query("IsLooping", ha).b, "Loop applied");

				// PlayOnMaterial with an unknown material
				STestNode* const m = N(eN_Material);
				t.Check(Count(m, "Error") >= 1 && LastInt(m, "ErrorCode") == static_cast<int>(cev::Status::InvalidArgument) && !Last(m, "ErrorMessage").empty(),
					"PlayOnMaterial unknown material -> Error " + Last(m, "ErrorCode") + " <" + Last(m, "ErrorMessage") + ">");

				// SurfaceEvents waits: flowtest_b does not exist yet
				STestNode* const ev = N(eN_Events);
				t.Check(Count(ev, "Ready") == 0 && Count(ev, "NewFrame") == 0 && Count(ev, "Closed") == 0, "SurfaceEvents silent while the surface does not exist");

				// second PlayToSurface with a taken id
				STestNode* const dup = N(eN_SurfaceDup);
				Set(t, dup, "Video", string(t.pathA.c_str()));
				Set(t, dup, "SurfaceID", string("FLOWTEST_A"));
				Trigger(t, dup, "Play");
				Update(t);
				t.Check(Count(dup, "Error") == 1 && LastInt(dup, "ErrorCode") == static_cast<int>(cev::Status::InvalidState) && !Last(dup, "ErrorMessage").empty(),
					"PlayToSurface with a taken SurfaceID -> Error " + Last(dup, "ErrorCode") + " <" + Last(dup, "ErrorMessage") + ">");

				// SurfaceInfo
				STestNode* const info = N(eN_Info);
				Set(t, info, "SurfaceID", string("flowtest_a"));
				Trigger(t, info, "Get");
				Update(t);
				t.Check(Count(info, "Exists") == 1 && LastBool(info, "Exists"), "SurfaceInfo Exists");
				t.Check(LastInt(info, "TextureId") == LastInt(a, "TextureId") && LastInt(info, "Width") == LastInt(a, "Width") && LastInt(info, "Height") == LastInt(a, "Height"),
					"SurfaceInfo TextureId / Width / Height " + Last(info, "TextureId") + " " + Last(info, "Width") + "x" + Last(info, "Height"));
				t.Check(LastInt(info, "PixelLayout") == 0 && LastBool(info, "Premultiplied") && LastBool(info, "HasAlpha") == LastBool(a, "HasAlpha"), "SurfaceInfo PixelLayout 0, Premultiplied, HasAlpha");
				t.Check(std::strtoull(Last(info, "FrameSerial").c_str(), nullptr, 10) > 0 && LastInt(info, "State") == static_cast<int>(EVideoPlayerState::Playing),
					"SurfaceInfo FrameSerial " + Last(info, "FrameSerial") + ", State " + Last(info, "State"));
				Set(t, info, "SurfaceID", string("flowtest_none"));
				Trigger(t, info, "Get");
				Update(t);
				t.Check(Count(info, "Exists") == 2 && !LastBool(info, "Exists") && LastInt(info, "TextureId") == 0 && LastInt(info, "Width") == 0 && Last(info, "FrameSerial").empty() && LastInt(info, "State") == 0,
					"SurfaceInfo of an unknown id: Exists false, the rest 0 / empty");

				// Open -> handle
				t.handle = LastInt(N(eN_Open), "Handle");
				t.Check(t.handle != 0 && mEnv->pApi->Find(t.handle, false) != nullptr, "Open Handle " + std::to_string(t.handle));
				t.Check(Query("State", t.handle).i == static_cast<int>(EVideoPlayerState::Stopped), "Open loads but does not play");

				// Settings: only the triggered input is applied
				STestNode* const s = N(eN_Settings);
				Set(t, s, "Handle", t.handle);
				Set(t, s, "Rate", 2.f);
				Update(t);
				t.Check(Count(s, "Done") == 1 && Query("Rate", t.handle).f == 2.f && !Query("IsLooping", t.handle).b && Query("Volume", t.handle).f == 1.f, "Settings Rate=2 applied alone");
				Set(t, s, "Loop", true);
				Update(t);
				t.Check(Count(s, "Done") == 2 && Query("IsLooping", t.handle).b && Query("Rate", t.handle).f == 2.f, "Settings Loop=true applied alone");
				Set(t, s, "Volume", 0.25f);
				Update(t);
				t.Check(Count(s, "Done") == 3 && Query("Volume", t.handle).f == 0.25f && Query("IsLooping", t.handle).b, "Settings Volume=0.25 applied alone");
				Set(t, s, "Variant", 0);
				Set(t, s, "Language", string(""));
				Update(t);
				t.Check(Count(s, "Done") == 5 && Count(s, "Error") == 0, "Settings Variant and Language accepted");
				Set(t, s, "Skippable", true);
				Update(t);
				t.Check(Count(s, "Done") == 6 && mEnv->pApi->IsSkippable(t.handle), "Settings Skippable=true applied");
				Set(t, s, "Skippable", false);
				Set(t, s, "Rate", 1.f);
				Update(t);
				t.Check(Count(s, "Done") == 8 && !mEnv->pApi->IsSkippable(t.handle) && Query("Rate", t.handle).f == 1.f, "Settings Skippable=false, Rate=1");
				Set(t, s, "Rate", -1.f);
				Update(t);
				t.Check(Count(s, "Error") == 1, "Settings Rate=-1 rejected -> Error");

				// Events and Transport on the handle
				Set(t, N(eN_AdvEvents), "Handle", t.handle);
				Set(t, N(eN_Transport), "Handle", t.handle);
				Set(t, N(eN_AdvInfo), "Handle", t.handle);
				Trigger(t, N(eN_Transport), "Play");
				Update(t);
				t.Check(Count(N(eN_Transport), "Done") == 1, "Transport Play accepted");
				t.Go(SFlowTest::EStep::Transport1);
				return;
			}
			case SFlowTest::EStep::Transport1:
				if (t.Elapsed() < 0.6)
					return;
				t.Check(Query("State", t.handle).i == static_cast<int>(EVideoPlayerState::Playing), "Transport Play -> Playing");
				t.timeBeforePause = Query("Time", t.handle).f;
				Trigger(t, N(eN_Transport), "Pause");
				Update(t);
				t.Check(Query("State", t.handle).i == static_cast<int>(EVideoPlayerState::Paused), "Transport Pause -> Paused");
				t.Go(SFlowTest::EStep::Transport2);
				return;
			case SFlowTest::EStep::Transport2:
			{
				if (t.Elapsed() < 0.3)
					return;
				const float paused = Query("Time", t.handle).f;
				t.Check(Query("State", t.handle).i == static_cast<int>(EVideoPlayerState::Paused) && paused >= t.timeBeforePause - 0.05f, "still Paused at " + std::to_string(paused) + " s");
				Trigger(t, N(eN_Transport), "Play");
				Update(t);
				t.Check(Query("State", t.handle).i == static_cast<int>(EVideoPlayerState::Playing) && Query("Time", t.handle).f >= paused - 0.05f, "Transport Play continues the paused video (does not restart)");
				t.Go(SFlowTest::EStep::Transport3);
				return;
			}
			case SFlowTest::EStep::Transport3:
			{
				if (t.Elapsed() < 0.3)
				{
					if (t.Elapsed() > 0.05 && Count(N(eN_Transport), "Done") == 3)
					{
						Set(t, N(eN_Transport), "Seconds", 1.5f);
						Trigger(t, N(eN_Transport), "Seek");
						Update(t);
					}
					return;
				}
				const float time = Query("Time", t.handle).f;
				t.Check(Count(N(eN_Transport), "Done") == 4 && time >= 1.4f && time < 2.5f, "Transport Seek 1.5 -> time " + std::to_string(time));
				Trigger(t, N(eN_Transport), "Stop");
				Update(t);
				t.Check(Query("State", t.handle).i == static_cast<int>(EVideoPlayerState::Stopped) && Query("Time", t.handle).f < 0.05f, "Transport Stop -> Stopped at 0");
				t.Check(Count(N(eN_Transport), "Error") == 0, "Transport no Error");

				// Info
				STestNode* const info = N(eN_AdvInfo);
				Trigger(t, info, "Get");
				Update(t);
				t.Check(Count(info, "State") == 1 && LastInt(info, "State") == static_cast<int>(EVideoPlayerState::Stopped) && LastInt(info, "Width") == Query("Width", t.handle).i && LastInt(info, "Width") > 0,
					"Info State " + Last(info, "State") + ", Width " + Last(info, "Width"));
				t.Check(LastFloat(info, "Duration") > 0.f && LastInt(info, "DecodeBackend") == 0 && Last(info, "SurfaceId").empty() && !Last(info, "FrameSerial").empty(),
					"Info Duration " + Last(info, "Duration") + ", DecodeBackend 0, SurfaceId empty, FrameSerial " + Last(info, "FrameSerial"));
				Set(t, info, "Handle", 999999);
				Trigger(t, info, "Get");
				Update(t);
				t.Check(Count(info, "Error") == 1, "Info Get on an invalid handle -> Error");

				// Events on the handle
				STestNode* const ev = N(eN_AdvEvents);
				const std::string serial = Last(ev, "NewFrameSerial");
				t.Check(Count(ev, "Started") >= 1 && Count(ev, "NewFrame") >= 5 && !serial.empty() && serial.find_first_not_of("0123456789") == std::string::npos,
					"Events Started " + std::to_string(Count(ev, "Started")) + ", NewFrame " + std::to_string(Count(ev, "NewFrame")) + ", serial " + serial);
				t.Check(Count(ev, "Error") == 0 && Count(ev, "Ready") == 0, "Events: no Error, no Ready (attached after the load)");

				// PlayOnScreen Stop destroys its player
				const uint32_t before = mEnv->pManager->GetPlayerCount();
				Trigger(t, N(eN_Screen), "Stop");
				Update(t);
				t.Check(mEnv->pManager->GetPlayerCount() + 1 == before, "PlayOnScreen Stop destroyed its player (" + std::to_string(before) + " -> " + std::to_string(mEnv->pManager->GetPlayerCount()) + ")");

				// the surface SurfaceEvents waits for appears now
				STestNode* const b = N(eN_SurfaceB);
				Set(t, b, "Video", string(t.pathA.c_str()));
				Set(t, b, "SurfaceID", string("flowtest_b"));
				Trigger(t, b, "Play");
				Update(t);
				t.Go(SFlowTest::EStep::WaitSurfaceB);
				return;
			}
			case SFlowTest::EStep::WaitSurfaceB:
			{
				STestNode* const ev = N(eN_Events);
				if (Count(ev, "Ready") < 1 || Count(ev, "NewFrame") < 3)
				{
					Timeout(t, 6.0, "SurfaceEvents Ready / NewFrame after the surface appeared");
					return;
				}
				t.Check(Count(ev, "Ready") == 1 && LastInt(ev, "Width") == LastInt(N(eN_SurfaceA), "Width") && LastInt(ev, "TextureId") > 0,
					"SurfaceEvents Ready after the surface appeared (" + Last(ev, "Width") + "x" + Last(ev, "Height") + ", tex " + Last(ev, "TextureId") + ")");
				t.Check(!Last(ev, "FrameSerial").empty(), "SurfaceEvents NewFrame " + std::to_string(Count(ev, "NewFrame")) + ", serial " + Last(ev, "FrameSerial"));
				// another video of another size into the same surface: Resized
				STestNode* const b = N(eN_SurfaceB);
				Set(t, b, "Video", string(t.pathB.c_str()));
				Trigger(t, b, "Play");
				Update(t);
				t.Go(SFlowTest::EStep::WaitResized);
				return;
			}
			case SFlowTest::EStep::WaitResized:
			{
				STestNode* const ev = N(eN_Events);
				if (Count(ev, "Resized") < 1)
				{
					Timeout(t, 6.0, "SurfaceEvents Resized");
					return;
				}
				const int hb = mEnv->pApi->AttachSurface("flowtest_b");
				t.Check(Count(ev, "Resized") >= 1 && LastInt(ev, "Width") == Query("Width", hb).i && LastInt(ev, "Height") == Query("Height", hb).i,
					"SurfaceEvents Resized " + std::to_string(Count(ev, "Resized")) + " -> " + Last(ev, "Width") + "x" + Last(ev, "Height") + " (Ready " + std::to_string(Count(ev, "Ready")) + ")");
				Trigger(t, N(eN_SurfaceB), "Stop");
				Update(t);
				t.Go(SFlowTest::EStep::WaitClosed);
				return;
			}
			case SFlowTest::EStep::WaitClosed:
			{
				STestNode* const ev = N(eN_Events);
				if (Count(ev, "Closed") < 1)
				{
					Timeout(t, 2.0, "SurfaceEvents Closed");
					return;
				}
				t.Check(Count(ev, "Closed") == 1 && mEnv->pApi->AttachSurface("flowtest_b") == 0, "SurfaceEvents Closed after PlayToSurface Stop");
				// alive now: surface A, the Open player
				t.Check(Contract::SurfaceCount() == t.baseSurfaces + 1, "surfaces " + std::to_string(Contract::SurfaceCount()) + " = base " + std::to_string(t.baseSurfaces) + " + 1");
				t.Check(mEnv->pManager->GetPlayerCount() == t.basePlayers + 2, "players " + std::to_string(mEnv->pManager->GetPlayerCount()) + " = base " + std::to_string(t.basePlayers) + " + 2");
				Contract::LogSurfaces();
				// destroying the graph destroys the players its nodes own
				t.nodes.clear();
				t.pGraph = nullptr;
				t.Go(SFlowTest::EStep::WaitRelease);
				return;
			}
			case SFlowTest::EStep::WaitRelease:
				if (mEnv->pManager->GetPlayerCount() != t.basePlayers || Contract::SurfaceCount() != t.baseSurfaces)
				{
					Timeout(t, 2.0, "players / surfaces released with the graph");
					return;
				}
				Contract::LogSurfaces();
				t.Check(true, "graph destroyed: players " + std::to_string(mEnv->pManager->GetPlayerCount()) + " and surfaces " + std::to_string(Contract::SurfaceCount()) + " back to the base");
				t.Check(t.handle != 0 && mEnv->pApi->Find(t.handle, false) == nullptr, "the Open handle is invalid after the graph is gone");
				Finish(t, nullptr);
				return;
			default:
				return;
			}
		}

		void ShutdownFlowTest()
		{
			SFlowTest& t = s_test;
			if (t.step == SFlowTest::EStep::Idle && !t.pGraph && !t.typeRegistered)
				return;
			t.nodes.clear();
			t.pGraph = nullptr;
			if (t.typeRegistered && gEnv != nullptr && gEnv->pFlowSystem != nullptr)
				gEnv->pFlowSystem->UnregisterType(kProbeType);
			t.typeRegistered = false;
			t.step = SFlowTest::EStep::Idle;
		}
	}
}
