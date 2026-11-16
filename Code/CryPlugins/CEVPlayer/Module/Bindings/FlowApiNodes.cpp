// FlowGraph nodes generated from the API table (CEVPlayer:Video:*). Registered through the same auto registration
// list as the legacy nodes (CAutoRegFlowNodeBase), so CVideoPlugin's PLUGIN_FLOWNODE_REGISTER picks them up.
//
//   CEVPlayer:Video:Control:<Name>   one per control:  In Trigger [, Handle] [, parameters]  ->  Done / Error [, result]
//   CEVPlayer:Video:Query:<Name>     one per query:    In Get [, Handle]  ->  Value / Error
//   CEVPlayer:Video:Events           In Attach / Detach / Handle  ->  one output per event (+ one per payload value)
//
// Players are addressed by their integer handle (Control:Create outputs it). Nodes only call Api::Call.
#include "StdAfx.h"
#include "PluginEnv.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <CryFlowGraph/IFlowBaseNode.h>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"

namespace CEVPlayer
{
	namespace Api
	{
		namespace
		{
			enum class ENode : uint8_t { Control, Query, Events };

			//! The static description of one node class (ports, indices). Lives as long as the module.
			struct SNodeDef
			{
				ENode node = ENode::Control;
				const SEntry* pEntry = nullptr;
				std::string name;
				std::string description;
				std::deque<std::string> text;   // stable storage for composed port names
				std::vector<SInputPortConfig> in;
				std::vector<SOutputPortConfig> out;

				// input layout
				int inTrigger = -1;
				int inHandle = -1;
				int inFirstParam = -1;
				// output layout (Control / Query)
				int outDone = -1;
				int outError = -1;
				int outResult = -1;
				// output layout (Events)
				int outEvent[eEvent_Count] = {};
				int outEventParam[eEvent_Count][kMaxParams] = {};

				const char* Keep(const std::string& s) { text.push_back(s); return text.back().c_str(); }
			};

			SInputPortConfig MakeInput(SNodeDef& def, const SParam& p)
			{
				const char* const name = p.name;
				const char* const desc = p.description;
				switch (p.type)
				{
				case EType::Bool: return InputPortConfig<bool>(name, p.defNum != 0.0, desc);
				case EType::Int: return InputPortConfig<int>(name, static_cast<int>(p.defNum), desc);
				case EType::Float: return InputPortConfig<float>(name, static_cast<float>(p.defNum), desc);
				default: return InputPortConfig<string>(name, string(p.defStr != nullptr ? p.defStr : ""), desc);
				}
			}

			SOutputPortConfig MakeOutput(EType type, const char* name, const char* desc)
			{
				switch (type)
				{
				case EType::Bool: return OutputPortConfig<bool>(name, desc);
				case EType::Int: return OutputPortConfig<int>(name, desc);
				case EType::Float: return OutputPortConfig<float>(name, desc);
				case EType::String: return OutputPortConfig<string>(name, desc);
				default: return OutputPortConfig_AnyType(name, desc);
				}
			}

			SValue ReadPort(IFlowNode::SActivationInfo* pActInfo, int port, EType type)
			{
				switch (type)
				{
				case EType::Bool: return SValue::FromBool(GetPortBool(pActInfo, port));
				case EType::Int: return SValue::FromInt(GetPortInt(pActInfo, port));
				case EType::Float: return SValue::FromFloat(GetPortFloat(pActInfo, port));
				case EType::String: return SValue::FromString(GetPortString(pActInfo, port).c_str());
				default: return SValue();
				}
			}

			void WritePort(IFlowNode::SActivationInfo* pActInfo, int port, const SValue& v)
			{
				switch (v.type)
				{
				case EType::Bool: ActivateOutput(pActInfo, port, v.b); break;
				case EType::Int: ActivateOutput(pActInfo, port, v.i); break;
				case EType::Float: ActivateOutput(pActInfo, port, v.f); break;
				case EType::String: ActivateOutput(pActInfo, port, string(v.s.c_str())); break;
				default: ActivateOutput(pActInfo, port, true); break;
				}
			}

			std::unique_ptr<SNodeDef> BuildControlOrQuery(const SEntry& e)
			{
				std::unique_ptr<SNodeDef> pDef = std::make_unique<SNodeDef>();
				SNodeDef& d = *pDef;
				const bool isQuery = e.kind == EKind::Query;
				d.node = isQuery ? ENode::Query : ENode::Control;
				d.pEntry = &e;
				d.name = std::string("CEVPlayer:Video:") + (isQuery ? "Query:" : "Control:") + e.name;
				d.description = e.description;

				d.inTrigger = static_cast<int>(d.in.size());
				d.in.push_back(InputPortConfig_Void(isQuery ? "Get" : "Trigger", isQuery ? "Reads the value and sends it to Value" : "Runs the control"));
				if (e.NeedsPlayer())
				{
					d.inHandle = static_cast<int>(d.in.size());
					d.in.push_back(InputPortConfig<int>("Handle", 0, "Player handle (from Control:Create)"));
				}
				d.inFirstParam = static_cast<int>(d.in.size());
				for (uint32_t i = 0; i < e.paramCount; ++i)
					d.in.push_back(MakeInput(d, e.params[i]));
				d.in.push_back(SInputPortConfig{}); // terminator ({ 0 })

				if (e.returnType != EType::Void)
				{
					d.outResult = static_cast<int>(d.out.size());
					d.out.push_back(MakeOutput(e.returnType, e.returnName != nullptr ? e.returnName : "Value", "Result"));
				}
				if (!isQuery)
				{
					d.outDone = static_cast<int>(d.out.size());
					d.out.push_back(OutputPortConfig_AnyType("Done", e.async ? "The request was accepted; completion is reported by the Events node (Ready / Error)" : "Done"));
				}
				d.outError = static_cast<int>(d.out.size());
				d.out.push_back(OutputPortConfig_AnyType("Error", "The call was rejected (invalid handle, bad argument, load refused); see the log"));
				d.out.push_back(SOutputPortConfig{});
				return pDef;
			}

			std::unique_ptr<SNodeDef> BuildEvents()
			{
				std::unique_ptr<SNodeDef> pDef = std::make_unique<SNodeDef>();
				SNodeDef& d = *pDef;
				d.node = ENode::Events;
				d.name = "CEVPlayer:Video:Events";
				d.description = "Events of one player: Attach it to a handle and the outputs fire when the player reports them. The payload outputs are set just before the event output.";

				d.in.push_back(InputPortConfig_Void("Attach", "Starts listening to the player of Handle (replaces a previous one)"));
				d.in.push_back(InputPortConfig_Void("Detach", "Stops listening"));
				d.inHandle = static_cast<int>(d.in.size());
				d.in.push_back(InputPortConfig<int>("Handle", 0, "Player handle (from Control:Create)"));
				d.in.push_back(SInputPortConfig{});

				const SSpan events = Events();
				for (uint32_t ev = 0; ev < eEvent_Count; ++ev)
				{
					const SEntry& e = events[ev];
					for (uint32_t i = 0; i < e.paramCount; ++i)
					{
						d.outEventParam[ev][i] = static_cast<int>(d.out.size());
						d.out.push_back(MakeOutput(e.params[i].type, d.Keep(std::string(e.name) + e.params[i].name), e.params[i].description));
					}
					d.outEvent[ev] = static_cast<int>(d.out.size());
					d.out.push_back(OutputPortConfig_AnyType(e.name, e.description));
				}
				d.out.push_back(SOutputPortConfig{});
				return pDef;
			}

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

			class CApiFlowNode final : public CFlowBaseNode<eNCT_Instanced>, public IEventSink
			{
			public:
				explicit CApiFlowNode(const SNodeDef& def) : m_def(def) {}

				~CApiFlowNode()
				{
					if (mEnv != nullptr && mEnv->pApi != nullptr)
					{
						mEnv->pApi->UnsubscribeAll(this);
						// players this node made die with it (the flow graph that owns the node is gone)
						for (const int handle : m_created)
						{
							if (mEnv->pApi->Find(handle, false) != nullptr)
								mEnv->pApi->Destroy(handle);
						}
					}
				}

				virtual IFlowNodePtr Clone(SActivationInfo* pActInfo) override { return new CApiFlowNode(m_def); }
				virtual void GetMemoryUsage(ICrySizer* pSizer) const override { pSizer->Add(*this); }

				virtual void GetConfiguration(SFlowNodeConfig& config) override
				{
					config.pInputPorts = m_def.in.data();
					config.pOutputPorts = m_def.out.data();
					config.sDescription = m_def.description.c_str();
					config.SetCategory(EFLN_APPROVED);
				}

				virtual void ProcessEvent(EFlowEvent event, SActivationInfo* pActInfo) override
				{
					if (event != eFE_Activate)
						return;
					if (m_def.node == ENode::Events)
						ProcessEventsNode(pActInfo);
					else
						ProcessCall(pActInfo);
				}

				// IEventSink
				virtual void OnApiEvent(int handle, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					if (event >= eEvent_Count || !m_graph.IsValid() || handle != m_attachedHandle)
						return;
					SActivationInfo* const pInfo = m_graph.Info();
					const SEntry& e = Events()[event];
					for (uint32_t i = 0; i < e.paramCount && i < argCount; ++i)
						WritePort(pInfo, m_def.outEventParam[event][i], args[i]);
					ActivateOutput(pInfo, m_def.outEvent[event], true);
				}

			private:
				void ProcessCall(SActivationInfo* pActInfo)
				{
					if (!IsPortActive(pActInfo, m_def.inTrigger))
						return;
					m_graph.Set(pActInfo);
					const SEntry& e = *m_def.pEntry;

					SValue in[kMaxParams];
					for (uint32_t i = 0; i < e.paramCount; ++i)
						in[i] = ReadPort(pActInfo, m_def.inFirstParam + static_cast<int>(i), e.params[i].type);
					const int handle = m_def.inHandle >= 0 ? GetPortInt(pActInfo, m_def.inHandle) : 0;

					SValue result;
					const bool ok = Call(e, handle, in, e.paramCount, &result);
					if (ok)
					{
						if (m_def.outResult >= 0)
							WritePort(pActInfo, m_def.outResult, result);
						if (e.fn != nullptr && stricmp(e.name, "Create") == 0 && result.i != 0)
							m_created.push_back(result.i);
						if (m_def.outDone >= 0)
							ActivateOutput(pActInfo, m_def.outDone, true);
					}
					else
					{
						ActivateOutput(pActInfo, m_def.outError, true);
					}
				}

				void ProcessEventsNode(SActivationInfo* pActInfo)
				{
					m_graph.Set(pActInfo);
					if (mEnv == nullptr || mEnv->pApi == nullptr)
						return;
					if (IsPortActive(pActInfo, 1)) // Detach
					{
						mEnv->pApi->UnsubscribeAll(this);
						m_attachedHandle = 0;
					}
					if (IsPortActive(pActInfo, 0)) // Attach
					{
						mEnv->pApi->UnsubscribeAll(this);
						m_attachedHandle = GetPortInt(pActInfo, m_def.inHandle);
						mEnv->pApi->Subscribe(m_attachedHandle, this); // warns for an invalid handle
					}
				}

				const SNodeDef& m_def;
				CGraphRef m_graph;
				int m_attachedHandle = 0;
				std::vector<int> m_created;
			};

			class CApiNodeFactory final : public CAutoRegFlowNodeBase
			{
			public:
				explicit CApiNodeFactory(const SNodeDef& def) : CAutoRegFlowNodeBase(def.name.c_str()), m_def(def) {}
				virtual IFlowNodePtr Create(IFlowNode::SActivationInfo*) override { return new CApiFlowNode(m_def); }
			private:
				const SNodeDef& m_def;
			};

			//! Builds the definitions from the table at module load and links the factories into the auto
			//! registration list (CryRegisterFlowNodes registers them with the flow system later).
			struct SRegistrar
			{
				std::vector<std::unique_ptr<SNodeDef>> defs;
				std::vector<std::unique_ptr<CApiNodeFactory>> factories;

				SRegistrar()
				{
					for (const SEntry& e : Controls())
						defs.push_back(BuildControlOrQuery(e));
					for (const SEntry& e : Queries())
						defs.push_back(BuildControlOrQuery(e));
					defs.push_back(BuildEvents());
					for (const std::unique_ptr<SNodeDef>& pDef : defs)
						factories.push_back(std::make_unique<CApiNodeFactory>(*pDef));
				}
			};

			SRegistrar s_registrar;
		}

		uint32_t FlowNodeCount() { return static_cast<uint32_t>(s_registrar.factories.size()); }
	}
}
