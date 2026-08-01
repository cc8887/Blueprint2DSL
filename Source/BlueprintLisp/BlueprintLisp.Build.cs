// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintLisp : ModuleRules
{
	public BlueprintLisp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// UE 4.27 supports C++17; the plugin does not require C++20 language features.
		CppStandard = CppStandardVersion.Cpp17;

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
			"AssetRegistry",
			"AssetTools",
			"InputCore",
			"Slate",
			"SlateCore",
			// AnimGraph: for AnimationTransitionGraph and AnimGraphNode_TransitionResult
			"AnimGraph",
		});
	}
}
