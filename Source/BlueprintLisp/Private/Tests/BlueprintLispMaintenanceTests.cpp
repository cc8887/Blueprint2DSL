// Copyright (c) 2026 cc8887. All Rights Reserved.
#include "Tests/BlueprintLispSemanticTestTypes.h"
#if WITH_DEV_AUTOMATION_TESTS && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))
#include "BlueprintLispConverter.h"
#include "BlueprintLispGraphMaintenance.h"
#include "Misc/AutomationTest.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MakeArray.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace BLMaintenanceTests
{
static UBlueprint* Fixture()
{
	return FKismetEditorUtilities::CreateBlueprint(UBlueprintLispSemanticTestObject::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BL_Maintenance")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
}
static FBlueprintLispResult Import(UBlueprint* BP, const FString& Text, bool Compile = true)
{
	auto* G = FBlueprintEditorUtils::CreateNewGraph(BP, TEXT("Run"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, G, true, nullptr);
	FBlueprintLispConverter::FImportOptions O; O.bAutoLayout = false; O.bCompile = Compile;
	return FBlueprintLispConverter::Import(BP, TEXT("Run"), Text, O);
}
}
#define BL_MAINT_TEST(Name) IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBLMaintenance##Name, "BlueprintLisp.Maintenance." #Name, EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

BL_MAINT_TEST(LegacyArrayPreservesValues)
bool FBLMaintenanceLegacyArrayPreservesValues::RunTest(const FString&)
{
	auto* BP = BLMaintenanceTests::Fixture();
	const auto R = BLMaintenanceTests::Import(BP, TEXT("(function Run (RecordValues :owner self :Number 0 :bPositive false :Values \"[ 0x0, 0x11, -3 ]\"))"));
	TestTrue(*R.Error, R.bSuccess);
	if (!R.bSuccess) return false;
	auto* Object = NewObject<UBlueprintLispSemanticTestObject>(GetTransientPackage(), BP->GeneratedClass);
	Object->ProcessEvent(Object->FindFunction(TEXT("Run")), nullptr);
	TestTrue(TEXT("values survive execution"), Object->ObservedValues == TArray<int32>({0, 17, -3}));
	return true;
}

BL_MAINT_TEST(ArrayFailurePreservesConnection)
bool FBLMaintenanceArrayFailurePreservesConnection::RunTest(const FString&)
{
	auto* BP = BLMaintenanceTests::Fixture();
	const auto R = BLMaintenanceTests::Import(BP, TEXT("(function Run (RecordValues :owner self :Number 0 :bPositive false :Values \"[1,2]\"))"));
	if (!TestTrue(*R.Error, R.bSuccess)) return false;
	UEdGraph* G = BP->FunctionGraphs[0];
	UEdGraphPin* Target = nullptr;
	for (UEdGraphNode* N : G->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N))
		if (C->GetFunctionName() == TEXT("RecordValues")) Target = C->FindPin(TEXT("Values"));
	if (!TestNotNull(TEXT("array destination"), Target)) return false;
	auto* Source = Target->LinkedTo[0];
	for (const auto* Invalid : { TEXT("\"[1,,2]\""), TEXT("\"[1,garbage]\""), TEXT("\"[1,2]\" trailing") })
	{
		const auto Failure = FBlueprintLispConverter::ImportPureExpression(G, Target, Invalid);
		TestFalse(TEXT("invalid array rejected"), Failure.bSuccess);
		TestTrue(TEXT("previous array remains wired"), Target->LinkedTo.Num() == 1 && Target->LinkedTo[0] == Source);
	}
	return true;
}

BL_MAINT_TEST(EnumArrayUsesEnumerators)
bool FBLMaintenanceEnumArrayUsesEnumerators::RunTest(const FString&)
{
	auto* BP = BLMaintenanceTests::Fixture();
	BLMaintenanceTests::Import(BP, TEXT("(function Run)"), false);
	UEdGraph* G = BP->FunctionGraphs[0];
	auto* Call = NewObject<UK2Node_CallFunction>(G);
	Call->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("BoxTraceMultiForObjects")));
	G->AddNode(Call, false, false); Call->CreateNewGuid(); Call->AllocateDefaultPins();
	auto* Pin = Call->FindPin(TEXT("ObjectTypes"));
	const auto R = FBlueprintLispConverter::ImportPureExpression(G, Pin, TEXT("\"[0x0, 0x11]\""));
	if (!TestTrue(*R.Error, R.bSuccess)) return false;
	auto* Array = Cast<UK2Node_MakeArray>(Pin->LinkedTo[0]->GetOwningNode());
	if (!TestNotNull(TEXT("native array created"), Array)) return false;
	TArray<FString> Values;
	for (auto* P : Array->Pins) if (P->Direction == EGPD_Input) Values.Add(P->DefaultValue);
	TestTrue(TEXT("native enumerators preserve byte values"), Values == TArray<FString>({TEXT("ObjectTypeQuery1"), TEXT("ObjectTypeQuery18")}));
	return true;
}

BL_MAINT_TEST(MissingReferenceAndRowTypeRejected)
bool FBLMaintenanceMissingReferenceAndRowTypeRejected::RunTest(const FString&)
{
	const auto Ref = BLMaintenanceTests::Import(BLMaintenanceTests::Fixture(), TEXT("(function Run (K2_ClearAndInvalidateTimerHandle :owner \"/Script/Engine.KismetSystemLibrary\"))"), false);
	TestFalse(TEXT("unconnected writable reference rejected without compile"), Ref.bSuccess);
	TestTrue(TEXT("reference diagnostic"), Ref.Error.Contains(TEXT("reference input")));
	const auto Row = BLMaintenanceTests::Import(BLMaintenanceTests::Fixture(), TEXT("(function Run (GetDataTableRowFromName :owner \"/Script/Engine.DataTableFunctionLibrary\" :RowName \"InGame\"))"), false);
	TestFalse(TEXT("untyped generic table call rejected"), Row.bSuccess);
	TestTrue(TEXT("actionable typed-row diagnostic"), Row.Error.Contains(TEXT("get-data-table-row")));
	return true;
}

BL_MAINT_TEST(DisconnectedImpureExportRejected)
bool FBLMaintenanceDisconnectedImpureExportRejected::RunTest(const FString&)
{
	auto* BP = BLMaintenanceTests::Fixture();
	const auto R = BLMaintenanceTests::Import(BP, TEXT("(function Run (ProduceValues :owner self :Seed 3 :id \"11111111\") (RecordScalar :owner self :Input (node-output :node \"11111111\" :pin \"ReturnValue\")))"));
	if (!TestTrue(*R.Error, R.bSuccess)) return false;
	for (UEdGraphNode* N : BP->FunctionGraphs[0]->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N))
		if (C->GetFunctionName() == TEXT("ProduceValues")) C->GetExecPin()->BreakAllPinLinks();
	const auto Exported = FBlueprintLispConverter::Export(BP, TEXT("Run"));
	TestFalse(TEXT("default-substitution cannot be exported as successful semantics"), Exported.bSuccess);
	TestTrue(TEXT("execution diagnostic"), Exported.Error.Contains(TEXT("execution input")));
	return true;
}

BL_MAINT_TEST(ScheduledCallPreservesAllOutputs)
bool FBLMaintenanceScheduledCallPreservesAllOutputs::RunTest(const FString&)
{
	auto* BP = BLMaintenanceTests::Fixture();
	const auto R = BLMaintenanceTests::Import(BP, TEXT("(function Run (ProduceValues :owner self :Seed 3 :id \"11111111\") (RecordValues :owner self :Number (node-output :node \"11111111\" :pin \"ReturnValue\") :bPositive (node-output :node \"11111111\" :pin \"bPositive\") :Values (node-output :node \"11111111\" :pin \"Values\")))"));
	if (!TestTrue(*R.Error, R.bSuccess)) return false;
	UEdGraph* G = BP->FunctionGraphs[0];
	UK2Node_CallFunction* Producer = nullptr; UK2Node_CallFunction* Consumer = nullptr; UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* N : G->Nodes)
	{
		if (auto* E = Cast<UK2Node_FunctionEntry>(N)) Entry = E;
		if (auto* C = Cast<UK2Node_CallFunction>(N))
		{
			if (C->GetFunctionName() == TEXT("ProduceValues")) Producer = C;
			if (C->GetFunctionName() == TEXT("RecordValues")) Consumer = C;
		}
	}
	if (!Entry || !Producer || !Consumer) return false;
	Producer->GetExecPin()->BreakAllPinLinks(); Producer->GetThenPin()->BreakAllPinLinks();
	GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Entry->GetThenPin(), Consumer->GetExecPin());
	const auto Fixed = UBlueprintLispGraphMaintenance::ScheduleCallBeforeConsumer(BP, TEXT("Run"), Producer->GetName(), Consumer->GetName());
	if (!TestTrue(*Fixed.Message, Fixed.bSuccess)) return false;
	FKismetEditorUtilities::CompileBlueprint(BP);
	if (!TestTrue(TEXT("scheduled graph compiles"), BP->Status != BS_Error)) return false;
	auto* Object = NewObject<UBlueprintLispSemanticTestObject>(GetTransientPackage(), BP->GeneratedClass);
	Object->ProcessEvent(Object->FindFunction(TEXT("Run")), nullptr);
	TestEqual(TEXT("one execution supplies all outputs"), Object->InvocationCount, 1);
	TestEqual(TEXT("return value"), Object->ObservedNumber, 6);
	TestTrue(TEXT("out bool"), Object->bObservedPositive);
	TestTrue(TEXT("out array"), Object->ObservedValues == TArray<int32>({3,4}));
	return true;
}
#endif
