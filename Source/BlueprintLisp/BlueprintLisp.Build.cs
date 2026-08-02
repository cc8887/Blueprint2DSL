// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

using UnrealBuildTool;

public class BlueprintLisp : ModuleRules
{
	public BlueprintLisp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 8
			? (CppStandardVersion)System.Enum.Parse(typeof(CppStandardVersion), "Cpp20")
			: CppStandardVersion.Cpp17;
		PublicDefinitions.Add("ENGINE_MAJOR_VERSION=" + Target.Version.MajorVersion);
		PublicDefinitions.Add("ENGINE_MINOR_VERSION=" + Target.Version.MinorVersion);
		if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion <= 3)
		{
			PublicDefinitions.Add("__has_feature(x)=0");
		}

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
