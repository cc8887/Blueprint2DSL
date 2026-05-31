// Copyright (c) 2026 OpenClaw Research. All Rights Reserved.
// BlueprintLispAST.cpp - S-expression AST & Parser implementation
//
// Derived from ECABridge/BlueprintLisp.cpp (Epic Games, Experimental)
// Original author: Jon Olick

#include "BlueprintLispAST.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ============================================================================
// FLispNode
// ============================================================================

FLispNodePtr FLispNode::MakeNil()
{
	return MakeShared<FLispNode>();
}

FLispNodePtr FLispNode::MakeSymbol(const FString& Name)
{
	auto N = MakeShared<FLispNode>();
	N->Type = ELispNodeType::Symbol;
	N->StringValue = Name;
	return N;
}

FLispNodePtr FLispNode::MakeKeyword(const FString& Name)
{
	auto N = MakeShared<FLispNode>();
	N->Type = ELispNodeType::Keyword;
	N->StringValue = Name;
	return N;
}

FLispNodePtr FLispNode::MakeNumber(double Value)
{
	auto N = MakeShared<FLispNode>();
	N->Type = ELispNodeType::Number;
	N->NumberValue = Value;
	return N;
}

FLispNodePtr FLispNode::MakeString(const FString& Value)
{
	auto N = MakeShared<FLispNode>();
	N->Type = ELispNodeType::String;
	N->StringValue = Value;
	return N;
}

FLispNodePtr FLispNode::MakeList(const TArray<FLispNodePtr>& Items)
{
	auto N = MakeShared<FLispNode>();
	N->Type = ELispNodeType::List;
	N->Children = Items;
	return N;
}

bool FLispNode::IsForm(const FString& FormName) const
{
	if (!IsList() || Children.Num() == 0) return false;
	const auto& First = Children[0];
	return First.IsValid() && First->IsSymbol()
		&& First->StringValue.Equals(FormName, ESearchCase::IgnoreCase);
}

FString FLispNode::GetFormName() const
{
	if (IsList() && Children.Num() > 0 && Children[0].IsValid() && Children[0]->IsSymbol())
		return Children[0]->StringValue;
	return TEXT("");
}

FLispNodePtr FLispNode::operator[](int32 Index) const
{
	return Get(Index);
}

FLispNodePtr FLispNode::Get(int32 Index) const
{
	if (Index >= 0 && Index < Children.Num())
		return Children[Index];
	return MakeNil();
}

FLispNodePtr FLispNode::GetKeywordArg(const FString& Keyword) const
{
	if (!IsList()) return MakeNil();

	FString Key = Keyword.StartsWith(TEXT(":")) ? Keyword : (TEXT(":") + Keyword);

	for (int32 i = 0; i < Children.Num() - 1; i++)
	{
		if (Children[i].IsValid() && Children[i]->IsKeyword()
			&& Children[i]->StringValue.Equals(Key, ESearchCase::IgnoreCase))
		{
			return Children[i + 1];
		}
	}
	return MakeNil();
}

bool FLispNode::HasKeyword(const FString& Keyword) const
{
	if (!IsList()) return false;
	FString Key = Keyword.StartsWith(TEXT(":")) ? Keyword : (TEXT(":") + Keyword);
	for (const auto& Child : Children)
	{
		if (Child.IsValid() && Child->IsKeyword()
			&& Child->StringValue.Equals(Key, ESearchCase::IgnoreCase))
			return true;
	}
	return false;
}

FString FLispNode::ToString(bool bPretty, int32 IndentLevel) const
{
	FString Indent  = bPretty ? FString::ChrN(IndentLevel * 2, ' ') : TEXT("");
	FString Newline = bPretty ? TEXT("\n") : TEXT("");

	switch (Type)
	{
	case ELispNodeType::Nil:     return TEXT("nil");
	case ELispNodeType::Symbol:  return StringValue;
	case ELispNodeType::Keyword: return StringValue; // already includes ':'
	case ELispNodeType::Number:
		if (FMath::IsNearlyEqual(NumberValue, FMath::RoundToDouble(NumberValue)))
			return FString::Printf(TEXT("%lld"), (long long)(int64)NumberValue);
		return FString::SanitizeFloat(NumberValue);

	case ELispNodeType::String:
		{
			FString Esc = StringValue;
			Esc.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			Esc.ReplaceInline(TEXT("\""), TEXT("\\\""));
			Esc.ReplaceInline(TEXT("\n"), TEXT("\\n"));
			Esc.ReplaceInline(TEXT("\t"), TEXT("\\t"));
			return FString::Printf(TEXT("\"%s\""), *Esc);
		}

	case ELispNodeType::List:
		{
			if (Children.Num() == 0) return TEXT("()");

			bool bMultiLine = bPretty && Children.Num() > 3;
			if (!bMultiLine && bPretty)
			{
				for (const auto& C : Children)
					if (C.IsValid() && C->IsList() && C->Children.Num() > 2) { bMultiLine = true; break; }
			}
			FString FormName = GetFormName();
			if (bPretty && (FormName == TEXT("event") || FormName == TEXT("func")
				|| FormName == TEXT("macro") || FormName == TEXT("seq")
				|| FormName == TEXT("branch") || FormName == TEXT("foreach")
				|| FormName == TEXT("switch")))
				bMultiLine = true;

			FString Result = TEXT("(");
			if (bMultiLine)
			{
				for (int32 i = 0; i < Children.Num(); i++)
				{
					if (i == 0)
					{
						Result += Children[i].IsValid() ? Children[i]->ToString(false, 0) : TEXT("nil");
					}
					else if (Children[i].IsValid() && Children[i]->IsKeyword())
					{
						Result += Newline + Indent + TEXT("  ");
						Result += Children[i]->ToString(false, 0);
						if (i + 1 < Children.Num())
						{
							i++;
							if (Children[i].IsValid() && Children[i]->IsList() && Children[i]->Children.Num() > 2)
							{
								Result += Newline + Indent + TEXT("    ");
								Result += Children[i]->ToString(bPretty, IndentLevel + 2);
							}
							else
							{
								Result += TEXT(" ");
								Result += Children[i].IsValid() ? Children[i]->ToString(bPretty, IndentLevel + 2) : TEXT("nil");
							}
						}
					}
					else
					{
						Result += Newline + Indent + TEXT("  ");
						Result += Children[i].IsValid() ? Children[i]->ToString(bPretty, IndentLevel + 1) : TEXT("nil");
					}
				}
			}
			else
			{
				for (int32 i = 0; i < Children.Num(); i++)
				{
					if (i > 0) Result += TEXT(" ");
					Result += Children[i].IsValid() ? Children[i]->ToString(false, 0) : TEXT("nil");
				}
			}
			Result += TEXT(")");
			return Result;
		}
	}
	return TEXT("nil");
}

// ============================================================================
// FLispParseResult
// ============================================================================

FLispParseResult FLispParseResult::Success(const TArray<FLispNodePtr>& InNodes)
{
	FLispParseResult R;
	R.bSuccess = true;
	R.Nodes    = InNodes;
	return R;
}

FLispParseResult FLispParseResult::Failure(const FString& InError, int32 InLine, int32 InColumn)
{
	FLispParseResult R;
	R.bSuccess     = false;
	R.Error        = InError;
	R.ErrorLine    = InLine;
	R.ErrorColumn  = InColumn;
	return R;
}

// ============================================================================
// FLispParser
// ============================================================================

FLispParser::FLispParser(const FString& InSource)
	: Source(InSource)
{}

FLispParseResult FLispParser::Parse(const FString& Source)
{
	FLispParser P(Source);
	return P.DoParse();
}

FLispParseResult FLispParser::DoParse()
{
	TArray<FLispNodePtr> Nodes;
	SkipWhitespaceAndComments();
	while (!IsAtEnd())
	{
		FLispNodePtr Expr = ParseExpr();
		if (!Expr.IsValid()) return MakeError(TEXT("Failed to parse expression"));
		Nodes.Add(Expr);
		SkipWhitespaceAndComments();
	}
	return FLispParseResult::Success(Nodes);
}

FLispNodePtr FLispParser::ParseExpr()
{
	SkipWhitespaceAndComments();
	if (IsAtEnd()) return nullptr;
	TCHAR c = Peek();
	if (c == '(') return ParseList();
	if (c == '"') return ParseString();
	if (c == ':') return ParseSymbolOrKeyword();
	if (FChar::IsDigit(c) || (c == '-' && Position + 1 < Source.Len() && FChar::IsDigit(Source[Position + 1])))
		return ParseNumber();
	return ParseSymbolOrKeyword();
}

FLispNodePtr FLispParser::ParseList()
{
	int32 SL = Line, SC = Column;
	if (!Match('(')) return nullptr;
	TArray<FLispNodePtr> Items;
	SkipWhitespaceAndComments();
	while (!IsAtEnd() && Peek() != ')')
	{
		FLispNodePtr Item = ParseExpr();
		if (!Item.IsValid()) return nullptr;
		Items.Add(Item);
		SkipWhitespaceAndComments();
	}
	if (!Match(')')) return nullptr;
	auto N = FLispNode::MakeList(Items);
	N->Line = SL; N->Column = SC;
	return N;
}

FLispNodePtr FLispParser::ParseString()
{
	int32 SL = Line, SC = Column;
	if (!Match('"')) return nullptr;
	FString Value;
	while (!IsAtEnd() && Peek() != '"')
	{
		TCHAR c = Advance();
		if (c == '\\' && !IsAtEnd())
		{
			TCHAR E = Advance();
			switch (E)
			{
			case 'n':  Value += '\n'; break;
			case 't':  Value += '\t'; break;
			case 'r':  Value += '\r'; break;
			case '"':  Value += '"';  break;
			case '\\': Value += '\\'; break;
			default:   Value += E;    break;
			}
		}
		else Value += c;
	}
	if (!Match('"')) return nullptr;
	auto N = FLispNode::MakeString(Value);
	N->Line = SL; N->Column = SC;
	return N;
}

FLispNodePtr FLispParser::ParseNumber()
{
	int32 SL = Line, SC = Column;
	FString NumStr;
	if (Peek() == '-') NumStr += Advance();
	while (!IsAtEnd() && FChar::IsDigit(Peek())) NumStr += Advance();
	if (!IsAtEnd() && Peek() == '.' && Position + 1 < Source.Len() && FChar::IsDigit(Source[Position + 1]))
	{
		NumStr += Advance();
		while (!IsAtEnd() && FChar::IsDigit(Peek())) NumStr += Advance();
	}
	if (!IsAtEnd() && (Peek() == 'e' || Peek() == 'E'))
	{
		NumStr += Advance();
		if (!IsAtEnd() && (Peek() == '+' || Peek() == '-')) NumStr += Advance();
		while (!IsAtEnd() && FChar::IsDigit(Peek())) NumStr += Advance();
	}
	double Val = FCString::Atod(*NumStr);
	auto N = FLispNode::MakeNumber(Val);
	N->StringValue = NumStr;
	N->Line = SL; N->Column = SC;
	return N;
}

FLispNodePtr FLispParser::ParseSymbolOrKeyword()
{
	int32 SL = Line, SC = Column;
	bool bKW = (Peek() == ':');
	FString Name;
	if (bKW) Name += Advance();

	auto IsSymChar = [](TCHAR c) {
		return FChar::IsAlnum(c) || c=='_'||c=='-'||c=='+'||c=='*'||
			   c=='/'||c=='?'||c=='!'||c=='<'||c=='>'||c=='='||
			   c=='.'||c=='@'||c=='&';
	};
	while (!IsAtEnd() && IsSymChar(Peek())) Name += Advance();
	if (Name.IsEmpty() || (bKW && Name.Len() == 1)) return nullptr;

	if (Name.Equals(TEXT("nil"),   ESearchCase::IgnoreCase)) return FLispNode::MakeNil();
	if (Name.Equals(TEXT("true"),  ESearchCase::IgnoreCase)) { auto N = FLispNode::MakeSymbol(TEXT("true"));  N->Line=SL; N->Column=SC; return N; }
	if (Name.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { auto N = FLispNode::MakeSymbol(TEXT("false")); N->Line=SL; N->Column=SC; return N; }

	FLispNodePtr N = bKW ? FLispNode::MakeKeyword(Name) : FLispNode::MakeSymbol(Name);
	N->Line = SL; N->Column = SC;
	return N;
}

void FLispParser::SkipWhitespaceAndComments()
{
	while (!IsAtEnd())
	{
		TCHAR c = Peek();
		if (FChar::IsWhitespace(c)) { Advance(); }
		else if (c == ';') { while (!IsAtEnd() && Peek() != '\n') Advance(); }
		else break;
	}
}

TCHAR FLispParser::Peek() const  { return IsAtEnd() ? '\0' : Source[Position]; }
bool  FLispParser::IsAtEnd() const { return Position >= Source.Len(); }

TCHAR FLispParser::Advance()
{
	if (IsAtEnd()) return '\0';
	TCHAR c = Source[Position++];
	if (c == '\n') { Line++; Column = 1; } else Column++;
	return c;
}

bool FLispParser::Match(TCHAR Expected)
{
	if (IsAtEnd() || Peek() != Expected) return false;
	Advance(); return true;
}

FLispParseResult FLispParser::MakeError(const FString& Message)
{
	return FLispParseResult::Failure(Message, Line, Column);
}

// ============================================================================
// namespace BlueprintLisp  (utilities)
// ============================================================================

namespace BlueprintLisp
{

FString PrettyPrint(const FString& LispCode)
{
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess) return LispCode;
	FString Result;
	for (int32 i = 0; i < PR.Nodes.Num(); i++)
	{
		if (i > 0) Result += TEXT("\n\n");
		Result += PR.Nodes[i]->ToString(true, 0);
	}
	return Result;
}

FString Minify(const FString& LispCode)
{
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess) return LispCode;
	FString Result;
	for (int32 i = 0; i < PR.Nodes.Num(); i++)
	{
		if (i > 0) Result += TEXT(" ");
		Result += PR.Nodes[i]->ToString(false, 0);
	}
	return Result;
}

TArray<FString> ExtractSymbols(const FString& LispCode)
{
	TSet<FString> SymSet;
	FLispParseResult PR = FLispParser::Parse(LispCode);
	if (!PR.bSuccess) return {};

	TFunction<void(const FLispNodePtr&)> Collect = [&](const FLispNodePtr& N)
	{
		if (!N.IsValid()) return;
		if (N->IsSymbol()) SymSet.Add(N->StringValue);
		else if (N->IsList()) for (const auto& C : N->Children) Collect(C);
	};
	for (const auto& N : PR.Nodes) Collect(N);
	return SymSet.Array();
}

bool IsValidSymbol(const FString& Name)
{
	if (Name.IsEmpty()) return false;
	TCHAR First = Name[0];
	if (!FChar::IsAlpha(First) && First != '_') return false;
	for (int32 i = 1; i < Name.Len(); i++)
	{
		TCHAR c = Name[i];
		if (!FChar::IsAlnum(c) && c != '_' && c != '-') return false;
	}
	return true;
}

} // namespace BlueprintLisp
