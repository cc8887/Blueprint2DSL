// BlueprintLispMCP.Build.cs - UE 5.8 Toolset Registry adapter

using UnrealBuildTool;

public class BlueprintLispMCP : ModuleRules
{
	public BlueprintLispMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		string CppStandardName = Target.Version.MajorVersion > 5
			|| (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion >= 5)
			? "Cpp20" : "Cpp17";
		CppStandard = (CppStandardVersion)System.Enum.Parse(typeof(CppStandardVersion), CppStandardName);

		if (Target.Version.MajorVersion < 5
			|| (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 8))
		{
			throw new BuildException("BlueprintLispMCP requires Unreal Engine 5.8 or newer.");
		}

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ToolsetRegistry"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"BlueprintLisp",
			"UnrealEd",
			"Json"
		});

		// Later engine branches split the editor implementation into a separate module.
		if (Target.Version.MajorVersion > 5)
		{
			PublicDependencyModuleNames.Add("ToolsetRegistryEditor");
		}
	}
}
