//===-- ResourceScriptParser.cpp --------------------------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// This implements the parser defined in ResourceScriptParser.h.
//
//===---------------------------------------------------------------------===//

#include "ResourceScriptParser.h"

// Take an expression returning llvm::Error and forward the error if it exists.
#define RETURN_IF_ERROR(Expr)                                                  \
  if (auto Err = (Expr))                                                       \
    return std::move(Err);

// Take an expression returning llvm::Expected<T> and assign it to Var or
// forward the error out of the function.
#define ASSIGN_OR_RETURN(Var, Expr)                                            \
  auto Var = (Expr);                                                           \
  if (!Var)                                                                    \
    return Var.takeError();

namespace llvm {
namespace rc {

RCParser::ParserError::ParserError(const Twine Expected, const LocIter CurLoc,
                                   const LocIter End)
    : ErrorLoc(CurLoc), FileEnd(End) {
  CurMessage = "Error parsing file: expected " + Expected.str() + ", got " +
               (CurLoc == End ? "<EOF>" : CurLoc->value()).str();
}

char RCParser::ParserError::ID = 0;

RCParser::RCParser(const std::vector<RCToken> &TokenList)
    : Tokens(TokenList), CurLoc(Tokens.begin()), End(Tokens.end()) {}

RCParser::RCParser(std::vector<RCToken> &&TokenList)
    : Tokens(std::move(TokenList)), CurLoc(Tokens.begin()), End(Tokens.end()) {}

bool RCParser::isEof() const { return CurLoc == End; }

RCParser::ParseType RCParser::parseSingleResource() {
  // The first thing we read is usually a resource's name. However, in some
  // cases (LANGUAGE and STRINGTABLE) the resources don't have their names
  // and the first token to be read is the type.
  ASSIGN_OR_RETURN(NameToken, readTypeOrName());

  if (NameToken->equalsLower("LANGUAGE"))
    return parseLanguageResource();
  else if (NameToken->equalsLower("STRINGTABLE"))
    return parseStringTableResource();

  // If it's not an unnamed resource, what we've just read is a name. Now,
  // read resource type;
  ASSIGN_OR_RETURN(TypeToken, readTypeOrName());

  ParseType Result = std::unique_ptr<RCResource>();
  (void)!Result;

  if (TypeToken->equalsLower("ACCELERATORS"))
    Result = parseAcceleratorsResource();
  else if (TypeToken->equalsLower("CURSOR"))
    Result = parseCursorResource();
  else if (TypeToken->equalsLower("ICON"))
    Result = parseIconResource();
  else if (TypeToken->equalsLower("HTML"))
    Result = parseHTMLResource();
  else if (TypeToken->equalsLower("MENU"))
    Result = parseMenuResource();
  else
    return getExpectedError("resource type", /* IsAlreadyRead = */ true);

  if (Result)
    (*Result)->setName(*NameToken);

  return Result;
}

bool RCParser::isNextTokenKind(Kind TokenKind) const {
  return !isEof() && look().kind() == TokenKind;
}

const RCToken &RCParser::look() const {
  assert(!isEof());
  return *CurLoc;
}

const RCToken &RCParser::read() {
  assert(!isEof());
  return *CurLoc++;
}

void RCParser::consume() {
  assert(!isEof());
  CurLoc++;
}

Expected<uint32_t> RCParser::readInt() {
  if (!isNextTokenKind(Kind::Int))
    return getExpectedError("integer");
  return read().intValue();
}

Expected<StringRef> RCParser::readString() {
  if (!isNextTokenKind(Kind::String))
    return getExpectedError("string");
  return read().value();
}

Expected<StringRef> RCParser::readIdentifier() {
  if (!isNextTokenKind(Kind::Identifier))
    return getExpectedError("identifier");
  return read().value();
}

Expected<IntOrString> RCParser::readIntOrString() {
  if (!isNextTokenKind(Kind::Int) && !isNextTokenKind(Kind::String))
    return getExpectedError("int or string");
  return IntOrString(read());
}

Expected<IntOrString> RCParser::readTypeOrName() {
  // We suggest that the correct resource name or type should be either an
  // identifier or an integer. The original RC tool is much more liberal.
  if (!isNextTokenKind(Kind::Identifier) && !isNextTokenKind(Kind::Int))
    return getExpectedError("int or identifier");
  return IntOrString(read());
}

Error RCParser::consumeType(Kind TokenKind) {
  if (isNextTokenKind(TokenKind)) {
    consume();
    return Error::success();
  }

  switch (TokenKind) {
#define TOKEN(TokenName)                                                       \
  case Kind::TokenName:                                                        \
    return getExpectedError(#TokenName);
#define SHORT_TOKEN(TokenName, TokenCh)                                        \
  case Kind::TokenName:                                                        \
    return getExpectedError(#TokenCh);
#include "ResourceScriptTokenList.h"
#undef SHORT_TOKEN
#undef TOKEN
  }

  llvm_unreachable("All case options exhausted.");
}

bool RCParser::consumeOptionalType(Kind TokenKind) {
  if (isNextTokenKind(TokenKind)) {
    consume();
    return true;
  }

  return false;
}

Expected<SmallVector<uint32_t, 8>>
RCParser::readIntsWithCommas(size_t MinCount, size_t MaxCount) {
  assert(MinCount <= MaxCount);

  SmallVector<uint32_t, 8> Result;

  auto FailureHandler =
      [&](llvm::Error Err) -> Expected<SmallVector<uint32_t, 8>> {
    if (Result.size() < MinCount)
      return std::move(Err);
    consumeError(std::move(Err));
    return Result;
  };

  for (size_t i = 0; i < MaxCount; ++i) {
    // Try to read a comma unless we read the first token.
    // Sometimes RC tool requires them and sometimes not. We decide to
    // always require them.
    if (i >= 1) {
      if (auto CommaError = consumeType(Kind::Comma))
        return FailureHandler(std::move(CommaError));
    }

    if (auto IntResult = readInt())
      Result.push_back(*IntResult);
    else
      return FailureHandler(IntResult.takeError());
  }

  return std::move(Result);
}

Expected<uint32_t> RCParser::parseFlags(ArrayRef<StringRef> FlagDesc) {
  assert(FlagDesc.size() <= 32 && "More than 32 flags won't fit in result.");
  assert(!FlagDesc.empty());

  uint32_t Result = 0;
  while (isNextTokenKind(Kind::Comma)) {
    consume();
    ASSIGN_OR_RETURN(FlagResult, readIdentifier());
    bool FoundFlag = false;

    for (size_t FlagId = 0; FlagId < FlagDesc.size(); ++FlagId) {
      if (!FlagResult->equals_lower(FlagDesc[FlagId]))
        continue;

      Result |= (1U << FlagId);
      FoundFlag = true;
      break;
    }

    if (!FoundFlag)
      return getExpectedError(join(FlagDesc, "/"), true);
  }

  return Result;
}

// As for now, we ignore the extended set of statements.
Expected<OptionalStmtList> RCParser::parseOptionalStatements(bool IsExtended) {
  OptionalStmtList Result;

  // The last statement is always followed by the start of the block.
  while (!isNextTokenKind(Kind::BlockBegin)) {
    ASSIGN_OR_RETURN(SingleParse, parseSingleOptionalStatement(IsExtended));
    Result.addStmt(std::move(*SingleParse));
  }

  return std::move(Result);
}

Expected<std::unique_ptr<OptionalStmt>>
RCParser::parseSingleOptionalStatement(bool) {
  ASSIGN_OR_RETURN(TypeToken, readIdentifier());
  if (TypeToken->equals_lower("CHARACTERISTICS"))
    return parseCharacteristicsStmt();
  else if (TypeToken->equals_lower("LANGUAGE"))
    return parseLanguageStmt();
  else if (TypeToken->equals_lower("VERSION"))
    return parseVersionStmt();
  else
    return getExpectedError("optional statement type, BEGIN or '{'",
                            /* IsAlreadyRead = */ true);
}

RCParser::ParseType RCParser::parseLanguageResource() {
  // Read LANGUAGE as an optional statement. If it's read correctly, we can
  // upcast it to RCResource.
  return parseLanguageStmt();
}

RCParser::ParseType RCParser::parseAcceleratorsResource() {
  ASSIGN_OR_RETURN(OptStatements, parseOptionalStatements());
  RETURN_IF_ERROR(consumeType(Kind::BlockBegin));

  auto Accels = make_unique<AcceleratorsResource>(std::move(*OptStatements));

  while (!consumeOptionalType(Kind::BlockEnd)) {
    ASSIGN_OR_RETURN(EventResult, readIntOrString());
    RETURN_IF_ERROR(consumeType(Kind::Comma));
    ASSIGN_OR_RETURN(IDResult, readInt());
    ASSIGN_OR_RETURN(FlagsResult,
                     parseFlags(AcceleratorsResource::Accelerator::OptionsStr));
    Accels->addAccelerator(*EventResult, *IDResult, *FlagsResult);
  }

  return std::move(Accels);
}

RCParser::ParseType RCParser::parseCursorResource() {
  ASSIGN_OR_RETURN(Arg, readString());
  return make_unique<CursorResource>(*Arg);
}

RCParser::ParseType RCParser::parseIconResource() {
  ASSIGN_OR_RETURN(Arg, readString());
  return make_unique<IconResource>(*Arg);
}

RCParser::ParseType RCParser::parseHTMLResource() {
  ASSIGN_OR_RETURN(Arg, readString());
  return make_unique<HTMLResource>(*Arg);
}

RCParser::ParseType RCParser::parseMenuResource() {
  ASSIGN_OR_RETURN(OptStatements, parseOptionalStatements());
  ASSIGN_OR_RETURN(Items, parseMenuItemsList());
  return make_unique<MenuResource>(std::move(*OptStatements),
                                   std::move(*Items));
}

Expected<MenuDefinitionList> RCParser::parseMenuItemsList() {
  RETURN_IF_ERROR(consumeType(Kind::BlockBegin));

  MenuDefinitionList List;

  // Read a set of items. Each item is of one of three kinds:
  //   MENUITEM SEPARATOR
  //   MENUITEM caption:String, result:Int [, menu flags]...
  //   POPUP caption:String [, menu flags]... { items... }
  while (!consumeOptionalType(Kind::BlockEnd)) {
    ASSIGN_OR_RETURN(ItemTypeResult, readIdentifier());

    bool IsMenuItem = ItemTypeResult->equals_lower("MENUITEM");
    bool IsPopup = ItemTypeResult->equals_lower("POPUP");
    if (!IsMenuItem && !IsPopup)
      return getExpectedError("MENUITEM, POPUP, END or '}'", true);

    if (IsMenuItem && isNextTokenKind(Kind::Identifier)) {
      // Now, expecting SEPARATOR.
      ASSIGN_OR_RETURN(SeparatorResult, readIdentifier());
      if (SeparatorResult->equals_lower("SEPARATOR")) {
        List.addDefinition(make_unique<MenuSeparator>());
        continue;
      }

      return getExpectedError("SEPARATOR or string", true);
    }

    // Not a separator. Read the caption.
    ASSIGN_OR_RETURN(CaptionResult, readString());

    // If MENUITEM, expect also a comma and an integer.
    uint32_t MenuResult = -1;

    if (IsMenuItem) {
      RETURN_IF_ERROR(consumeType(Kind::Comma));
      ASSIGN_OR_RETURN(IntResult, readInt());
      MenuResult = *IntResult;
    }

    ASSIGN_OR_RETURN(FlagsResult, parseFlags(MenuDefinition::OptionsStr));

    if (IsPopup) {
      // If POPUP, read submenu items recursively.
      ASSIGN_OR_RETURN(SubMenuResult, parseMenuItemsList());
      List.addDefinition(make_unique<PopupItem>(*CaptionResult, *FlagsResult,
                                                std::move(*SubMenuResult)));
      continue;
    }

    assert(IsMenuItem);
    List.addDefinition(
        make_unique<MenuItem>(*CaptionResult, MenuResult, *FlagsResult));
  }

  return std::move(List);
}

RCParser::ParseType RCParser::parseStringTableResource() {
  ASSIGN_OR_RETURN(OptStatements, parseOptionalStatements());
  RETURN_IF_ERROR(consumeType(Kind::BlockBegin));

  auto Table = make_unique<StringTableResource>(std::move(*OptStatements));

  // Read strings until we reach the end of the block.
  while (!consumeOptionalType(Kind::BlockEnd)) {
    // Each definition consists of string's ID (an integer) and a string.
    // Some examples in documentation suggest that there might be a comma in
    // between, however we strictly adhere to the single statement definition.
    ASSIGN_OR_RETURN(IDResult, readInt());
    ASSIGN_OR_RETURN(StrResult, readString());
    Table->addString(*IDResult, *StrResult);
  }

  return std::move(Table);
}

RCParser::ParseOptionType RCParser::parseLanguageStmt() {
  ASSIGN_OR_RETURN(Args, readIntsWithCommas(/* min = */ 2, /* max = */ 2));
  return make_unique<LanguageResource>((*Args)[0], (*Args)[1]);
}

RCParser::ParseOptionType RCParser::parseCharacteristicsStmt() {
  ASSIGN_OR_RETURN(Arg, readInt());
  return make_unique<CharacteristicsStmt>(*Arg);
}

RCParser::ParseOptionType RCParser::parseVersionStmt() {
  ASSIGN_OR_RETURN(Arg, readInt());
  return make_unique<VersionStmt>(*Arg);
}

Error RCParser::getExpectedError(const Twine Message, bool IsAlreadyRead) {
  return make_error<ParserError>(
      Message, IsAlreadyRead ? std::prev(CurLoc) : CurLoc, End);
}

} // namespace rc
} // namespace llvm
