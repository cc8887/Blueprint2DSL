// BlueprintLispToolset.cpp - UE 5.8 native MCP Toolset adapter

#include "BlueprintLispToolset.h"

FBlueprintLispPythonResult UBlueprintLispToolset::ExportBlueprintGraph(
	const FString& AssetPath,
	const FString& GraphName,
	bool bIncludePositions,
	bool bStableIds)
{
	return UBlueprintLispPythonBridge::ExportGraphToText(
		AssetPath, GraphName, bIncludePositions, bStableIds);
}

FBlueprintLispPythonResult UBlueprintLispToolset::ListBlueprintGraphs(const FString& AssetPath)
{
	return UBlueprintLispPythonBridge::ListGraphs(AssetPath);
}

FBlueprintLispPythonResult UBlueprintLispToolset::ListBlueprintMemberVariables(const FString& AssetPath)
{
	return UBlueprintLispPythonBridge::ListMemberVariables(AssetPath);
}

FBlueprintLispPythonResult UBlueprintLispToolset::InspectBlueprintMemberVariable(
	const FString& AssetPath,
	const FString& VariableName)
{
	return UBlueprintLispPythonBridge::InspectMemberVariable(AssetPath, VariableName);
}

FBlueprintLispPythonResult UBlueprintLispToolset::ValidateBlueprintLisp(const FString& DSLText)
{
	return UBlueprintLispPythonBridge::ValidateDSL(DSLText);
}

FBlueprintLispPythonResult UBlueprintLispToolset::ReplaceBlueprintGraph(
	const FString& AssetPath,
	const FString& GraphName,
	const FString& DSLText,
	bool bCompile,
	bool bSavePackage)
{
	return UBlueprintLispPythonBridge::ImportGraphFromText(
		AssetPath,
		GraphName,
		DSLText,
		EBlueprintLispPythonImportMode::ReplaceGraph,
		bCompile,
		bSavePackage);
}

FBlueprintLispPythonResult UBlueprintLispToolset::MergeBlueprintGraph(
	const FString& AssetPath,
	const FString& GraphName,
	const FString& DSLText,
	bool bCompile,
	bool bSavePackage)
{
	return UBlueprintLispPythonBridge::ImportGraphFromText(
		AssetPath,
		GraphName,
		DSLText,
		EBlueprintLispPythonImportMode::MergeAppend,
		bCompile,
		bSavePackage);
}
