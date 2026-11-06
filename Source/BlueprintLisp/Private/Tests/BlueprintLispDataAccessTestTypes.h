// Copyright (c) 2026 cc8887. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Tests/BlueprintLispSemanticTestTypes.h"
#include "BlueprintLispDataAccessTestTypes.generated.h"

USTRUCT(BlueprintType)
struct FBlueprintLispDataAccessRow : public FTableRowBase
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	int32 Count = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	float Weight = 0;
};

UCLASS(BlueprintType, Blueprintable)
class UBlueprintLispDataAccessTestObject : public UBlueprintLispSemanticTestObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	TMap<FName, int32> IntMap;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	TMap<FName, FBlueprintLispDataAccessRow> RowMap;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	UDataTable* Table = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="BlueprintLispTests")
	FName RequestedRow = TEXT("Existing");
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	TArray<bool> LookupFound;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	TArray<int32> LookupValues;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	int32 FoundPaths = 0;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	int32 MissingPaths = 0;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	int32 RowCount = -1;
	UPROPERTY(BlueprintReadOnly, Category="BlueprintLispTests")
	float RowWeight = -1;

	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	void RecordLookup(bool Found, int32 Result)
	{
		LookupFound.Add(Found); LookupValues.Add(Result);
	}
	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	void RecordRow(const FBlueprintLispDataAccessRow& Row)
	{
		++FoundPaths; RowCount = Row.Count; RowWeight = Row.Weight;
	}
	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	void RecordRowFields(int32 Count, float Weight)
	{
		++FoundPaths; RowCount = Count; RowWeight = Weight;
	}
	UFUNCTION(BlueprintCallable, Category="BlueprintLispTests")
	void RecordMissing() { ++MissingPaths; }
};
