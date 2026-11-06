// Copyright (c) 2026 cc8887. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "BlueprintLispPythonBridge.h"
#include "BlueprintLispGraphMaintenance.generated.h"

class UBlueprint;

/** UE 5.8+ explicit, unsaved migrations. Older engines report unsupported without changing assets. */
UCLASS()
class BLUEPRINTLISP_API UBlueprintLispGraphMaintenance : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** JSON inventory with native references, pin types and connections, including broken nodes. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static FString InspectBlueprint(UBlueprint* Blueprint);

	/** Expand valid primitive array defaults and bypass obsolete float conversions. No save/compile. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static FBlueprintLispPythonResult RepairGraph(UBlueprint* Blueprint, const FString& GraphName,
		bool bRemoveIsolatedMissingVariables = false);

	/** Explicit precision migration; refreshes getter/setter pins without a modal dialog. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static bool SetMemberRealPrecision(UBlueprint* Blueprint, const FString& VariableName, bool bDoublePrecision);

	/** Explicit object-output signature migration. No body/default is synthesized. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static bool SetFunctionObjectOutputClass(UBlueprint* Blueprint, const FString& FunctionName, const FString& ClassPath);

	/** Restore a downgraded object<Class> input to an actual subclass input. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static bool SetFunctionClassInput(UBlueprint* Blueprint, const FString& FunctionName,
		const FString& ParameterName, const FString& BaseClassPath);

	/** Explicitly schedule an unexecuted call before one data consumer, cloning shared pure expressions. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static FBlueprintLispPythonResult ScheduleCallBeforeConsumer(UBlueprint* Blueprint, const FString& GraphName,
		const FString& CallNodeName, const FString& ConsumerNodeName);

	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static FBlueprintLispPythonResult BindDelegateInput(UBlueprint* Blueprint, const FString& GraphName,
		const FString& NodeName, const FString& PinName, const FString& FunctionName);

	/** Replace a single input expression using the same checked importer as full graph import. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static FBlueprintLispPythonResult SetPinExpression(UBlueprint* Blueprint, const FString& GraphName,
		const FString& NodeName, const FString& PinName, const FString& Expression);

	/** Create a missing row struct and table from explicit schema/data. Existing assets are refused. */
	UFUNCTION(BlueprintCallable, Category="BlueprintLisp|Maintenance")
	static FBlueprintLispPythonResult CreateDataTable(const FString& StructPath, const FString& TablePath,
		const FString& FieldsJson, const FString& RowsJson);
};
