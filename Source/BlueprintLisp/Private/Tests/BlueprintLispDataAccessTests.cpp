// Copyright (c) 2026 cc8887. All Rights Reserved.
#include "Tests/BlueprintLispDataAccessTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))
#include "BlueprintLispConverter.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_GetDataTableRow.h"
#include "Kismet/BlueprintMapLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SavePackage.h"

namespace BlueprintLispDataAccessTests
{
struct FFixture { UBlueprint* BP; UEdGraph* Graph; UK2Node_FunctionEntry* Entry; };
static bool SaveEnabled() { return FParse::Param(FCommandLine::Get(), TEXT("BlueprintLispSemanticSaveFixtures")); }
static FString AssetPath(const TCHAR* Name)
{
	static const FString DefaultRun = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString Run = DefaultRun;
	FParse::Value(FCommandLine::Get(), TEXT("BlueprintLispSemanticFixtureRun="), Run);
	return FString(TEXT("/Game/BlueprintLispTests/Round2/")) + Run + TEXT("/") + Name;
}
static bool Save(UObject* Asset)
{
	if (!SaveEnabled() || Asset->GetOutermost() == GetTransientPackage()) return true;
	FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Asset->GetOutermost(), Asset, *Filename, Args);
}
static FFixture Fixture(const TCHAR* Name, bool bPersistent = false)
{
	UPackage* Package = bPersistent && SaveEnabled() ? CreatePackage(*AssetPath(Name)) : GetTransientPackage();
	FName ObjectName = Package == GetTransientPackage() ? MakeUniqueObjectName(Package, UBlueprint::StaticClass(), Name) : FName(Name);
	FFixture F = {};
	F.BP = FKismetEditorUtilities::CreateBlueprint(UBlueprintLispDataAccessTestObject::StaticClass(), Package, ObjectName,
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	F.Graph = FBlueprintEditorUtils::CreateNewGraph(F.BP, TEXT("Run"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(F.BP, F.Graph, true, nullptr);
	for (UEdGraphNode* N : F.Graph->Nodes) if (auto* E = Cast<UK2Node_FunctionEntry>(N)) F.Entry = E;
	return F;
}
static bool Link(UEdGraphPin* A, UEdGraphPin* B) { return A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B); }
static UK2Node_CallFunction* Call(const FFixture& F, const TCHAR* Name, bool bMap = false)
{
	auto* N = NewObject<UK2Node_CallFunction>(F.Graph);
	UClass* Owner = bMap ? UBlueprintMapLibrary::StaticClass() : UBlueprintLispDataAccessTestObject::StaticClass();
	N->SetFromFunction(Owner->FindFunctionByName(Name)); F.Graph->AddNode(N, false, false);
	N->CreateNewGuid(); N->AllocateDefaultPins(); return N;
}
static UEdGraphPin* Get(const FFixture& F, const TCHAR* Name)
{
	auto* N = NewObject<UK2Node_VariableGet>(F.Graph); N->VariableReference.SetSelfMember(Name);
	F.Graph->AddNode(N, false, false); N->CreateNewGuid(); N->AllocateDefaultPins(); return N->GetValuePin();
}
static FBlueprintLispConverter::FImportOptions Options(bool bMerge = false)
{
	FBlueprintLispConverter::FImportOptions O; O.bAutoLayout = false; O.bCompile = true;
	O.ImportMode = bMerge ? FBlueprintLispConverter::EImportMode::MergeAppend : FBlueprintLispConverter::EImportMode::ReplaceGraph;
	return O;
}
static UBlueprintLispDataAccessTestObject* Run(const FFixture& F, UDataTable* Table = nullptr, FName Row = TEXT("Existing"))
{
	auto* O = NewObject<UBlueprintLispDataAccessTestObject>(GetTransientPackage(), F.BP->GeneratedClass);
	O->Table = Table; O->RequestedRow = Row;
	if (UFunction* Fn = O->FindFunction(TEXT("Run"))) O->ProcessEvent(Fn, nullptr);
	return O;
}
static UDataTable* Table(const TCHAR* Name = TEXT("DT_BL_Rows"), int32 Count = 0)
{
	UPackage* Package = SaveEnabled() ? CreatePackage(*AssetPath(Name)) : GetTransientPackage();
	const FName ObjectName = Package == GetTransientPackage() ? MakeUniqueObjectName(Package, UDataTable::StaticClass(), Name) : FName(Name);
	auto* T = NewObject<UDataTable>(Package, ObjectName, RF_Public | RF_Standalone);
	T->RowStruct = FBlueprintLispDataAccessRow::StaticStruct();
	FBlueprintLispDataAccessRow R; R.Count = Count; R.Weight = 4.75f; T->AddRow(TEXT("Existing"), R);
	R.Count = 42; R.Weight = 9.25f; T->AddRow(TEXT("Other"), R);
	return T;
}
static FString TableCode(UDataTable* T)
{
	return FString::Printf(TEXT("(asset \"%s\")"), *T->GetPathName());
}
}

#define BL_DATA_TEST(Name) IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBLData##Name, "BlueprintLisp.DataAccess." #Name, \
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

BL_DATA_TEST(MapFindExplicitValueBinding)
bool FBLDataMapFindExplicitValueBinding::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispDataAccessTests;
	const auto F = Fixture(TEXT("BL_MapBinding"));
	const FString Code = TEXT("(function Run ")
		TEXT("(Map_Add :TargetMap (self.IntMap) :Key \"Hit\" :Value 0) ")
		TEXT("(let IsPresent (Map_Find :TargetMap (self.IntMap) :Key \"Hit\" :out-pin \"ReturnValue\")) ")
		TEXT("(RecordLookup :Found IsPresent :Result 0) ")
		TEXT("(Map_Add :TargetMap (self.IntMap) :Key \"Hit\" :Value 7) ")
		TEXT("(let Selected (Map_Find :TargetMap (self.IntMap) :Key \"Hit\" :out-pin \"Value\")) ")
		TEXT("(RecordLookup :Found true :Result Selected))");
	const auto R = FBlueprintLispConverter::ImportGraph(F.Graph, Code, Options());
	TestTrue(TEXT("authored Map DSL imports: ") + R.Error, R.bSuccess);
	if (!R.bSuccess) return false;
	auto* O = Run(F);
	TestTrue(TEXT("explicit Found binding is true even when the stored value is zero"), O->LookupFound == TArray<bool>({true, true}));
	TestTrue(TEXT("explicit Value binding reads seven, not the Found bool"), O->LookupValues == TArray<int32>({0, 7}));
	const auto Bad = Fixture(TEXT("BL_MapBadOutput"));
	const auto Rejected = FBlueprintLispConverter::ImportGraph(Bad.Graph,
		TEXT("(function Run (let Selected (Map_Find :TargetMap (self.IntMap) :Key \"Hit\" :out-pin \"Missing\")))"), Options());
	TestFalse(TEXT("an invalid explicit output is rejected instead of selecting the first output"), Rejected.bSuccess);
	return true;
}

BL_DATA_TEST(MapStructKeywordOrder)
bool FBLDataMapStructKeywordOrder::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispDataAccessTests;
	const auto F = Fixture(TEXT("BP_BL_MapStruct"), true);
	const FString Code = TEXT("(function Run ")
		TEXT("(Map_Add :Key \"ExactKey\" :Value (make-struct :struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\" ")
		TEXT(":field (Count int 23) :field (Weight float 2.5)) :TargetMap (self.RowMap)) ")
		TEXT("(RecordRow :Row (Map_Find :TargetMap (self.RowMap) :Key \"ExactKey\" :out-pin \"Value\")))");
	const auto R = FBlueprintLispConverter::ImportGraph(F.Graph, Code, Options());
	TestTrue(TEXT("struct-valued Map imports: ") + R.Error, R.bSuccess);
	if (!R.bSuccess) return false;
	auto* O = Run(F);
	TestTrue(TEXT("late TargetMap binding preserves the literal key"), O->RowMap.Contains(TEXT("ExactKey")));
	TestEqual(TEXT("Map_Find preserves struct value"), O->RowCount, 23);
	TestEqual(TEXT("Map_Find preserves second struct member"), O->RowWeight, 2.5f);
	TestTrue(TEXT("struct Map fixture saves"), Save(F.BP));
	return true;
}

BL_DATA_TEST(MapNativeRoundTrip)
bool FBLDataMapNativeRoundTrip::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispDataAccessTests;
	const auto Source = Fixture(TEXT("BL_MapSource"));
	UEdGraphPin* Exec = Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then);
	auto Add = [&](int32 Value)
	{
		auto* C = Call(Source, TEXT("Map_Add"), true);
		TestTrue(TEXT("native map target connects"), Link(Get(Source, TEXT("IntMap")), C->FindPin(TEXT("TargetMap"))));
		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*C->FindPin(TEXT("Key")), TEXT("Zero"));
		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*C->FindPin(TEXT("Value")), FString::FromInt(Value));
		TestTrue(TEXT("Map_Add executes"), Link(Exec, C->GetExecPin())); Exec = C->GetThenPin();
	};
	auto Find = [&](const TCHAR* Key)
	{
		auto* C = Call(Source, TEXT("Map_Find"), true);
		TestTrue(TEXT("native Map_Find target connects"), Link(Get(Source, TEXT("IntMap")), C->FindPin(TEXT("TargetMap"))));
		GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*C->FindPin(TEXT("Key")), Key);
		auto* Record = Call(Source, TEXT("RecordLookup"));
		TestTrue(TEXT("Found is a separate output"), Link(C->GetReturnValuePin(), Record->FindPin(TEXT("Found"))));
		TestTrue(TEXT("Value is a separate output"), Link(C->FindPin(TEXT("Value")), Record->FindPin(TEXT("Result"))));
		TestTrue(TEXT("Map consumer executes"), Link(Exec, Record->GetExecPin())); Exec = Record->GetThenPin();
	};
	Add(0); Find(TEXT("Zero")); Find(TEXT("Absent")); Add(37); Find(TEXT("Zero"));
	for (bool bIds : { true, false })
	{
		FBlueprintLispConverter::FExportOptions EO; EO.bStableIds = bIds;
		const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, EO);
		TestTrue(TEXT("native Map source exports: ") + Exported.Error, Exported.bSuccess);
		const auto Dest = Fixture(bIds ? TEXT("BP_BL_Map") : TEXT("BL_MapNoIds"), bIds);
		for (bool bMerge : { false, true })
		{
			const auto R = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, Options(bMerge));
			TestTrue(TEXT("native Map graph imports: ") + R.Error, R.bSuccess);
			if (!R.bSuccess) return false;
			auto* O = Run(Dest);
			TestTrue(TEXT("zero value is found, missing key is not, overwrite is found"), O->LookupFound == TArray<bool>({true, false, true}));
			TestTrue(TEXT("zero, missing default and overwritten value survive"), O->LookupValues == TArray<int32>({0, 0, 37}));
			TestEqual(TEXT("Map_Add updates the original member Map"), O->IntMap.FindRef(TEXT("Zero")), 37);
			int32 FindCount = 0;
			for (UEdGraphNode* N : Dest.Graph->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N))
				if (C->GetTargetFunction() && C->GetTargetFunction()->GetFName() == TEXT("Map_Find")) ++FindCount;
			TestEqual(TEXT("each pure Map_Find supplies both of its own outputs"), FindCount, 3);
		}
		TestTrue(TEXT("native Map fixture saves"), Save(Dest.BP));
	}
	return true;
}

BL_DATA_TEST(DataTableNativeRoundTrip)
bool FBLDataDataTableNativeRoundTrip::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispDataAccessTests;
	auto* Rows = Table(); TestTrue(TEXT("row fixture saves"), Save(Rows));
	for (bool bDynamic : { false, true }) for (bool bSplit : { false, true })
	{
		const auto Source = Fixture(TEXT("BL_TableSource"));
		auto* Row = NewObject<UK2Node_GetDataTableRow>(Source.Graph);
		Source.Graph->AddNode(Row, false, false); Row->CreateNewGuid(); Row->AllocateDefaultPins();
		if (bDynamic) TestTrue(TEXT("dynamic DataTable connects"), Link(Get(Source, TEXT("Table")), Row->GetDataTablePin()));
		else GetDefault<UEdGraphSchema_K2>()->TrySetDefaultObject(*Row->GetDataTablePin(), Rows);
		TestTrue(TEXT("row name is evaluated at runtime"), Link(Get(Source, TEXT("RequestedRow")), Row->GetRowNamePin()));
		auto* Found = Call(Source, bSplit ? TEXT("RecordRowFields") : TEXT("RecordRow"));
		if (bSplit)
		{
			Row->GetResultPin()->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Row->GetResultPin()->PinType.PinSubCategoryObject = FBlueprintLispDataAccessRow::StaticStruct();
			GetDefault<UEdGraphSchema_K2>()->SplitPin(Row->GetResultPin(), false);
			for (UEdGraphPin* P : Row->GetResultPin()->SubPins)
			{
				if (P->PinName.ToString().EndsWith(TEXT("_Count"))) TestTrue(TEXT("split Count connects"), Link(P, Found->FindPin(TEXT("Count"))));
				if (P->PinName.ToString().EndsWith(TEXT("_Weight"))) TestTrue(TEXT("split Weight connects"), Link(P, Found->FindPin(TEXT("Weight"))));
			}
		}
		else TestTrue(TEXT("OutRow connects as a struct"), Link(Row->GetResultPin(), Found->FindPin(TEXT("Row"))));
		auto* Missing = Call(Source, TEXT("RecordMissing"));
		TestTrue(TEXT("row node executes"), Link(Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then), Row->GetExecPin()));
		TestTrue(TEXT("row found has its own branch"), Link(Row->GetThenPin(), Found->GetExecPin()));
		TestTrue(TEXT("row not found has its own branch"), Link(Row->GetRowNotFoundPin(), Missing->GetExecPin()));
		FBlueprintLispConverter::FExportOptions EO; EO.bStableIds = !bSplit;
		const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, EO);
		TestTrue(TEXT("DataTable source exports: ") + Exported.Error, Exported.bSuccess);
		if (!Exported.bSuccess) return false;
		TestTrue(TEXT("DataTable exports a dedicated form"), Exported.LispCode.Contains(TEXT("get-data-table-row")));
		const auto Dest = Fixture(bDynamic ? TEXT("BP_BL_DataTableDynamic") : TEXT("BP_BL_DataTable"), !bSplit);
		for (bool bMerge : { false, true })
		{
			const auto R = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, Options(bMerge));
			TestTrue(FString::Printf(TEXT("DataTable import dynamic=%d split=%d merge=%d: "), bDynamic, bSplit, bMerge) + R.Error, R.bSuccess);
			if (!R.bSuccess) return false;
			for (FName Key : { FName(TEXT("Existing")), FName(TEXT("Other")), FName(TEXT("Missing")) })
			{
				auto* O = Run(Dest, Rows, Key); const bool bPresent = Key != TEXT("Missing");
				TestEqual(TEXT("exactly the row-found path executes"), O->FoundPaths, bPresent ? 1 : 0);
				TestEqual(TEXT("exactly the row-not-found path executes"), O->MissingPaths, bPresent ? 0 : 1);
				if (bPresent)
				{
					TestEqual(TEXT("row count is data, not an existence gate"), O->RowCount, Key == TEXT("Existing") ? 0 : 42);
					TestEqual(TEXT("second row member survives"), O->RowWeight, Key == TEXT("Existing") ? 4.75f : 9.25f);
				}
			}
			int32 RowNodes = 0;
			for (UEdGraphNode* N : Dest.Graph->Nodes) if (Cast<UK2Node_GetDataTableRow>(N)) ++RowNodes;
			TestEqual(TEXT("one DataTable node is shared by exec and OutRow"), RowNodes, 1);
		}
		TestTrue(TEXT("DataTable Blueprint fixture saves"), Save(Dest.BP));
	}
	return true;
}

BL_DATA_TEST(DataTableRejectsInvalidInputs)
bool FBLDataDataTableRejectsInvalidInputs::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispDataAccessTests;
	auto* Rows = Table(TEXT("DT_BL_RejectRows"));
	const FString Prefix = TEXT("(function Run (get-data-table-row :id \"abcd1234\" :row (self.RequestedRow) ");
	TArray<FString> Bad = {
		Prefix + TEXT(":table nil :row-struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\"))"),
		Prefix + TEXT(":table (self.Table)))"),
		Prefix + TEXT(":table (asset \"/Script/Engine.Actor\") :row-struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\"))"),
		Prefix + TEXT(":table (asset \"/Game/BlueprintLispTests/DoesNotExist.DoesNotExist\") :row-struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\"))"),
		Prefix + TEXT(":table ") + TableCode(Rows) + TEXT(" :row-struct \"/Script/CoreUObject.Vector\"))"),
		Prefix + TEXT(":table ") + TableCode(Rows) + TEXT(" :row-struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\" :found (RecordRow :Row (node-output :node \"abcd1234\" :pin \"MissingPin\"))))") };
	for (const FString& Code : Bad)
	{
		const auto F = Fixture(TEXT("BL_BadTable")); auto O = Options(); O.bCompile = false;
		const auto R = FBlueprintLispConverter::ImportGraph(F.Graph, Code, O);
		TestFalse(TEXT("invalid DataTable import is rejected"), R.bSuccess);
		TestFalse(TEXT("invalid DataTable import has a diagnostic"), R.Error.IsEmpty());
	}
	return true;
}

BL_DATA_TEST(DataTableRebindAndUnusedResult)
bool FBLDataDataTableRebindAndUnusedResult::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispDataAccessTests;
	auto* A = Table(TEXT("DT_BL_RebindA"), 3);
	auto* B = Table(TEXT("DT_BL_RebindB"), 8);
	const auto F = Fixture(TEXT("BL_RebindTable"));
	UK2Node_GetDataTableRow* First = nullptr;
	for (UDataTable* T : { A, B })
	{
		const FString Code = TEXT("(function Run (get-data-table-row :id \"e1000001\" :table ") + TableCode(T)
			+ TEXT(" :row \"Existing\" :row-struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\" ")
			+ TEXT(":found (RecordRow :Row (node-output :node \"e1000001\" :pin \"ReturnValue\")) :not-found (RecordMissing)))");
		const auto R = FBlueprintLispConverter::ImportGraph(F.Graph, Code, Options(First != nullptr));
		TestTrue(TEXT("DataTable rebind imports: ") + R.Error, R.bSuccess);
		if (!R.bSuccess) return false;
		UK2Node_GetDataTableRow* Current = nullptr;
		for (UEdGraphNode* N : F.Graph->Nodes) if (auto* Row = Cast<UK2Node_GetDataTableRow>(N)) Current = Row;
		if (First) TestTrue(TEXT("MergeAppend reuses the row node when the table changes"), Current == First);
		First = Current;
		TestEqual(TEXT("rebound table supplies new row data"), Run(F)->RowCount, T == A ? 3 : 8);
	}
	const auto Gate = Fixture(TEXT("BP_BL_DataTableGate"), true);
	const FString GateCode = TEXT("(function Run (get-data-table-row :id \"e2000002\" ")
		TEXT(":table (self.Table) :row (self.RequestedRow) :row-struct \"/Script/BlueprintLisp.BlueprintLispDataAccessRow\" ")
		TEXT(":found (RecordLookup :Found true :Result 5) :not-found (RecordMissing)))");
	for (bool bMerge : { false, true })
	{
		const auto R = FBlueprintLispConverter::ImportGraph(Gate.Graph, GateCode, Options(bMerge));
		TestTrue(TEXT("dynamic row type survives without a data consumer: ") + R.Error, R.bSuccess);
		if (!R.bSuccess) return false;
		auto* Present = Run(Gate, A); auto* Missing = Run(Gate, A, TEXT("Missing"));
		TestTrue(TEXT("gate executes the found branch"), Present->LookupValues == TArray<int32>({5}));
		TestEqual(TEXT("gate executes the missing branch"), Missing->MissingPaths, 1);
		const auto Exported = FBlueprintLispConverter::ExportGraph(Gate.Graph);
		TestTrue(TEXT("unused dynamic OutRow retains its row struct on export"), Exported.bSuccess && Exported.LispCode.Contains(TEXT("BlueprintLispDataAccessRow")));
	}
	TestTrue(TEXT("dynamic gate fixture saves"), Save(Gate.BP));
	return true;
}
#endif
