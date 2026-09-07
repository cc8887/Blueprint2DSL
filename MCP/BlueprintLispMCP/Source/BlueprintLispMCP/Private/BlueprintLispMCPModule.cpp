// BlueprintLispMCPModule.cpp - Toolset Registry lifecycle for BlueprintLisp

#include "BlueprintLispToolset.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FBlueprintLispMCPModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		RegisterToolset();
		if (!bToolsetRegistered)
		{
			PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(
				this, &FBlueprintLispMCPModule::RegisterToolset);
		}
	}

	virtual void ShutdownModule() override
	{
		if (PostEngineInitHandle.IsValid())
		{
			FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
			PostEngineInitHandle.Reset();
		}

		if (bToolsetRegistered && UToolsetRegistry::IsAvailable())
		{
			UToolsetRegistry::UnregisterToolsetClass(UBlueprintLispToolset::StaticClass());
		}
		bToolsetRegistered = false;
	}

private:
	void RegisterToolset()
	{
		if (bToolsetRegistered)
		{
			return;
		}

		if (!UToolsetRegistry::IsAvailable())
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("BlueprintLispMCP: ToolsetRegistry is not ready; waiting for post-engine init."));
			return;
		}

		UToolsetRegistry::RegisterToolsetClass(UBlueprintLispToolset::StaticClass());
		bToolsetRegistered = UToolsetRegistry::IsToolsetClassRegistered(UBlueprintLispToolset::StaticClass());
	}

	FDelegateHandle PostEngineInitHandle;
	bool bToolsetRegistered = false;
};

IMPLEMENT_MODULE(FBlueprintLispMCPModule, BlueprintLispMCP)
