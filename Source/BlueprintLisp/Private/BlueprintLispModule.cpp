// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispModule.cpp

#include "BlueprintLispModule.h"
#include "FBlueprintLispMappingRegistry.h"
#include "Modules/ModuleManager.h"

#define LOCTEXT_NAMESPACE "FBlueprintLispModule"

void FBlueprintLispModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("[BlueprintLisp] Module loaded."));

	// Initialize the MappingRegistry for default path resolution
	FBlueprintLispMappingRegistry::Get().Initialize();
}

void FBlueprintLispModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("[BlueprintLisp] Module unloaded."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintLispModule, BlueprintLisp)
