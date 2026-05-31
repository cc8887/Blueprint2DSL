// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispModule.h

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UObject;
class UEdGraph;
class UEdGraphNode;

namespace BlueprintLispImportLifecycle
{
	enum class EImportLifecyclePhase : uint8
	{
		PreNodeChanges,
		PostNodeChanges,
		PrePropertyChanges,
		PostPropertyChanges,
		PreFinalize,
		PostFinalize,
	};

	enum class EImportNodeChangeType : uint8
	{
		Added,
		Modified,
		Removed,
	};

	enum class EImportPropertyChangeType : uint8
	{
		Added,
		Modified,
		Removed,
	};

	struct BLUEPRINTLISP_API FImportLifecycleContext
	{
		FGuid ImportSessionId;
		UObject* TargetAsset = nullptr;
		UEdGraph* TargetGraph = nullptr;
		FName ScopeName;
		bool bIsFullRebuild = false;
		bool bIsIncremental = false;
		bool bIsHeadless = false;
		bool bWillCompile = false;
		TSet<FName> RequestedBehaviors;
	};

	struct BLUEPRINTLISP_API FImportNodeChange
	{
		UEdGraphNode* Node = nullptr;
		EImportNodeChangeType ChangeType = EImportNodeChangeType::Added;
	};

	struct BLUEPRINTLISP_API FImportPropertyChange
	{
		UObject* TargetObject = nullptr;
		FName PropertyName;
		EImportPropertyChangeType ChangeType = EImportPropertyChangeType::Modified;
	};

	struct BLUEPRINTLISP_API FImportNodePhaseEvent
	{
		EImportLifecyclePhase Phase = EImportLifecyclePhase::PreNodeChanges;
		FImportLifecycleContext Context;
		TArray<FImportNodeChange> Changes;
	};

	struct BLUEPRINTLISP_API FImportPropertyPhaseEvent
	{
		EImportLifecyclePhase Phase = EImportLifecyclePhase::PrePropertyChanges;
		FImportLifecycleContext Context;
		TArray<FImportPropertyChange> Changes;
	};

	struct BLUEPRINTLISP_API FImportFinalizePhaseEvent
	{
		EImportLifecyclePhase Phase = EImportLifecyclePhase::PreFinalize;
		FImportLifecycleContext Context;
	};

	struct BLUEPRINTLISP_API FImportLifecycleHookHandle
	{
		FGuid Id;

		bool IsValid() const
		{
			return Id.IsValid();
		}

		friend bool operator==(const FImportLifecycleHookHandle& Lhs, const FImportLifecycleHookHandle& Rhs)
		{
			return Lhs.Id == Rhs.Id;
		}
	};

	class BLUEPRINTLISP_API IImportLifecycleHook
	{
	public:
		virtual ~IImportLifecycleHook() = default;

		virtual int32 GetPriority(EImportLifecyclePhase Phase) const
		{
			return 0;
		}

		virtual void OnNodePhase(const FImportNodePhaseEvent& Event) {}
		virtual void OnPropertyPhase(const FImportPropertyPhaseEvent& Event) {}
		virtual void OnFinalizePhase(const FImportFinalizePhaseEvent& Event) {}
	};
}

class BLUEPRINTLISP_API IBlueprintLispImportHookHost
{
public:
	virtual ~IBlueprintLispImportHookHost() = default;

	virtual BlueprintLispImportLifecycle::FImportLifecycleHookHandle RegisterImportLifecycleHook(
		TSharedRef<BlueprintLispImportLifecycle::IImportLifecycleHook> Hook) = 0;

	virtual void UnregisterImportLifecycleHook(
		BlueprintLispImportLifecycle::FImportLifecycleHookHandle Handle) = 0;
};

class BLUEPRINTLISP_API FBlueprintLispModule : public IModuleInterface, public IBlueprintLispImportHookHost
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FBlueprintLispModule& Get()
	{
		return FModuleManager::GetModuleChecked<FBlueprintLispModule>("BlueprintLisp");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("BlueprintLisp");
	}

	virtual BlueprintLispImportLifecycle::FImportLifecycleHookHandle RegisterImportLifecycleHook(
		TSharedRef<BlueprintLispImportLifecycle::IImportLifecycleHook> Hook) override;

	virtual void UnregisterImportLifecycleHook(
		BlueprintLispImportLifecycle::FImportLifecycleHookHandle Handle) override;

	void BroadcastNodePhase(const BlueprintLispImportLifecycle::FImportNodePhaseEvent& Event);
	void BroadcastPropertyPhase(const BlueprintLispImportLifecycle::FImportPropertyPhaseEvent& Event);
	void BroadcastFinalizePhase(const BlueprintLispImportLifecycle::FImportFinalizePhaseEvent& Event);

	struct FRegisteredImportHook
	{
		BlueprintLispImportLifecycle::FImportLifecycleHookHandle Handle;
		TSharedRef<BlueprintLispImportLifecycle::IImportLifecycleHook> Hook;
		int64 RegistrationOrder = 0;
	};

private:
	TArray<FRegisteredImportHook> RegisteredHooks;
	int64 NextRegistrationOrder = 0;
};
