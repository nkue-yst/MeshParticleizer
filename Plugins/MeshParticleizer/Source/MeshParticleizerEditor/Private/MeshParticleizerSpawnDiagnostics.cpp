#include "MeshParticleizerPreparationLibrary.h"
#include "NiagaraDataInterfaceMeshParticleizerBudget.h"
#include "NiagaraDataInterfaceTexture.h"
#include "NiagaraGPUSystemTick.h"
#include "NiagaraScriptRuntimeData.h"
#include "NiagaraShader.h"
#include "Dom/JsonObject.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"

namespace MeshParticleizerSpawnDiagnostics
{
	struct FTrace
	{
		int32 Limit = 0;
		int32 Scheduled = 0;
		TArray<TSharedPtr<FJsonValue>> Rows;
	};

	// GTで開始・終了し、記録中はRTだけが触る。終了時のFlush後にJSONへ変換する。
	TSharedPtr<FTrace, ESPMode::ThreadSafe> Active;

	void Capture(const TSharedRef<FTrace, ESPMode::ThreadSafe>& Trace,
		const FNiagaraDataInterfaceSetShaderParametersContext& Context, const uint8* BudgetParameters)
	{
		const auto& Instance = Context.GetComputeInstanceData();
		if (Trace->Scheduled >= Trace->Limit ||
			(Instance.bResetData == false && Instance.SpawnInfo.SpawnRateInstances == 0 && Instance.SpawnInfo.EventSpawnTotal == 0))
		{
			return;
		}

		const auto& Metadata = Instance.Context->GPUScript_RT->GetScriptParametersMetadata_RT();
		const auto ProxyIndex = Instance.DataInterfaceProxies.IndexOfByKey(&Context.GetProxy<FNiagaraDataInterfaceProxy>());
		if (Metadata.DataInterfaceParamInfo.IsValidIndex(ProxyIndex) == false)
		{
			return;
		}
		const auto BudgetOffset = Metadata.DataInterfaceParamInfo[ProxyIndex].ShaderParametersOffset;
		if (BudgetOffset == INDEX_NONE)
		{
			return;
		}
		const auto* BaseParameters = BudgetParameters - Align(BudgetOffset, TShaderParameterStructTypeInfo<UNiagaraDataInterfaceMeshParticleizerBudget::FShaderParameters>::Alignment);

		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("render_frame"), GFrameNumberRenderThread);
		Row->SetStringField(TEXT("system_instance"), LexToString(Context.GetSystemInstanceID()));
		Row->SetStringField(TEXT("script"), Instance.Context->GPUScript_RT->GetFriendlyName());
		Row->SetBoolField(TEXT("reset"), Instance.bResetData);
		Row->SetNumberField(TEXT("spawn_instances"), Instance.SpawnInfo.SpawnRateInstances);
		Row->SetNumberField(TEXT("event_spawn"), Instance.SpawnInfo.EventSpawnTotal);

		// 実際にUniform Bufferへコピーされた定数を読む。GTの最新値とは区別する。
		auto Values = MakeShared<FJsonObject>();
		const auto* Store = Instance.Context->GetScriptRuntimeData()->GetExecutionReadyParameterStore();
		for (const auto& Variable : Store->ReadParameterVariables())
		{
			const auto Name = Variable.GetName().ToString();
			if (Name.StartsWith(TEXT("User.")) == false || Variable.Offset < 0 ||
				Variable.GetType().IsDataInterface() || Variable.GetType().IsUObject() ||
					static_cast<uint32>(Variable.Offset + Variable.GetSizeInBytes()) > Instance.ExternalParamDataSize)
			{
				continue;
			}
			const auto* Data = Instance.ExternalParamData + Variable.Offset;
			if (Variable.GetType() == FNiagaraTypeDefinition::GetIntDef())
			{
				auto Value = int32{0};
				FMemory::Memcpy(&Value, Data, sizeof(Value));
				Values->SetNumberField(Name, Value);
			}
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef() ||
				Variable.GetType() == FNiagaraTypeDefinition::GetVec3Def() || Variable.GetType() == FNiagaraTypeDefinition::GetVec4Def())
			{
				auto Components = TArray<TSharedPtr<FJsonValue>>{};
				for (auto Offset = 0; Offset < Variable.GetSizeInBytes(); Offset += sizeof(float))
				{
					auto Value = 0.f;
					FMemory::Memcpy(&Value, Data + Offset, sizeof(Value));
					Components.Add(MakeShared<FJsonValueNumber>(Value));
				}
				Values->SetArrayField(Name, Components);
			}
		}
		Row->SetObjectField(TEXT("uniforms"), Values);

		// このDIより後で設定されるTextureも含め、全DIの設定完了後に値を読む。
		// RDG内のCPU診断だけを追加し、GPU読戻しやTextureの再登録は行わない。
		auto Textures = TArray<TPair<FString, const UNiagaraDataInterfaceTexture::FShaderParameters*>>{};
		for (const auto& Info : Metadata.DataInterfaceParamInfo)
		{
			if (Info.DIClassName != TEXT("NiagaraDataInterfaceTexture") || Info.ShaderParametersOffset == INDEX_NONE)
			{
				continue;
			}
			const auto Offset = Align(Info.ShaderParametersOffset,
				TShaderParameterStructTypeInfo<UNiagaraDataInterfaceTexture::FShaderParameters>::Alignment);
			const auto* Parameters = reinterpret_cast<const UNiagaraDataInterfaceTexture::FShaderParameters*>(BaseParameters + Offset);
			if (Context.IsResourceBound(&Parameters->Texture) == true)
			{
				Textures.Emplace(Info.DataInterfaceHLSLSymbol, Parameters);
			}
		}
		++Trace->Scheduled;
		Context.GetGraphBuilder().AddPass(RDG_EVENT_NAME("MeshParticleizerSpawnDiagnostics"), ERDGPassFlags::None,
			[Trace, Row, Textures = MoveTemp(Textures)](FRHICommandListImmediate&) {
				auto Rows = TArray<TSharedPtr<FJsonValue>>{};
				for (const auto& Pair : Textures)
				{
					const auto* Parameters = Pair.Value;
					auto Texture = MakeShared<FJsonObject>();
					Texture->SetStringField(TEXT("symbol"), Pair.Key);
					Texture->SetNumberField(TEXT("width"), Parameters->TextureSize.X);
					Texture->SetNumberField(TEXT("height"), Parameters->TextureSize.Y);
					Texture->SetNumberField(TEXT("mips"), Parameters->MipLevels);
					Texture->SetBoolField(TEXT("bound"), Parameters->Texture != nullptr);
					if (Parameters->Texture != nullptr)
					{
						Texture->SetStringField(TEXT("rdg_name"), Parameters->Texture->Name);
						Texture->SetNumberField(TEXT("rdg_width"), Parameters->Texture->Desc.Extent.X);
						Texture->SetNumberField(TEXT("rdg_height"), Parameters->Texture->Desc.Extent.Y);
						Texture->SetNumberField(TEXT("format"), Parameters->Texture->Desc.Format);
					}
					Rows.Add(MakeShared<FJsonValueObject>(Texture));
				}
				Row->SetArrayField(TEXT("textures"), Rows);
				Trace->Rows.Add(MakeShared<FJsonValueObject>(Row));
			});
	}
}

bool UMeshParticleizerPreparationLibrary::BeginSpawnDiagnostics(int32 MaxRecords)
{
	if (MeshParticleizerSpawnDiagnostics::Active.IsValid() == true || MaxRecords < 1 || MaxRecords > 128)
	{
		return false;
	}
	auto Trace = MakeShared<MeshParticleizerSpawnDiagnostics::FTrace, ESPMode::ThreadSafe>();
	Trace->Limit = MaxRecords;
	MeshParticleizerSpawnDiagnostics::Active = Trace;
	UNiagaraDataInterfaceMeshParticleizerBudget::SetEditorDispatchObserver(
		[Trace](const FNiagaraDataInterfaceSetShaderParametersContext& Context, const uint8* Parameters) {
			MeshParticleizerSpawnDiagnostics::Capture(Trace, Context, Parameters);
		});
	return true;
}

FString UMeshParticleizerPreparationLibrary::EndSpawnDiagnostics()
{
	const auto Trace = MeshParticleizerSpawnDiagnostics::Active;
	if (Trace.IsValid() == false)
	{
		return TEXT("{\"active\":false,\"dispatches\":[]}");
	}
	UNiagaraDataInterfaceMeshParticleizerBudget::SetEditorDispatchObserver({});
	FlushRenderingCommands();
	MeshParticleizerSpawnDiagnostics::Active.Reset();

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("active"), false);
	Result->SetNumberField(TEXT("scheduled"), Trace->Scheduled);
	Result->SetBoolField(TEXT("limit_reached"), Trace->Scheduled == Trace->Limit);
	Result->SetArrayField(TEXT("dispatches"), Trace->Rows);
	auto Output = FString{};
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Output));
	return Output;
}
