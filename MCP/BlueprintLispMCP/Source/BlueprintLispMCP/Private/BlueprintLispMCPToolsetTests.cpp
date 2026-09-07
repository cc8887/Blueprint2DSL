// BlueprintLispMCPToolsetTests.cpp - MCP exposure regression coverage

#include "BlueprintLispToolset.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintLispMCPToolsetSurfaceTest,
	"BlueprintLisp.MCP.ToolsetSurface",
	EAutomationTestFlags::EditorContext |
	EAutomationTestFlags::CommandletContext |
	EAutomationTestFlags::EngineFilter)

bool FBlueprintLispMCPToolsetSurfaceTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(
		TEXT("BlueprintLisp Toolset should be registered with Toolset Registry"),
		UToolsetRegistry::IsToolsetClassRegistered(UBlueprintLispToolset::StaticClass()));

	const FString Schema = UToolsetRegistry::GetToolsetJsonSchema(UBlueprintLispToolset::StaticClass());
	TestFalse(TEXT("Toolset schema should not be empty"), Schema.IsEmpty());

	TSharedPtr<FJsonObject> Root;
	const bool bParsed = FJsonSerializer::Deserialize(
		TJsonReaderFactory<>::Create(Schema), Root) && Root.IsValid();
	TestTrue(TEXT("Toolset schema should be valid JSON"), bParsed);
	if (!bParsed)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolEntries = nullptr;
	const bool bHasTools = Root->TryGetArrayField(TEXT("tools"), ToolEntries) && ToolEntries != nullptr;
	TestTrue(TEXT("Toolset schema should contain a tools array"), bHasTools);
	if (!bHasTools)
	{
		return false;
	}
	TestEqual(TEXT("Toolset schema tool count"), ToolEntries->Num(), 7);

	TSet<FString> ToolNames;
	for (const TSharedPtr<FJsonValue>& ToolEntry : *ToolEntries)
	{
		const TSharedPtr<FJsonObject> ToolObject = ToolEntry.IsValid() ? ToolEntry->AsObject() : nullptr;
		if (ToolObject.IsValid())
		{
			FString Name;
			if (ToolObject->TryGetStringField(TEXT("name"), Name))
			{
				int32 LastDot = INDEX_NONE;
				if (Name.FindLastChar(TEXT('.'), LastDot))
				{
					Name = Name.RightChop(LastDot + 1);
				}
				ToolNames.Add(Name);
			}
		}
	}

	const TSet<FString> ExpectedToolNames = {
		TEXT("ExportBlueprintGraph"),
		TEXT("ListBlueprintGraphs"),
		TEXT("ListBlueprintMemberVariables"),
		TEXT("InspectBlueprintMemberVariable"),
		TEXT("ValidateBlueprintLisp"),
		TEXT("ReplaceBlueprintGraph"),
		TEXT("MergeBlueprintGraph")
	};

	TestEqual(TEXT("MCP tool count"), ToolNames.Num(), ExpectedToolNames.Num());
	for (const FString& ExpectedName : ExpectedToolNames)
	{
		TestTrue(
			*FString::Printf(TEXT("MCP tool %s should be exposed"), *ExpectedName),
			ToolNames.Contains(ExpectedName));
	}

	const TSet<FString> ForbiddenToolNames = {
		TEXT("ExportGraphToFile"),
		TEXT("ExportGraphToDefaultPath"),
		TEXT("ImportGraphFromFile"),
		TEXT("UpdateGraphFromFile"),
		TEXT("ExportStub")
	};
	for (const FString& ForbiddenName : ForbiddenToolNames)
	{
		TestFalse(
			*FString::Printf(TEXT("File-backed bridge tool %s must not be exposed"), *ForbiddenName),
			ToolNames.Contains(ForbiddenName));
	}

	return true;
}
