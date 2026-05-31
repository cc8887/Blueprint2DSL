// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispModule.cpp

#include "BlueprintLispModule.h"
#include "FBlueprintLispMappingRegistry.h"
#include "Modules/ModuleManager.h"
#include "Algo/Sort.h"

#define LOCTEXT_NAMESPACE "FBlueprintLispModule"

namespace
{
	using namespace BlueprintLispImportLifecycle;

	template <typename EventType, typename CallbackType>
	void BP_SortAndBroadcastHooks(
		const TArray<FBlueprintLispModule::FRegisteredImportHook>& RegisteredHooks,
		EImportLifecyclePhase Phase,
		const EventType& Event,
		CallbackType&& Callback)
	{
		TArray<FBlueprintLispModule::FRegisteredImportHook> Hooks = RegisteredHooks;
		Algo::Sort(Hooks, [Phase](const FBlueprintLispModule::FRegisteredImportHook& Lhs,
			const FBlueprintLispModule::FRegisteredImportHook& Rhs)
		{
			const int32 LhsPriority = Lhs.Hook->GetPriority(Phase);
			const int32 RhsPriority = Rhs.Hook->GetPriority(Phase);
			if (LhsPriority != RhsPriority)
			{
				return LhsPriority > RhsPriority;
			}
			return Lhs.RegistrationOrder < Rhs.RegistrationOrder;
		});

		for (const FBlueprintLispModule::FRegisteredImportHook& Entry : Hooks)
		{
			Callback(*Entry.Hook, Event);
		}
	}
}

void FBlueprintLispModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("[BlueprintLisp] Module loaded."));
	FBlueprintLispMappingRegistry::Get().Initialize();
}

void FBlueprintLispModule::ShutdownModule()
{
	RegisteredHooks.Reset();
	UE_LOG(LogTemp, Log, TEXT("[BlueprintLisp] Module unloaded."));
}

BlueprintLispImportLifecycle::FImportLifecycleHookHandle FBlueprintLispModule::RegisterImportLifecycleHook(
	TSharedRef<BlueprintLispImportLifecycle::IImportLifecycleHook> Hook)
{
	BlueprintLispImportLifecycle::FImportLifecycleHookHandle Handle;
	Handle.Id = FGuid::NewGuid();

	FRegisteredImportHook Entry{Handle, Hook, NextRegistrationOrder++};
	RegisteredHooks.Add(MoveTemp(Entry));
	return Handle;
}

void FBlueprintLispModule::UnregisterImportLifecycleHook(
	BlueprintLispImportLifecycle::FImportLifecycleHookHandle Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}

	RegisteredHooks.RemoveAll([&Handle](const FRegisteredImportHook& Entry)
	{
		return Entry.Handle == Handle;
	});
}

void FBlueprintLispModule::BroadcastNodePhase(const BlueprintLispImportLifecycle::FImportNodePhaseEvent& Event)
{
	BP_SortAndBroadcastHooks(RegisteredHooks, Event.Phase, Event,
		[](BlueprintLispImportLifecycle::IImportLifecycleHook& Hook,
			const BlueprintLispImportLifecycle::FImportNodePhaseEvent& InEvent)
		{
			Hook.OnNodePhase(InEvent);
		});
}

void FBlueprintLispModule::BroadcastPropertyPhase(const BlueprintLispImportLifecycle::FImportPropertyPhaseEvent& Event)
{
	BP_SortAndBroadcastHooks(RegisteredHooks, Event.Phase, Event,
		[](BlueprintLispImportLifecycle::IImportLifecycleHook& Hook,
			const BlueprintLispImportLifecycle::FImportPropertyPhaseEvent& InEvent)
		{
			Hook.OnPropertyPhase(InEvent);
		});
}

void FBlueprintLispModule::BroadcastFinalizePhase(const BlueprintLispImportLifecycle::FImportFinalizePhaseEvent& Event)
{
	BP_SortAndBroadcastHooks(RegisteredHooks, Event.Phase, Event,
		[](BlueprintLispImportLifecycle::IImportLifecycleHook& Hook,
			const BlueprintLispImportLifecycle::FImportFinalizePhaseEvent& InEvent)
		{
			Hook.OnFinalizePhase(InEvent);
		});
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintLispModule, BlueprintLisp)
