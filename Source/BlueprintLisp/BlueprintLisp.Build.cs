// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintLisp : ModuleRules
{
	public BlueprintLisp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// Public: available to dependent modules
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
		});

		// Editor-only: Blueprint graph manipulation
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"BlueprintGraph",
			"Kismet",
			"KismetCompiler",
			"GraphEditor",
			"EditorSubsystem",
			"EditorFramework",
			"AssetRegistry",
			"AssetTools",
			// AnimGraph: for AnimationTransitionGraph and AnimGraphNode_TransitionResult
			"AnimGraph",
		});
	}
}
