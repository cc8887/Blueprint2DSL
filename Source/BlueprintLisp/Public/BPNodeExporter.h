// BPNodeExporter.h - Export UE Blueprint Node definitions to BlueprintLisp stub
// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR

/**
 * 从 UE 引擎导出蓝图节点定义为 BlueprintLisp 类型存根
 * 扫描所有 UK2Node 子类，提取引脚签名和分类信息，
 * 输出 S-expression 格式的 stub 文件供 Lint/校验使用。
 */
class BLUEPRINTLISP_API FBPNodeExporter
{
public:
	/**
	 * 蓝图节点信息
	 */
	struct FNodeInfo
	{
		FString NodeName;        // kebab-case DSL 名, e.g. "call-function", "branch"
		FString ClassName;       // e.g. "UK2Node_CallFunction"
		FString Category;        // 分类, e.g. "Flow Control", "Math"
		FString Description;     // 节点描述
		FString Keywords;        // 搜索关键词

		struct FPinInfo
		{
			FString Name;
			FString Direction;    // "input" or "output"
			FString PinType;      // "exec", "bool", "float", "int", "string", "name", "object", "struct", "wildcard"
			bool bOptional;
		};
		TArray<FPinInfo> Pins;

		bool bIsPure;            // 是否是纯函数（无执行引脚）
		bool bIsCommutative;     // 是否可交换输入
	};

	/**
	 * 扫描所有 UK2Node 子类并导出到 stub 文件
	 * @param OutputPath 输出路径
	 * @return 是否成功
	 */
	static bool ExportAllNodes(const FString& OutputPath);

	/**
	 * 扫描所有蓝图节点类
	 */
	static TArray<FNodeInfo> ScanAllBlueprintNodes();

private:
	// 命名转换: CallFunction -> call-function
	static FString ToKebabCase(const FString& PascalCase);

	// 判断引脚类型
	static FString GetPinTypeString(const struct FEdGraphPinType& PinType);
};

#endif // WITH_EDITOR
