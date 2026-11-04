// Copyright (c) 2026 cc8887. All Rights Reserved.
#include "Tests/BlueprintLispSemanticTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS && (ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8))
#include "BlueprintLispConverter.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "K2Node_TemporaryVariable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace BlueprintLispSemanticTests
{
struct FFixture
{
	UBlueprint* BP = nullptr;
	UEdGraph* Graph = nullptr;
	UK2Node_FunctionEntry* Entry = nullptr;
};

static FFixture MakeFixture(const TCHAR* Label, bool bPersistent = false)
{
	static const FString DefaultRun = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString FixtureRun = DefaultRun;
	FParse::Value(FCommandLine::Get(), TEXT("BlueprintLispSemanticFixtureRun="), FixtureRun);
	UPackage* Package = bPersistent
		? CreatePackage(*(FString(TEXT("/Game/BlueprintLispTests/Round1/")) + FixtureRun + TEXT("/") + Label)) : GetTransientPackage();
	const FName Name = bPersistent ? FName(Label) : MakeUniqueObjectName(Package, UBlueprint::StaticClass(), FName(Label));
	FFixture F;
	F.BP = FKismetEditorUtilities::CreateBlueprint(UBlueprintLispSemanticTestObject::StaticClass(), Package,
		Name, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	F.Graph = FBlueprintEditorUtils::CreateNewGraph(F.BP, TEXT("Run"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(F.BP, F.Graph, true, nullptr);
	for (UEdGraphNode* N : F.Graph->Nodes) if (auto* E = Cast<UK2Node_FunctionEntry>(N)) F.Entry = E;
	return F;
}

static bool Link(UEdGraphPin* A, UEdGraphPin* B)
{
	return A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A, B);
}

static UK2Node_CallFunction* Call(const FFixture& F, const TCHAR* Name)
{
	auto* N = NewObject<UK2Node_CallFunction>(F.Graph);
	N->SetFromFunction(UBlueprintLispSemanticTestObject::StaticClass()->FindFunctionByName(Name));
	F.Graph->AddNode(N, false, false);
	N->CreateNewGuid();
	N->AllocateDefaultPins();
	return N;
}

static UK2Node_VariableGet* Getter(const FFixture& F, const TCHAR* Name, bool bExternal = false)
{
	auto* N = NewObject<UK2Node_VariableGet>(F.Graph);
	if (bExternal) N->VariableReference.SetExternalMember(Name, UBlueprintLispSemanticTestObject::StaticClass());
	else N->VariableReference.SetSelfMember(Name);
	F.Graph->AddNode(N, false, false);
	N->CreateNewGuid();
	N->AllocateDefaultPins();
	return N;
}

static FBlueprintLispConverter::FImportOptions ImportOptions(bool bMerge = false)
{
	FBlueprintLispConverter::FImportOptions O;
	O.bAutoLayout = false;
	O.bCompile = true;
	O.ImportMode = bMerge ? FBlueprintLispConverter::EImportMode::MergeAppend : FBlueprintLispConverter::EImportMode::ReplaceGraph;
	return O;
}

static UBlueprintLispSemanticTestObject* Run(const FFixture& F)
{
	auto* O = NewObject<UBlueprintLispSemanticTestObject>(GetTransientPackage(), F.BP->GeneratedClass);
	O->Other = NewObject<UBlueprintLispSemanticTestObject>();
	O->Other->Value = 91.25f; // Different from self.Value: catches target loss.
	O->Other->Location = FVector(44.25, 55, 66);
	O->Other->Other = NewObject<UBlueprintLispSemanticTestObject>();
	O->Other->Other->Value = 73.5f;
	if (UFunction* Fn = O->FindFunction(TEXT("Run"))) O->ProcessEvent(Fn, nullptr);
	return O;
}

static bool SaveFixture(const FFixture& F)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("BlueprintLispSemanticSaveFixtures"))) return true;
	if (F.BP->GetOutermost() == GetTransientPackage()) return true;
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(F.BP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(F.BP->GetOutermost(), F.BP, *Filename, Args);
}
}

#define BL_SEMANTIC_TEST(Name) IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBLSemantic##Name, "BlueprintLisp.Semantic." #Name, \
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

BL_SEMANTIC_TEST(ExternalMemberRoundTrip)
bool FBLSemanticExternalMemberRoundTrip::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	const auto Source = MakeFixture(TEXT("BL_ExternalSource"));
	auto* Other = Getter(Source, TEXT("Other"));
	auto* Value = Getter(Source, TEXT("Value"), true);
	auto* Record = Call(Source, TEXT("RecordScalar"));
	TestTrue(TEXT("external target connects"), Link(Other->GetValuePin(), Value->FindPin(UEdGraphSchema_K2::PN_Self)));
	TestTrue(TEXT("external value connects"), Link(Value->GetValuePin(), Record->FindPin(TEXT("Input"))));
	TestTrue(TEXT("source execution connects"), Link(Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then), Record->GetExecPin()));
	const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph);
	TestTrue(TEXT("source exports"), Exported.bSuccess);
	TestTrue(TEXT("external getter is explicit"), Exported.LispCode.Contains(TEXT("get-member")));
	const bool bSave = FParse::Param(FCommandLine::Get(), TEXT("BlueprintLispSemanticSaveFixtures"));
	const auto Dest = MakeFixture(TEXT("BP_BL_External"), bSave);
	const auto Imported = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, ImportOptions());
	TestTrue(TEXT("external getter imports and compiles: ") + Imported.Error, Imported.bSuccess);
	if (!Imported.bSuccess) return false;
	TestEqual(TEXT("runtime reads the external object"), Run(Dest)->ObservedScalar, 91.25f);
	TestTrue(TEXT("fixture saves"), SaveFixture(Dest));
	return true;
}

BL_SEMANTIC_TEST(ClassTypeRoundTrip)
bool FBLSemanticClassTypeRoundTrip::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	const auto Source = MakeFixture(TEXT("BL_ClassSource"));
	FEdGraphPinType Type;
	Type.PinCategory = UEdGraphSchema_K2::PC_Class;
	Type.PinSubCategoryObject = UBlueprintLispSemanticTestObject::StaticClass();
	auto* Input = Source.Entry->CreateUserDefinedPin(TEXT("InputClass"), Type, EGPD_Output, false);
	auto* Result = NewObject<UK2Node_FunctionResult>(Source.Graph);
	Source.Graph->AddNode(Result, false, false);
	Result->CreateNewGuid(); Result->AllocateDefaultPins();
	auto* Output = Result->CreateUserDefinedPin(TEXT("ReturnValue"), Type, EGPD_Input, false);
	TestTrue(TEXT("class data connects"), Link(Input, Output));
	TestTrue(TEXT("class exec connects"), Link(Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then), Result->GetExecPin()));
	const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph);
	TestTrue(TEXT("class source exports"), Exported.bSuccess);
	TestTrue(TEXT("class syntax preserves category"), Exported.LispCode.Contains(TEXT("class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>")));
	const bool bSave = FParse::Param(FCommandLine::Get(), TEXT("BlueprintLispSemanticSaveFixtures"));
	const auto Dest = MakeFixture(TEXT("BP_BL_Class"), bSave);
	const auto Imported = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, ImportOptions());
	TestTrue(TEXT("class graph imports: ") + Imported.Error, Imported.bSuccess);
	if (!Imported.bSuccess) return false;
	UFunction* Fn = Dest.BP->GeneratedClass->FindFunctionByName(TEXT("Run"));
	auto* InProperty = FindFProperty<FClassProperty>(Fn, TEXT("InputClass"));
	TestNotNull(TEXT("compiled parameter remains a class property"), InProperty);
	if (InProperty) TestTrue(TEXT("class base constraint survives"), InProperty->MetaClass == UBlueprintLispSemanticTestObject::StaticClass());
	struct { UClass* InputClass; UClass* ReturnValue; } Params = { UBlueprintLispSemanticTestObject::StaticClass(), nullptr };
	auto* Instance = NewObject<UBlueprintLispSemanticTestObject>(GetTransientPackage(), Dest.BP->GeneratedClass);
	Instance->ProcessEvent(Fn, &Params);
	TestEqual(TEXT("class value passes through at runtime"), Params.ReturnValue, Params.InputClass);
	TestTrue(TEXT("class fixture saves"), SaveFixture(Dest));
	return true;
}

BL_SEMANTIC_TEST(MultipleCallOutputs)
bool FBLSemanticMultipleCallOutputs::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	const auto Source = MakeFixture(TEXT("BL_OutputSource"));
	auto* First = Call(Source, TEXT("ProduceValues"));
	auto* Second = Call(Source, TEXT("ProduceValues"));
	auto* Record = Call(Source, TEXT("RecordValues"));
	First->FindPin(TEXT("Seed"))->DefaultValue = TEXT("3");
	Second->FindPin(TEXT("Seed"))->DefaultValue = TEXT("-7");
	TestTrue(TEXT("entry executes first call"), Link(Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then), First->GetExecPin()));
	TestTrue(TEXT("first executes second call"), Link(First->GetThenPin(), Second->GetExecPin()));
	TestTrue(TEXT("second executes consumer"), Link(Second->GetThenPin(), Record->GetExecPin()));
	TestTrue(TEXT("first return connects"), Link(First->GetReturnValuePin(), Record->FindPin(TEXT("Number"))));
	TestTrue(TEXT("second bool connects"), Link(Second->FindPin(TEXT("bPositive")), Record->FindPin(TEXT("bPositive"))));
	TestTrue(TEXT("first array connects"), Link(First->FindPin(TEXT("Values")), Record->FindPin(TEXT("Values"))));
	for (const bool bStableIds : { true, false })
	{
		FBlueprintLispConverter::FExportOptions EO; EO.bStableIds = bStableIds;
		const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, EO);
		TestTrue(TEXT("multi-output source exports"), Exported.bSuccess);
		const bool bSave = bStableIds && FParse::Param(FCommandLine::Get(), TEXT("BlueprintLispSemanticSaveFixtures"));
		const auto Dest = MakeFixture(bStableIds ? TEXT("BP_BL_Outputs") : TEXT("BL_NoEditorIds"), bSave);
		for (const bool bMerge : { false, true })
		{
			const auto Imported = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, ImportOptions(bMerge));
			TestTrue(TEXT("multi-output graph imports: ") + Imported.Error, Imported.bSuccess);
			if (!Imported.bSuccess) return false;
			auto* O = Run(Dest);
			TestEqual(TEXT("exactly the two source invocations execute"), O->InvocationCount, 2);
			TestEqual(TEXT("return belongs to first invocation"), O->ObservedNumber, 6);
			TestFalse(TEXT("bool belongs to second invocation"), O->bObservedPositive);
			TestTrue(TEXT("array belongs to first invocation"), O->ObservedValues == TArray<int32>({3, 4}));
			int32 CallCount = 0;
			for (UEdGraphNode* N : Dest.Graph->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N))
				if (C->GetTargetFunction() && C->GetTargetFunction()->GetFName() == TEXT("ProduceValues")) ++CallCount;
			TestEqual(TEXT("no floating duplicate producer nodes"), CallCount, 2);
		}
		TestTrue(TEXT("output fixture saves"), SaveFixture(Dest));
	}
	return true;
}

BL_SEMANTIC_TEST(RejectsSilentLoss)
bool FBLSemanticRejectsSilentLoss::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	const auto F = MakeFixture(TEXT("BL_LossDetection"));
	FBlueprintLispConverter::FImportOptions IO = ImportOptions(); IO.bCompile = false;
	for (const FString& Code : {
		TEXT("(function Run (RecordScalar :Input))"),
		TEXT("(function Run (RecordScalar :Input 1 2))"),
		TEXT("(function Run (RecordScalar :Input (self.DoesNotExist)))"),
		TEXT("(function Run (RecordScalar :Input (node-output :node \"12345678\" :pin \"Missing\")))"),
		TEXT("(function Run (RecordScalar :Input (ProduceValues :Seed 1)))") })
	{
		const auto R = FBlueprintLispConverter::ImportGraph(F.Graph, Code, IO);
		TestFalse(TEXT("lossy import is rejected: ") + Code, R.bSuccess);
		TestFalse(TEXT("rejection has a diagnostic"), R.Error.IsEmpty());
	}
	const auto Unsupported = MakeFixture(TEXT("BL_UnsupportedData"));
	auto* Consumer = Call(Unsupported, TEXT("RecordScalar"));
	auto* Opaque = NewObject<UK2Node_TemporaryVariable>(Unsupported.Graph);
	Opaque->VariableType = Consumer->FindPin(TEXT("Input"))->PinType;
	Unsupported.Graph->AddNode(Opaque, false, false); Opaque->CreateNewGuid(); Opaque->AllocateDefaultPins();
	auto* Data = Opaque->GetVariablePin();
	TestTrue(TEXT("opaque data fixture connects"), Link(Data, Consumer->FindPin(TEXT("Input"))));
	TestTrue(TEXT("opaque consumer is reachable"), Link(Unsupported.Entry->FindPin(UEdGraphSchema_K2::PN_Then), Consumer->GetExecPin()));
	const auto Exported = FBlueprintLispConverter::ExportGraph(Unsupported.Graph);
	TestFalse(TEXT("unsupported connected data source fails export"), Exported.bSuccess);
	TestFalse(TEXT("unsupported export explains the loss"), Exported.Error.IsEmpty());
	return true;
}

BL_SEMANTIC_TEST(NestedAndSplitExternalMembers)
bool FBLSemanticNestedAndSplitExternalMembers::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	for (bool bSplit : { false, true })
	{
		const auto Source = MakeFixture(TEXT("BL_ExternalVariants"));
		auto* Other = Getter(Source, TEXT("Other"));
		auto* Member = Getter(Source, bSplit ? TEXT("Location") : TEXT("Other"), true);
		TestTrue(TEXT("first external target connects"), Link(Other->GetValuePin(), Member->FindPin(UEdGraphSchema_K2::PN_Self)));
		UEdGraphPin* ValuePin = nullptr;
		if (bSplit)
		{
			GetDefault<UEdGraphSchema_K2>()->SplitPin(Member->GetValuePin(), false);
			for (auto* Pin : Member->GetValuePin()->SubPins)
				if (Pin->PinName.ToString().EndsWith(TEXT("_X"))) ValuePin = Pin;
		}
		else
		{
			auto* Leaf = Getter(Source, TEXT("Value"), true);
			TestTrue(TEXT("nested object target connects"), Link(Member->GetValuePin(), Leaf->FindPin(UEdGraphSchema_K2::PN_Self)));
			ValuePin = Leaf->GetValuePin();
		}
		auto* Record = Call(Source, TEXT("RecordScalar"));
		TestTrue(TEXT("external variant feeds consumer"), Link(ValuePin, Record->FindPin(TEXT("Input"))));
		TestTrue(TEXT("variant executes"), Link(Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then), Record->GetExecPin()));
		const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph);
		TestTrue(TEXT("nested/split source exports: ") + Exported.Error, Exported.bSuccess);
		const auto Dest = MakeFixture(TEXT("BL_ExternalVariantsDest"));
		const auto Imported = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, ImportOptions());
		TestTrue(TEXT("nested/split source imports: ") + Imported.Error, Imported.bSuccess);
		if (!Imported.bSuccess) return false;
		TestEqual(TEXT("nested/split runtime preserves object chain"), Run(Dest)->ObservedScalar, bSplit ? 44.25f : 73.5f);
	}
	return true;
}

BL_SEMANTIC_TEST(ClassContainersAndDefaults)
bool FBLSemanticClassContainersAndDefaults::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	const auto F = MakeFixture(TEXT("BL_ClassContainers"));
	const FString Code = TEXT("(function Run ")
		TEXT(":param (Classes array<class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>>) ")
		TEXT(":param (ClassSet set<class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>>) ")
		TEXT(":param (ClassMap map<name,class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>>) ")
		TEXT(":return (ReturnValue class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>) ")
		TEXT("(return :value (ReturnValue (asset \"/Script/BlueprintLisp.BlueprintLispSemanticTestObject\"))))");
	const auto Imported = FBlueprintLispConverter::ImportGraph(F.Graph, Code, ImportOptions());
	TestTrue(TEXT("class containers and class default import: ") + Imported.Error, Imported.bSuccess);
	if (!Imported.bSuccess) return false;
	UFunction* Fn = F.BP->GeneratedClass->FindFunctionByName(TEXT("Run"));
	auto* Array = FindFProperty<FArrayProperty>(Fn, TEXT("Classes"));
	auto* Set = FindFProperty<FSetProperty>(Fn, TEXT("ClassSet"));
	auto* Map = FindFProperty<FMapProperty>(Fn, TEXT("ClassMap"));
	TestTrue(TEXT("array element remains a class reference"), Array && CastField<FClassProperty>(Array->Inner));
	TestTrue(TEXT("set element remains a class reference"), Set && CastField<FClassProperty>(Set->ElementProp));
	TestTrue(TEXT("map value remains a class reference"), Map && CastField<FClassProperty>(Map->ValueProp));
	const auto Exported = FBlueprintLispConverter::ExportGraph(F.Graph);
	TestTrue(TEXT("class containers re-export"), Exported.bSuccess);
	TestTrue(TEXT("map class base re-exports"), Exported.LispCode.Contains(TEXT("map<name,class</Script/BlueprintLisp.BlueprintLispSemanticTestObject>>")));
	FString Wrong = Code.Replace(TEXT("(asset \"/Script/BlueprintLisp.BlueprintLispSemanticTestObject\")"), TEXT("(asset \"/Script/Engine.Actor\")"));
	TestFalse(TEXT("unrelated class default is rejected"), FBlueprintLispConverter::ImportGraph(F.Graph, Wrong, ImportOptions()).bSuccess);
	return true;
}

BL_SEMANTIC_TEST(PureMultipleOutputs)
bool FBLSemanticPureMultipleOutputs::RunTest(const FString& Parameters)
{
	using namespace BlueprintLispSemanticTests;
	const auto Source = MakeFixture(TEXT("BL_PureOutputs"));
	auto* Pure = Call(Source, TEXT("PureValues"));
	auto* Record = Call(Source, TEXT("RecordValues"));
	Pure->FindPin(TEXT("Seed"))->DefaultValue = TEXT("9");
	TestTrue(TEXT("pure return connects"), Link(Pure->GetReturnValuePin(), Record->FindPin(TEXT("Number"))));
	TestTrue(TEXT("pure bool connects"), Link(Pure->FindPin(TEXT("bPositive")), Record->FindPin(TEXT("bPositive"))));
	TestTrue(TEXT("pure array connects"), Link(Pure->FindPin(TEXT("Values")), Record->FindPin(TEXT("Values"))));
	TestTrue(TEXT("pure consumer executes"), Link(Source.Entry->FindPin(UEdGraphSchema_K2::PN_Then), Record->GetExecPin()));
	for (bool bIds : { false, true })
	{
		FBlueprintLispConverter::FExportOptions EO; EO.bStableIds = bIds;
		const auto Exported = FBlueprintLispConverter::ExportGraph(Source.Graph, EO);
		const auto Dest = MakeFixture(TEXT("BL_PureOutputDest"));
		const auto Imported = FBlueprintLispConverter::ImportGraph(Dest.Graph, Exported.LispCode, ImportOptions());
		TestTrue(TEXT("pure multi-output source imports: ") + Imported.Error, Imported.bSuccess);
		if (!Imported.bSuccess) return false;
		auto* O = Run(Dest);
		TestEqual(TEXT("pure return value survives"), O->ObservedNumber, 18);
		TestTrue(TEXT("pure array value survives"), O->ObservedValues == TArray<int32>({9, 10}));
		int32 Count = 0;
		for (UEdGraphNode* N : Dest.Graph->Nodes) if (auto* C = Cast<UK2Node_CallFunction>(N))
			if (C->GetTargetFunction() && C->GetTargetFunction()->GetFName() == TEXT("PureValues")) ++Count;
		TestEqual(TEXT("one pure source node supplies all outputs"), Count, 1);
	}
	return true;
}
#endif
