//===--- ParseSIL.cpp - SIL File Parsing logic ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILParserFunctionBuilder.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Timer.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/ParseSILSupport.h"
#include "swift/Parse/Parser.h"
#include "swift/SyntaxParse/SyntaxTreeCreator.h"
#include "swift/Syntax/SyntaxArena.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Subsystems.h"
#include "swift/Syntax/SyntaxKind.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace swift::syntax;

//===----------------------------------------------------------------------===//
// SILParserState implementation
//===----------------------------------------------------------------------===//

namespace swift {
// This has to be in the 'swift' namespace because it's forward-declared for
// SILParserState.
class SILParserTUState : public SILParserTUStateBase {
public:
  explicit SILParserTUState(SILModule &M) : M(M) {}
  ~SILParserTUState();

  SILModule &M;

  /// This is all of the forward referenced functions with
  /// the location for where the reference is.
  llvm::DenseMap<Identifier,
                 std::pair<SILFunction*, SourceLoc>> ForwardRefFns;
  /// A list of all functions forward-declared by a sil_scope.
  llvm::DenseSet<SILFunction *> PotentialZombieFns;

  /// A map from textual .sil scope number to SILDebugScopes.
  llvm::DenseMap<unsigned, SILDebugScope *> ScopeSlots;

  /// Did we parse a sil_stage for this module?
  bool DidParseSILStage = false;

  bool parseDeclSIL(Parser &P) override;
  bool parseDeclSILStage(Parser &P) override;
  bool parseSILVTable(Parser &P) override;
  bool parseSILGlobal(Parser &P) override;
  bool parseSILWitnessTable(Parser &P) override;
  bool parseSILDefaultWitnessTable(Parser &P) override;
  bool parseSILCoverageMap(Parser &P) override;
  bool parseSILProperty(Parser &P) override;
  bool parseSILScope(Parser &P) override;
};
} // end namespace swift

SILParserTUState::~SILParserTUState() {
  if (!ForwardRefFns.empty()) {
    for (auto Entry : ForwardRefFns) {
      if (Entry.second.second.isValid()) {
        M.getASTContext().Diags.diagnose(Entry.second.second,
                                         diag::sil_use_of_undefined_value,
                                         Entry.first.str());
      }
    }
  }

  // Turn any debug-info-only function declarations into zombies.
  for (auto *Fn : PotentialZombieFns)
    if (Fn->isExternalDeclaration()) {
      Fn->setInlined();
      M.eraseFunction(Fn);
    }
}

SILParserState::SILParserState(SILModule *M)
    : Impl(M ? llvm::make_unique<SILParserTUState>(*M) : nullptr) {}

SILParserState::~SILParserState() = default;

void PrettyStackTraceParser::print(llvm::raw_ostream &out) const {
  out << "With parser at source location: ";
  P.Tok.getLoc().print(out, P.Context.SourceMgr);
  out << '\n';
}

static bool parseIntoSourceFileImpl(SourceFile &SF,
                                unsigned BufferID,
                                bool *Done,
                                SILParserState *SIL,
                                PersistentParserState *PersistentState,
                                DelayedParsingCallbacks *DelayedParseCB,
                                bool FullParse,
                                bool DelayBodyParsing) {
  assert((!FullParse || (SF.canBeParsedInFull() && !SIL)) &&
         "cannot parse in full with the given parameters!");

  std::shared_ptr<SyntaxTreeCreator> STreeCreator;
  if (SF.shouldBuildSyntaxTree()) {
    STreeCreator = std::make_shared<SyntaxTreeCreator>(
        SF.getASTContext().SourceMgr, BufferID,
        SF.SyntaxParsingCache, SF.getASTContext().getSyntaxArena());
  }

  // Not supported right now.
  if (SF.Kind == SourceFileKind::REPL)
    DelayBodyParsing = false;
  if (SF.hasInterfaceHash())
    DelayBodyParsing = false;
  if (SF.shouldCollectToken())
    DelayBodyParsing = false;
  if (SF.shouldBuildSyntaxTree())
    DelayBodyParsing = false;
  if (SIL)
    DelayBodyParsing = false;

  SharedTimer timer("Parsing");
  Parser P(BufferID, SF, SIL ? SIL->Impl.get() : nullptr,
           PersistentState, STreeCreator, DelayBodyParsing);
  PrettyStackTraceParser StackTrace(P);

  llvm::SaveAndRestore<bool> S(P.IsParsingInterfaceTokens,
                               SF.hasInterfaceHash());
  if (DelayedParseCB)
    P.setDelayedParsingCallbacks(DelayedParseCB);

  bool FoundSideEffects = false;
  do {
    bool hasSideEffects = P.parseTopLevel();
    FoundSideEffects = FoundSideEffects || hasSideEffects;
    *Done = P.Tok.is(tok::eof);
  } while (FullParse && !*Done);

  if (!FullParse && !*Done) {
    // Only parsed a part of the source file.
    // Add artificial EOF to be able to finalize the tree.
    P.SyntaxContext->addRawSyntax(
      ParsedRawSyntaxNode::makeDeferredMissing(tok::eof, P.Tok.getLoc()));
  }

  if (STreeCreator) {
    auto rawNode = P.finalizeSyntaxTree();
    STreeCreator->acceptSyntaxRoot(rawNode, SF);
  }

  return FoundSideEffects;
}

bool swift::parseIntoSourceFile(SourceFile &SF,
                                unsigned BufferID,
                                bool *Done,
                                SILParserState *SIL,
                                PersistentParserState *PersistentState,
                                DelayedParsingCallbacks *DelayedParseCB,
                                bool DelayBodyParsing) {
  return parseIntoSourceFileImpl(SF, BufferID, Done, SIL,
                                 PersistentState, DelayedParseCB,
                                 /*FullParse=*/SF.shouldBuildSyntaxTree(),
                                 DelayBodyParsing);
}

bool swift::parseIntoSourceFileFull(SourceFile &SF, unsigned BufferID,
                                    PersistentParserState *PersistentState,
                                    DelayedParsingCallbacks *DelayedParseCB,
                                    bool DelayBodyParsing) {
  bool Done = false;
  return parseIntoSourceFileImpl(SF, BufferID, &Done, /*SIL=*/nullptr,
                                 PersistentState, DelayedParseCB,
                                 /*FullParse=*/true, DelayBodyParsing);
}


//===----------------------------------------------------------------------===//
// SILParser
//===----------------------------------------------------------------------===//

namespace {
  struct ParsedSubstitution {
    SourceLoc loc;
    Type replacement;
  };

  struct ParsedSpecAttr {
    ArrayRef<RequirementRepr> requirements;
    bool exported;
    SILSpecializeAttr::SpecializationKind kind;
  };

  enum class ConformanceContext {
    /// A normal conformance parse.
    Ordinary,

    /// We're parsing this for a SIL witness table.
    /// Leave any generic parameter clauses in scope, and use an explicit
    /// self-conformance instead of an abstract one.
    WitnessTable,
  };

  class SILParser {
    friend SILParserTUState;
  public:
    Parser &P;
    SILModule &SILMod;
    SILParserTUState &TUState;
    SILFunction *F = nullptr;
    GenericEnvironment *ContextGenericEnv = nullptr;

  private:
    /// HadError - Have we seen an error parsing this function?
    bool HadError = false;

    /// Data structures used to perform name lookup of basic blocks.
    llvm::DenseMap<Identifier, SILBasicBlock*> BlocksByName;
    llvm::DenseMap<SILBasicBlock*,
                   std::pair<SourceLoc, Identifier>> UndefinedBlocks;

    /// Data structures used to perform name lookup for local values.
    llvm::StringMap<ValueBase*> LocalValues;
    llvm::StringMap<SourceLoc> ForwardRefLocalValues;

    /// A callback to be invoked every time a type was deserialized.
    std::function<void(Type)> ParsedTypeCallback;

    bool performTypeLocChecking(TypeLoc &T, bool IsSILType,
                                GenericEnvironment *GenericEnv = nullptr,
                                DeclContext *DC = nullptr);

    void convertRequirements(SILFunction *F, ArrayRef<RequirementRepr> From,
                             SmallVectorImpl<Requirement> &To);

    Optional<ProtocolConformanceRef>
    parseProtocolConformanceHelper(ProtocolDecl *&proto,
                                   GenericEnvironment *GenericEnv,
                                   ConformanceContext context,
                                   ProtocolDecl *defaultForProto);
  public:
    SILParser(Parser &P)
        : P(P), SILMod(static_cast<SILParserTUState *>(P.SIL)->M),
          TUState(*static_cast<SILParserTUState *>(P.SIL)),
          ParsedTypeCallback([](Type ty) {}) {}

    /// diagnoseProblems - After a function is fully parse, emit any diagnostics
    /// for errors and return true if there were any.
    bool diagnoseProblems();

    /// getGlobalNameForReference - Given a reference to a global name, look it
    /// up and return an appropriate SIL function.
    SILFunction *getGlobalNameForReference(Identifier Name,
                                           CanSILFunctionType Ty,
                                           SourceLoc Loc,
                                           bool IgnoreFwdRef = false);
    /// getGlobalNameForDefinition - Given a definition of a global name, look
    /// it up and return an appropriate SIL function.
    SILFunction *getGlobalNameForDefinition(Identifier Name,
                                            CanSILFunctionType Ty,
                                            SourceLoc Loc);

    /// getBBForDefinition - Return the SILBasicBlock for a definition of the
    /// specified block.
    SILBasicBlock *getBBForDefinition(Identifier Name, SourceLoc Loc);
    
    /// getBBForReference - return the SILBasicBlock of the specified name.  The
    /// source location is used to diagnose a failure if the block ends up never
    /// being defined.
    SILBasicBlock *getBBForReference(Identifier Name, SourceLoc Loc);

    struct UnresolvedValueName {
      StringRef Name;
      SourceLoc NameLoc;

      bool isUndef() const { return Name == "undef"; }
    };

    /// getLocalValue - Get a reference to a local value with the specified name
    /// and type.
    SILValue getLocalValue(UnresolvedValueName Name, SILType Type,
                           SILLocation L, SILBuilder &B);

    /// setLocalValue - When an instruction or block argument is defined, this
    /// method is used to register it and update our symbol table.
    void setLocalValue(ValueBase *Value, StringRef Name, SourceLoc NameLoc);

    SILDebugLocation getDebugLoc(SILBuilder & B, SILLocation Loc) {
      return SILDebugLocation(Loc, F->getDebugScope());
    }

    /// @{ Primitive parsing.

    /// \verbatim
    ///   sil-identifier ::= [A-Za-z_0-9]+
    /// \endverbatim
    bool parseSILIdentifier(Identifier &Result, SourceLoc &Loc,
                            const Diagnostic &D);

    template<typename ...DiagArgTypes, typename ...ArgTypes>
    bool parseSILIdentifier(Identifier &Result, Diag<DiagArgTypes...> ID,
                            ArgTypes... Args) {
      SourceLoc L;
      return parseSILIdentifier(Result, L, Diagnostic(ID, Args...));
    }

    template <typename T, typename... DiagArgTypes, typename... ArgTypes>
    bool parseSILIdentifierSwitch(T &Result, ArrayRef<StringRef> Strings,
                                  Diag<DiagArgTypes...> ID, ArgTypes... Args) {
      Identifier TmpResult;
      SourceLoc L;
      if (parseSILIdentifier(TmpResult, L, Diagnostic(ID, Args...))) {
        return true;
      }

      auto Iter = std::find(Strings.begin(), Strings.end(), TmpResult.str());
      if (Iter == Strings.end()) {
        P.diagnose(P.Tok, Diagnostic(ID, Args...));
        return true;
      }

      Result = T(*Iter);
      return false;
    }

    template<typename ...DiagArgTypes, typename ...ArgTypes>
    bool parseSILIdentifier(Identifier &Result, SourceLoc &L,
                            Diag<DiagArgTypes...> ID, ArgTypes... Args) {
      return parseSILIdentifier(Result, L, Diagnostic(ID, Args...));
    }

    bool parseVerbatim(StringRef identifier);

    template <typename T>
    bool parseInteger(T &Result, const Diagnostic &D) {
      if (!P.Tok.is(tok::integer_literal)) {
        P.diagnose(P.Tok, D);
        return true;
      }
      bool error = parseIntegerLiteral(P.Tok.getText(), 0, Result);
      P.consumeToken(tok::integer_literal);
      return error;
    }

    template <typename T>
    bool parseIntegerLiteral(StringRef text, unsigned radix, T &result) {
      text = prepareIntegerLiteralForParsing(text);
      return text.getAsInteger(radix, result);
    }

    StringRef prepareIntegerLiteralForParsing(StringRef text) {
      // tok::integer_literal can contain characters that the library
      // parsing routines don't expect.
      if (text.contains('_'))
        text = P.copyAndStripUnderscores(text);
      return text;
    }

    /// @}

    /// @{ Type parsing.
    bool parseASTType(CanType &result,
                      GenericEnvironment *environment = nullptr);
    bool parseASTType(CanType &result, SourceLoc &TypeLoc) {
      TypeLoc = P.Tok.getLoc();
      return parseASTType(result);
    }
    bool parseASTType(CanType &result,
                      SourceLoc &TypeLoc,
                      GenericEnvironment *env) {
      TypeLoc = P.Tok.getLoc();
      return parseASTType(result, env);
    }
    bool parseSILOwnership(ValueOwnershipKind &OwnershipKind) {
      // We parse here @ <identifier>.
      if (!P.consumeIf(tok::at_sign)) {
        // If we fail, we must have @any ownership.
        OwnershipKind = ValueOwnershipKind::Any;
        return false;
      }

      StringRef AllOwnershipKinds[3] = {"unowned", "owned",
                                        "guaranteed"};
      return parseSILIdentifierSwitch(OwnershipKind, AllOwnershipKinds,
                                      diag::expected_sil_value_ownership_kind);
    }
    bool parseSILType(SILType &Result,
                      GenericEnvironment *&parsedGenericEnv,
                      bool IsFuncDecl = false,
                      GenericEnvironment *parentGenericEnv = nullptr);
    bool parseSILType(SILType &Result) {
      GenericEnvironment *IgnoredEnv;
      return parseSILType(Result, IgnoredEnv);
    }
    bool parseSILType(SILType &Result, SourceLoc &TypeLoc) {
      TypeLoc = P.Tok.getLoc();
      return parseSILType(Result);
    }
    bool parseSILType(SILType &Result, SourceLoc &TypeLoc,
                      GenericEnvironment *&parsedGenericEnv,
                      GenericEnvironment *parentGenericEnv = nullptr) {
      TypeLoc = P.Tok.getLoc();
      return parseSILType(Result, parsedGenericEnv, false, parentGenericEnv);
    }
    /// @}

    bool parseSILDottedPath(ValueDecl *&Decl,
                            SmallVectorImpl<ValueDecl *> &values);
    bool parseSILDottedPath(ValueDecl *&Decl) {
      SmallVector<ValueDecl *, 4> values;
      return parseSILDottedPath(Decl, values);
    }
    bool parseSILDottedPathWithoutPound(ValueDecl *&Decl,
                                        SmallVectorImpl<ValueDecl *> &values);
    bool parseSILDottedPathWithoutPound(ValueDecl *&Decl) {
      SmallVector<ValueDecl *, 4> values;
      return parseSILDottedPathWithoutPound(Decl, values);
    }
    /// At the time of calling this function, we may not have the type of the
    /// Decl yet. So we return a SILDeclRef on the first lookup result and also
    /// return all the lookup results. After parsing the expected type, the
    /// caller of this function can choose the one that has the expected type.
    bool parseSILDeclRef(SILDeclRef &Result,
                         SmallVectorImpl<ValueDecl *> &values);
    bool parseSILDeclRef(SILDeclRef &Result) {
      SmallVector<ValueDecl *, 4> values;
      return parseSILDeclRef(Result, values);
    }
    bool parseSILDeclRef(SILDeclRef &Member, bool FnTypeRequired);
    bool parseGlobalName(Identifier &Name);
    bool parseValueName(UnresolvedValueName &Name);
    bool parseValueRef(SILValue &Result, SILType Ty, SILLocation Loc,
                       SILBuilder &B);
    bool parseTypedValueRef(SILValue &Result, SourceLoc &Loc, SILBuilder &B);
    bool parseTypedValueRef(SILValue &Result, SILBuilder &B) {
      SourceLoc Tmp;
      return parseTypedValueRef(Result, Tmp, B);
    }
    bool parseSILOpcode(SILInstructionKind &Opcode, SourceLoc &OpcodeLoc,
                        StringRef &OpcodeName);
    bool parseSILDebugVar(SILDebugVariable &Var);

    /// Parses the basic block arguments as part of branch instruction.
    bool parseSILBBArgsAtBranch(SmallVector<SILValue, 6> &Args, SILBuilder &B);

    bool parseSILLocation(SILLocation &L);
    bool parseScopeRef(SILDebugScope *&DS);
    bool parseSILDebugLocation(SILLocation &L, SILBuilder &B,
                               bool parsedComma = false);
    bool parseSILInstruction(SILBuilder &B);
    bool parseCallInstruction(SILLocation InstLoc,
                              SILInstructionKind Opcode, SILBuilder &B,
                              SILInstruction *&ResultVal);
    bool parseSILFunctionRef(SILLocation InstLoc, SILFunction *&ResultFn);

    bool parseSILBasicBlock(SILBuilder &B);
    bool parseKeyPathPatternComponent(KeyPathPatternComponent &component,
                                      SmallVectorImpl<SILType> &operandTypes,
                                      SourceLoc componentLoc,
                                      Identifier componentKind,
                                      SILLocation InstLoc,
                                      GenericEnvironment *patternEnv);
    bool isStartOfSILInstruction();

    bool parseSubstitutions(SmallVectorImpl<ParsedSubstitution> &parsed,
                            GenericEnvironment *GenericEnv=nullptr,
                            ProtocolDecl *defaultForProto = nullptr);

    Optional<ProtocolConformanceRef>
    parseProtocolConformance(ProtocolDecl *&proto,
                             GenericEnvironment *&genericEnv,
                             ConformanceContext context,
                             ProtocolDecl *defaultForProto);
    Optional<ProtocolConformanceRef>
    parseProtocolConformance(ProtocolDecl *defaultForProto,
                             ConformanceContext context) {
      ProtocolDecl *dummy;
      GenericEnvironment *env;
      return parseProtocolConformance(dummy, env, context, defaultForProto);
    }

    Optional<llvm::coverage::Counter>
    parseSILCoverageExpr(llvm::coverage::CounterExpressionBuilder &Builder);

    template <class T>
    struct ParsedEnum {
      Optional<T> Value;
      StringRef Name;
      SourceLoc Loc;

      bool isSet() const { return Value.hasValue(); }
      T operator*() const { return *Value; }
    };

    template <class T>
    void setEnum(ParsedEnum<T> &existing,
                 T value, StringRef name, SourceLoc loc) {
      if (existing.Value) {
        if (*existing.Value == value) {
          P.diagnose(loc, diag::duplicate_attribute, /*modifier*/ 1);
        } else {
          P.diagnose(loc, diag::mutually_exclusive_attrs, name,
                     existing.Name, /*modifier*/ 1);
        }
        P.diagnose(existing.Loc, diag::previous_attribute, /*modifier*/ 1);
      }
      existing.Value = value;
      existing.Name = name;
      existing.Loc = loc;
    }

    template <class T>
    void maybeSetEnum(bool allowed, ParsedEnum<T> &existing,
                      T value, StringRef name, SourceLoc loc) {
      if (allowed)
        setEnum(existing, value, name, loc);
      else
        P.diagnose(loc, diag::unknown_attribute, name);
    }
  };
} // end anonymous namespace

bool SILParser::parseSILIdentifier(Identifier &Result, SourceLoc &Loc,
                                   const Diagnostic &D) {
  switch (P.Tok.getKind()) {
  case tok::identifier:
  case tok::dollarident:
    Result = P.Context.getIdentifier(P.Tok.getText());
    break;
  case tok::string_literal: {
    // Drop the double quotes.
    StringRef rawString = P.Tok.getText().drop_front().drop_back();
    Result = P.Context.getIdentifier(rawString);
    break;
  }
  case tok::oper_binary_unspaced:  // fixme?
  case tok::oper_binary_spaced:
  case tok::kw_init:
    // A binary operator or `init` can be part of a SILDeclRef.
    Result = P.Context.getIdentifier(P.Tok.getText());
    break;
  default:
    // If it's some other keyword, grab an identifier for it.
    if (P.Tok.isKeyword()) {
      Result = P.Context.getIdentifier(P.Tok.getText());
      break;
    }
    P.diagnose(P.Tok, D);
    return true;
  }

  Loc = P.Tok.getLoc();
  P.consumeToken();
  return false;
}

bool SILParser::parseVerbatim(StringRef name) {
  Identifier tok;
  SourceLoc loc;

  if (parseSILIdentifier(tok, loc, diag::expected_tok_in_sil_instr, name)) {
    return true;
  }
  if (tok.str() != name) {
    P.diagnose(loc, diag::expected_tok_in_sil_instr, name);
    return true;
  }
  return false;
}

/// diagnoseProblems - After a function is fully parse, emit any diagnostics
/// for errors and return true if there were any.
bool SILParser::diagnoseProblems() {
  // Check for any uses of basic blocks that were not defined.
  if (!UndefinedBlocks.empty()) {
    // FIXME: These are going to come out in nondeterministic order.
    for (auto Entry : UndefinedBlocks)
      P.diagnose(Entry.second.first, diag::sil_undefined_basicblock_use,
                 Entry.second.second);

    HadError = true;
  }
  
  if (!ForwardRefLocalValues.empty()) {
    // FIXME: These are going to come out in nondeterministic order.
    for (auto &Entry : ForwardRefLocalValues)
      P.diagnose(Entry.second, diag::sil_use_of_undefined_value,
                 Entry.first());
    HadError = true;
  }
  
  return HadError;
}

/// getGlobalNameForDefinition - Given a definition of a global name, look
/// it up and return an appropriate SIL function.
SILFunction *SILParser::getGlobalNameForDefinition(Identifier name,
                                                   CanSILFunctionType ty,
                                                   SourceLoc sourceLoc) {
  SILParserFunctionBuilder builder(SILMod);
  auto silLoc = RegularLocation(sourceLoc);

  // Check to see if a function of this name has been forward referenced.  If so
  // complete the forward reference.
  auto iter = TUState.ForwardRefFns.find(name);
  if (iter != TUState.ForwardRefFns.end()) {
    SILFunction *fn = iter->second.first;

    // Verify that the types match up.
    if (fn->getLoweredFunctionType() != ty) {
      P.diagnose(sourceLoc, diag::sil_value_use_type_mismatch, name.str(),
                 fn->getLoweredFunctionType(), ty);
      P.diagnose(iter->second.second, diag::sil_prior_reference);
      fn = builder.createFunctionForForwardReference("" /*name*/, ty, silLoc);
    }

    assert(fn->isExternalDeclaration() && "Forward defns cannot have bodies!");
    TUState.ForwardRefFns.erase(iter);

    // Move the function to this position in the module.
    //
    // FIXME: Should we move this functionality into SILParserFunctionBuilder?
    SILMod.getFunctionList().remove(fn);
    SILMod.getFunctionList().push_back(fn);

    return fn;
  }

  // If we don't have a forward reference, make sure the function hasn't been
  // defined already.
  if (SILMod.lookUpFunction(name.str()) != nullptr) {
    P.diagnose(sourceLoc, diag::sil_value_redefinition, name.str());
    return builder.createFunctionForForwardReference("" /*name*/, ty, silLoc);
  }

  // Otherwise, this definition is the first use of this name.
  return builder.createFunctionForForwardReference(name.str(), ty, silLoc);
}

/// getGlobalNameForReference - Given a reference to a global name, look it
/// up and return an appropriate SIL function.
SILFunction *SILParser::getGlobalNameForReference(Identifier name,
                                                  CanSILFunctionType funcTy,
                                                  SourceLoc sourceLoc,
                                                  bool ignoreFwdRef) {
  SILParserFunctionBuilder builder(SILMod);
  auto silLoc = RegularLocation(sourceLoc);

  // Check to see if we have a function by this name already.
  if (SILFunction *fn = SILMod.lookUpFunction(name.str())) {
    // If so, check for matching types.
    if (fn->getLoweredFunctionType() == funcTy) {
      return fn;
    }

    P.diagnose(sourceLoc, diag::sil_value_use_type_mismatch, name.str(),
               fn->getLoweredFunctionType(), funcTy);

    return builder.createFunctionForForwardReference("" /*name*/, funcTy,
                                                     silLoc);
  }
  
  // If we didn't find a function, create a new one - it must be a forward
  // reference.
  auto *fn =
      builder.createFunctionForForwardReference(name.str(), funcTy, silLoc);
  TUState.ForwardRefFns[name] = {fn, ignoreFwdRef ? SourceLoc() : sourceLoc};
  return fn;
}


/// getBBForDefinition - Return the SILBasicBlock for a definition of the
/// specified block.
SILBasicBlock *SILParser::getBBForDefinition(Identifier Name, SourceLoc Loc) {
  // If there was no name specified for this block, just create a new one.
  if (Name.empty())
    return F->createBasicBlock();

  SILBasicBlock *&BB = BlocksByName[Name];
  // If the block has never been named yet, just create it.
  if (BB == nullptr)
    return BB = F->createBasicBlock();

  // If it already exists, it was either a forward reference or a redefinition.
  // If it is a forward reference, it should be in our undefined set.
  if (!UndefinedBlocks.erase(BB)) {
    // If we have a redefinition, return a new BB to avoid inserting
    // instructions after the terminator.
    P.diagnose(Loc, diag::sil_basicblock_redefinition, Name);
    HadError = true;
    return F->createBasicBlock();
  }

  // FIXME: Splice the block to the end of the function so they come out in the
  // right order.
  return BB;
}

/// getBBForReference - return the SILBasicBlock of the specified name.  The
/// source location is used to diagnose a failure if the block ends up never
/// being defined.
SILBasicBlock *SILParser::getBBForReference(Identifier Name, SourceLoc Loc) {
  // If the block has already been created, use it.
  SILBasicBlock *&BB = BlocksByName[Name];
  if (BB != nullptr)
    return BB;

  // Otherwise, create it and remember that this is a forward reference so
  // that we can diagnose use without definition problems.
  BB = F->createBasicBlock();
  UndefinedBlocks[BB] = {Loc, Name};
  return BB;
}

///   sil-global-name:
///     '@' identifier
bool SILParser::parseGlobalName(Identifier &Name) {
  return P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
         parseSILIdentifier(Name, diag::expected_sil_value_name);
}

/// getLocalValue - Get a reference to a local value with the specified name
/// and type.
SILValue SILParser::getLocalValue(UnresolvedValueName Name, SILType Type,
                                  SILLocation Loc, SILBuilder &B) {
  if (Name.isUndef())
    return SILUndef::get(Type, B.getFunction());

  // Check to see if this is already defined.
  ValueBase *&Entry = LocalValues[Name.Name];

  if (Entry) {
    // If this value is already defined, check it to make sure types match.
    SILType EntryTy = Entry->getType();

    if (EntryTy != Type) {
      HadError = true;
      P.diagnose(Name.NameLoc, diag::sil_value_use_type_mismatch, Name.Name,
                 EntryTy.getASTType(), Type.getASTType());
      // Make sure to return something of the requested type.
      return new (SILMod) GlobalAddrInst(getDebugLoc(B, Loc), Type);
    }

    return SILValue(Entry);
  }
  
  // Otherwise, this is a forward reference.  Create a dummy node to represent
  // it until we see a real definition.
  ForwardRefLocalValues[Name.Name] = Name.NameLoc;

  Entry = new (SILMod) GlobalAddrInst(getDebugLoc(B, Loc), Type);
  return Entry;
}

/// setLocalValue - When an instruction or block argument is defined, this
/// method is used to register it and update our symbol table.
void SILParser::setLocalValue(ValueBase *Value, StringRef Name,
                              SourceLoc NameLoc) {
  ValueBase *&Entry = LocalValues[Name];

  // If this value was already defined, it is either a redefinition, or a
  // specification for a forward referenced value.
  if (Entry) {
    if (!ForwardRefLocalValues.erase(Name)) {
      P.diagnose(NameLoc, diag::sil_value_redefinition, Name);
      HadError = true;
      return;
    }

    // If the forward reference was of the wrong type, diagnose this now.
    if (Entry->getType() != Value->getType()) {
      P.diagnose(NameLoc, diag::sil_value_def_type_mismatch, Name,
                 Entry->getType().getASTType(),
                 Value->getType().getASTType());
      HadError = true;
    } else {
      // Forward references only live here if they have a single result.
      Entry->replaceAllUsesWith(Value);
    }
    Entry = Value;
    return;
  }

  // Otherwise, just store it in our map.
  Entry = Value;
}


//===----------------------------------------------------------------------===//
// SIL Parsing Logic
//===----------------------------------------------------------------------===//

/// parseSILLinkage - Parse a linkage specifier if present.
///   sil-linkage:
///     /*empty*/          // default depends on whether this is a definition
///     'public'
///     'hidden'
///     'shared'
///     'private'
///     'public_external'
///     'hidden_external'
///     'private_external'
static bool parseSILLinkage(Optional<SILLinkage> &Result, Parser &P) {
  // Begin by initializing result to our base value of None.
  Result = None;

  // Unfortunate collision with access control keywords.
  if (P.Tok.is(tok::kw_public)) {
    Result = SILLinkage::Public;
    P.consumeToken();
    return false;
  }

  // Unfortunate collision with access control keywords.
  if (P.Tok.is(tok::kw_private)) {
    Result = SILLinkage::Private;
    P.consumeToken();
    return false;
  }

  // If we do not have an identifier, bail. All SILLinkages that we are parsing
  // are identifiers.
  if (P.Tok.isNot(tok::identifier))
    return false;

  // Then use a string switch to try and parse the identifier.
  Result = llvm::StringSwitch<Optional<SILLinkage>>(P.Tok.getText())
    .Case("non_abi", SILLinkage::PublicNonABI)
    .Case("hidden", SILLinkage::Hidden)
    .Case("shared", SILLinkage::Shared)
    .Case("public_external", SILLinkage::PublicExternal)
    .Case("hidden_external", SILLinkage::HiddenExternal)
    .Case("shared_external", SILLinkage::SharedExternal)
    .Case("private_external", SILLinkage::PrivateExternal)
    .Default(None);

  // If we succeed, consume the token.
  if (Result) {
    P.consumeToken(tok::identifier);
  }

  return false;
}

/// Given whether it's known to be a definition, resolve an optional
/// SIL linkage to a real one.
static SILLinkage resolveSILLinkage(Optional<SILLinkage> linkage,
                                    bool isDefinition) {
  if (linkage.hasValue()) {
    return linkage.getValue();
  } else if (isDefinition) {
    return SILLinkage::DefaultForDefinition;
  } else {
    return SILLinkage::DefaultForDeclaration;
  }
}

static bool parseSILOptional(StringRef &Result, SourceLoc &Loc, SILParser &SP) {
  if (SP.P.consumeIf(tok::l_square)) {
    Identifier Id;
    SP.parseSILIdentifier(Id, Loc, diag::expected_in_attribute_list);
    SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
    Result = Id.str();
    return true;
  }
  return false;
}

static bool parseSILOptional(StringRef &Result, SILParser &SP) {
  SourceLoc Loc;
  return parseSILOptional(Result, Loc, SP);
}

/// Parse an option attribute ('[' Expected ']')?
static bool parseSILOptional(bool &Result, SILParser &SP, StringRef Expected) {
  StringRef Optional;
  if (parseSILOptional(Optional, SP)) {
    if (Optional != Expected)
      return true;
    Result = true;
  }
  return false;
}

namespace {
  /// A helper class to perform lookup of IdentTypes in the
  /// current parser scope.
  class IdentTypeReprLookup : public ASTWalker {
    Parser &P;
  public:
    IdentTypeReprLookup(Parser &P) : P(P) {}

    bool walkToTypeReprPre(TypeRepr *Ty) override {
      auto *T = dyn_cast_or_null<IdentTypeRepr>(Ty);
      auto Comp = T->getComponentRange().front();
      if (auto Entry = P.lookupInScope(Comp->getIdentifier()))
        if (auto *TD = dyn_cast<TypeDecl>(Entry)) {
          Comp->setValue(TD, nullptr);
          return false;
        }
      return true;
    }
  };
} // end anonymous namespace

/// Remap RequirementReps to Requirements.
void SILParser::convertRequirements(SILFunction *F,
                                    ArrayRef<RequirementRepr> From,
                                    SmallVectorImpl<Requirement> &To) {
  if (From.empty()) {
    To.clear();
    return;
  }

  auto *GenericEnv = F->getGenericEnvironment();
  assert(GenericEnv);
  (void)GenericEnv;

  IdentTypeReprLookup PerformLookup(P);
  // Use parser lexical scopes to resolve references
  // to the generic parameters.
  auto ResolveToInterfaceType = [&](TypeLoc Ty) -> Type {
    Ty.getTypeRepr()->walk(PerformLookup);
    performTypeLocChecking(Ty, /* IsSIL */ false);
    assert(Ty.getType());
    return Ty.getType()->mapTypeOutOfContext();
  };

  for (auto &Req : From) {
    if (Req.getKind() == RequirementReprKind::SameType) {
      auto FirstType = ResolveToInterfaceType(Req.getFirstTypeLoc());
      auto SecondType = ResolveToInterfaceType(Req.getSecondTypeLoc());
      Requirement ConvertedRequirement(RequirementKind::SameType, FirstType,
                                       SecondType);
      To.push_back(ConvertedRequirement);
      continue;
    }

    if (Req.getKind() == RequirementReprKind::TypeConstraint) {
      auto Subject = ResolveToInterfaceType(Req.getSubjectLoc());
      auto Constraint = ResolveToInterfaceType(Req.getConstraintLoc());
      Requirement ConvertedRequirement(RequirementKind::Conformance, Subject,
                                       Constraint);
      To.push_back(ConvertedRequirement);
      continue;
    }

    if (Req.getKind() == RequirementReprKind::LayoutConstraint) {
      auto Subject = ResolveToInterfaceType(Req.getSubjectLoc());
      Requirement ConvertedRequirement(RequirementKind::Layout, Subject,
                                       Req.getLayoutConstraint());
      To.push_back(ConvertedRequirement);
      continue;
    }
    llvm_unreachable("Unsupported requirement kind");
  }
}

static bool parseDeclSILOptional(bool *isTransparent,
                                 IsSerialized_t *isSerialized,
                                 bool *isCanonical,
                                 bool *hasOwnershipSSA,
                                 IsThunk_t *isThunk,
                                 IsDynamicallyReplaceable_t *isDynamic,
                                 SILFunction **dynamicallyReplacedFunction,
                                 Identifier *objCReplacementFor,
                                 bool *isGlobalInit,
                                 Inline_t *inlineStrategy,
                                 OptimizationMode *optimizationMode,
                                 bool *isLet, bool *isWeakLinked,
                                 bool *isWithoutActuallyEscapingThunk,
                                 SmallVectorImpl<std::string> *Semantics,
                                 SmallVectorImpl<ParsedSpecAttr> *SpecAttrs,
                                 ValueDecl **ClangDecl,
                                 EffectsKind *MRK, SILParser &SP,
                                 SILModule &M) {
  while (SP.P.consumeIf(tok::l_square)) {
    if (isLet && SP.P.Tok.is(tok::kw_let)) {
      *isLet = true;
      SP.P.consumeToken(tok::kw_let);
      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    }
    else if (SP.P.Tok.isNot(tok::identifier)) {
      SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
      return true;
    } else if (isTransparent && SP.P.Tok.getText() == "transparent")
      *isTransparent = true;
    else if (isSerialized && SP.P.Tok.getText() == "serialized")
      *isSerialized = IsSerialized;
    else if (isDynamic && SP.P.Tok.getText() == "dynamically_replacable")
      *isDynamic = IsDynamic;
    else if (isSerialized && SP.P.Tok.getText() == "serializable")
      *isSerialized = IsSerializable;
    else if (isCanonical && SP.P.Tok.getText() == "canonical")
      *isCanonical = true;
    else if (hasOwnershipSSA && SP.P.Tok.getText() == "ossa")
      *hasOwnershipSSA = true;
    else if (isThunk && SP.P.Tok.getText() == "thunk")
      *isThunk = IsThunk;
    else if (isThunk && SP.P.Tok.getText() == "signature_optimized_thunk")
      *isThunk = IsSignatureOptimizedThunk;
    else if (isThunk && SP.P.Tok.getText() == "reabstraction_thunk")
      *isThunk = IsReabstractionThunk;
    else if (isWithoutActuallyEscapingThunk
             && SP.P.Tok.getText() == "without_actually_escaping")
      *isWithoutActuallyEscapingThunk = true;
    else if (isGlobalInit && SP.P.Tok.getText() == "global_init")
      *isGlobalInit = true;
    else if (isWeakLinked && SP.P.Tok.getText() == "_weakLinked")
      if (M.getASTContext().LangOpts.Target.isOSBinFormatCOFF())
        SP.P.diagnose(SP.P.Tok, diag::attr_unsupported_on_target,
                      SP.P.Tok.getText(),
                      M.getASTContext().LangOpts.Target.str());
      else
        *isWeakLinked = true;
    else if (inlineStrategy && SP.P.Tok.getText() == "noinline")
      *inlineStrategy = NoInline;
    else if (optimizationMode && SP.P.Tok.getText() == "Onone")
      *optimizationMode = OptimizationMode::NoOptimization;
    else if (optimizationMode && SP.P.Tok.getText() == "Ospeed")
      *optimizationMode = OptimizationMode::ForSpeed;
    else if (optimizationMode && SP.P.Tok.getText() == "Osize")
      *optimizationMode = OptimizationMode::ForSize;
    else if (inlineStrategy && SP.P.Tok.getText() == "always_inline")
      *inlineStrategy = AlwaysInline;
    else if (MRK && SP.P.Tok.getText() == "readnone")
      *MRK = EffectsKind::ReadNone;
    else if (MRK && SP.P.Tok.getText() == "readonly")
      *MRK = EffectsKind::ReadOnly;
    else if (MRK && SP.P.Tok.getText() == "readwrite")
      *MRK = EffectsKind::ReadWrite;
    else if (MRK && SP.P.Tok.getText() == "releasenone")
      *MRK = EffectsKind::ReleaseNone;
    else if  (dynamicallyReplacedFunction && SP.P.Tok.getText() == "dynamic_replacement_for") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
      // Drop the double quotes.
      StringRef replacedFunc = SP.P.Tok.getText().drop_front().drop_back();
      SILFunction *Func = M.lookUpFunction(replacedFunc.str());
      if (!Func) {
        Identifier Id = SP.P.Context.getIdentifier(replacedFunc);
        SP.P.diagnose(SP.P.Tok, diag::sil_dynamically_replaced_func_not_found,
                      Id);
        return true;
      }
      *dynamicallyReplacedFunction = Func;
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (objCReplacementFor &&
               SP.P.Tok.getText() == "objc_replacement_for") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
      // Drop the double quotes.
      StringRef replacedFunc = SP.P.Tok.getText().drop_front().drop_back();
      *objCReplacementFor = SP.P.Context.getIdentifier(replacedFunc);
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (Semantics && SP.P.Tok.getText() == "_semantics") {
      SP.P.consumeToken(tok::identifier);
      if (SP.P.Tok.getKind() != tok::string_literal) {
        SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
        return true;
      }
  
      // Drop the double quotes.
      StringRef rawString = SP.P.Tok.getText().drop_front().drop_back();
      Semantics->push_back(rawString);
      SP.P.consumeToken(tok::string_literal);

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    } else if (SpecAttrs && SP.P.Tok.getText() == "_specialize") {
      SourceLoc AtLoc = SP.P.Tok.getLoc();
      SourceLoc Loc(AtLoc);

      // Parse a _specialized attribute, building a parsed substitution list
      // and pushing a new ParsedSpecAttr on the SpecAttrs list. Conformances
      // cannot be generated until the function declaration is fully parsed so
      // that the function's generic signature can be consulted.
      ParsedSpecAttr SpecAttr;
      SpecAttr.requirements = {};
      SpecAttr.exported = false;
      SpecAttr.kind = SILSpecializeAttr::SpecializationKind::Full;
      SpecializeAttr *Attr;

      if (!SP.P.parseSpecializeAttribute(tok::r_square, AtLoc, Loc, Attr))
        return true;

      // Convert SpecializeAttr into ParsedSpecAttr.
      SpecAttr.requirements = Attr->getTrailingWhereClause()->getRequirements();
      SpecAttr.kind = Attr->getSpecializationKind() ==
                              swift::SpecializeAttr::SpecializationKind::Full
                          ? SILSpecializeAttr::SpecializationKind::Full
                          : SILSpecializeAttr::SpecializationKind::Partial;
      SpecAttr.exported = Attr->isExported();
      SpecAttrs->emplace_back(SpecAttr);
      continue;
    }
    else if (ClangDecl && SP.P.Tok.getText() == "clang") {
      SP.P.consumeToken(tok::identifier);
      if (SP.parseSILDottedPathWithoutPound(*ClangDecl))
        return true;

      SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
      continue;
    }
    else {
      SP.P.diagnose(SP.P.Tok, diag::expected_in_attribute_list);
      return true;
    }
    SP.P.consumeToken(tok::identifier);
    SP.P.parseToken(tok::r_square, diag::expected_in_attribute_list);
  }
  return false;
}

bool SILParser::performTypeLocChecking(TypeLoc &T, bool IsSILType,
                                       GenericEnvironment *GenericEnv,
                                       DeclContext *DC) {
  // Do some type checking / name binding for the parsed type.
  assert(P.SF.ASTStage == SourceFile::Parsing &&
         "Unexpected stage during parsing!");

  if (GenericEnv == nullptr)
    GenericEnv = ContextGenericEnv;

  if (!DC)
    DC = &P.SF;
  else if (!GenericEnv)
    GenericEnv = DC->getGenericEnvironmentOfContext();

  return swift::performTypeLocChecking(P.Context, T,
                                       /*isSILMode=*/true, IsSILType,
                                       GenericEnv, DC);
}

/// Find the top-level ValueDecl or Module given a name.
static llvm::PointerUnion<ValueDecl *, ModuleDecl *>
lookupTopDecl(Parser &P, DeclBaseName Name) {
  // Use UnqualifiedLookup to look through all of the imports.
  // We have to lie and say we're done with parsing to make this happen.
  assert(P.SF.ASTStage == SourceFile::Parsing &&
         "Unexpected stage during parsing!");
  llvm::SaveAndRestore<SourceFile::ASTStage_t> ASTStage(P.SF.ASTStage,
                                                        SourceFile::Parsed);
  UnqualifiedLookup DeclLookup(Name, &P.SF, nullptr);
  assert(DeclLookup.isSuccess() && DeclLookup.Results.size() == 1);
  ValueDecl *VD = DeclLookup.Results.back().getValueDecl();
  return VD;
}

/// Find the ValueDecl given an interface type and a member name.
static ValueDecl *lookupMember(Parser &P, Type Ty, DeclBaseName Name,
                               SourceLoc Loc,
                               SmallVectorImpl<ValueDecl *> &Lookup,
                               bool ExpectMultipleResults) {
  Type CheckTy = Ty;
  if (auto MetaTy = CheckTy->getAs<AnyMetatypeType>())
    CheckTy = MetaTy->getInstanceType();

  if (auto nominal = CheckTy->getAnyNominal()) {
    auto found = nominal->lookupDirect(Name);
    Lookup.append(found.begin(), found.end());
  } else if (auto moduleTy = CheckTy->getAs<ModuleType>()) {
    moduleTy->getModule()->lookupValue({ }, Name, NLKind::QualifiedLookup,
                                       Lookup);
  } else {
    P.diagnose(Loc, diag::sil_member_lookup_bad_type, Name, Ty);
    return nullptr;
  }

  if (Lookup.empty() || (!ExpectMultipleResults && Lookup.size() != 1)) {
    P.diagnose(Loc, diag::sil_named_member_decl_not_found, Name, Ty);
    return nullptr;
  }
  return Lookup[0];
}

bool SILParser::parseASTType(CanType &result, GenericEnvironment *env) {
  ParserResult<TypeRepr> parsedType = P.parseType();
  if (parsedType.isNull()) return true;
  TypeLoc loc = parsedType.get();
  if (performTypeLocChecking(loc, /*IsSILType=*/ false, env))
    return true;

  if (env)
    result = loc.getType()->mapTypeOutOfContext()->getCanonicalType();
  else
    result = loc.getType()->getCanonicalType();

  // Invoke the callback on the parsed type.
  ParsedTypeCallback(loc.getType());
  return false;
}

///   sil-type:
///     '$' '*'? attribute-list (generic-params)? type
///
bool SILParser::parseSILType(SILType &Result,
                             GenericEnvironment *&ParsedGenericEnv,
                             bool IsFuncDecl,
                             GenericEnvironment *OuterGenericEnv) {
  ParsedGenericEnv = nullptr;

  if (P.parseToken(tok::sil_dollar, diag::expected_sil_type))
    return true;

  // If we have a '*', then this is an address type.
  SILValueCategory category = SILValueCategory::Object;
  if (P.Tok.isAnyOperator() && P.Tok.getText().startswith("*")) {
    category = SILValueCategory::Address;
    P.consumeStartingCharacterOfCurrentToken();
  }

  // Parse attributes.
  ParamDecl::Specifier specifier;
  SourceLoc specifierLoc;
  TypeAttributes attrs;
  P.parseTypeAttributeList(specifier, specifierLoc, attrs);

  // Global functions are implicitly @convention(thin) if not specified otherwise.
  if (IsFuncDecl && !attrs.has(TAK_convention)) {
    // Use a random location.
    attrs.setAttr(TAK_convention, P.PreviousLoc);
    attrs.convention = "thin";
  }

  ParserResult<TypeRepr> TyR = P.parseType(diag::expected_sil_type,
                                           /*handleCodeCompletion*/ true,
                                           /*isSILFuncDecl*/ IsFuncDecl);

  if (TyR.isNull())
    return true;
  
  // Resolve the generic environments for parsed generic function and box types.
  class HandleSILGenericParamsWalker : public ASTWalker {
    ASTContext &C;
    SourceFile *SF;
  public:
    HandleSILGenericParamsWalker(ASTContext &C,
                                 SourceFile *SF)
      : C(C), SF(SF)
    {}
    
    bool walkToTypeReprPre(TypeRepr *T) override {
      if (auto fnType = dyn_cast<FunctionTypeRepr>(T)) {
        if (auto generics = fnType->getGenericParams()) {
          auto env = handleSILGenericParams(C, generics, SF);
          fnType->setGenericEnvironment(env);
        }
      }
      if (auto boxType = dyn_cast<SILBoxTypeRepr>(T)) {
        if (auto generics = boxType->getGenericParams()) {
          auto env = handleSILGenericParams(C, generics, SF);
          boxType->setGenericEnvironment(env);
        }
      }
      return true;
    }
  };

  TyR.get()
    ->walk(HandleSILGenericParamsWalker(P.Context, &P.SF));
  
  // Save the top-level function generic environment if there was one.
  if (auto fnType = dyn_cast<FunctionTypeRepr>(TyR.get()))
    if (auto env = fnType->getGenericEnvironment())
      ParsedGenericEnv = env;
  
  // Apply attributes to the type.
  TypeLoc Ty = P.applyAttributeToType(TyR.get(), attrs, specifier, specifierLoc);

  if (performTypeLocChecking(Ty, /*IsSILType=*/true, OuterGenericEnv))
    return true;

  Result = SILType::getPrimitiveType(Ty.getType()->getCanonicalType(),
                                     category);

  // Invoke the callback on the parsed type.
  ParsedTypeCallback(Ty.getType());

  return false;
}

bool SILParser::parseSILDottedPath(ValueDecl *&Decl,
                                   SmallVectorImpl<ValueDecl *> &values) {
  if (P.parseToken(tok::pound, diag::expected_sil_constant))
    return true;
  return parseSILDottedPathWithoutPound(Decl, values);
}

bool SILParser::parseSILDottedPathWithoutPound(ValueDecl *&Decl,
                                   SmallVectorImpl<ValueDecl *> &values) {
  // Handle sil-dotted-path.
  Identifier Id;
  SmallVector<DeclBaseName, 4> FullName;
  SmallVector<SourceLoc, 4> Locs;
  do {
    Locs.push_back(P.Tok.getLoc());
    switch (P.Tok.getKind()) {
    case tok::kw_subscript:
      P.consumeToken();
      FullName.push_back(DeclBaseName::createSubscript());
      break;
    case tok::kw_init:
      P.consumeToken();
      FullName.push_back(DeclBaseName::createConstructor());
      break;
    case tok::kw_deinit:
      P.consumeToken();
      FullName.push_back(DeclBaseName::createDestructor());
      break;
    default:
      if (parseSILIdentifier(Id, diag::expected_sil_constant))
        return true;
      FullName.push_back(Id);
      break;
    }
  } while (P.consumeIf(tok::period));

  // Look up ValueDecl from a dotted path.
  ValueDecl *VD;
  llvm::PointerUnion<ValueDecl*, ModuleDecl *> Res = lookupTopDecl(P, FullName[0]);
  // It is possible that the last member lookup can return multiple lookup
  // results. One example is the overloaded member functions.
  if (Res.is<ModuleDecl*>()) {
    assert(FullName.size() > 1 &&
           "A single module is not a full path to SILDeclRef");
    auto Mod = Res.get<ModuleDecl*>();
    values.clear();
    VD = lookupMember(P, ModuleType::get(Mod), FullName[1], Locs[1], values,
                      FullName.size() == 2/*ExpectMultipleResults*/);
    for (unsigned I = 2, E = FullName.size(); I < E; I++) {
      values.clear();
      VD = lookupMember(P, VD->getInterfaceType(), FullName[I], Locs[I], values,
                        I == FullName.size() - 1/*ExpectMultipleResults*/);
    }
  } else {
    VD = Res.get<ValueDecl*>();
    for (unsigned I = 1, E = FullName.size(); I < E; I++) {
      values.clear();
      VD = lookupMember(P, VD->getInterfaceType(), FullName[I], Locs[I], values,
                        I == FullName.size() - 1/*ExpectMultipleResults*/);
    }
  }
  Decl = VD;
  return false;
}

static Optional<AccessorKind> getAccessorKind(StringRef ident) {
  return llvm::StringSwitch<Optional<AccessorKind>>(ident)
           .Case("getter", AccessorKind::Get)
           .Case("setter", AccessorKind::Set)
           .Case("addressor", AccessorKind::Address)
           .Case("mutableAddressor", AccessorKind::MutableAddress)
           .Case("read", AccessorKind::Read)
           .Case("modify", AccessorKind::Modify)
           .Default(None);
}

///  sil-decl-ref ::= '#' sil-identifier ('.' sil-identifier)* sil-decl-subref?
///  sil-decl-subref ::= '!' sil-decl-subref-part ('.' sil-decl-uncurry-level)?
///                      ('.' sil-decl-lang)?
///  sil-decl-subref ::= '!' sil-decl-uncurry-level ('.' sil-decl-lang)?
///  sil-decl-subref ::= '!' sil-decl-lang
///  sil-decl-subref-part ::= 'getter'
///  sil-decl-subref-part ::= 'setter'
///  sil-decl-subref-part ::= 'allocator'
///  sil-decl-subref-part ::= 'initializer'
///  sil-decl-subref-part ::= 'enumelt'
///  sil-decl-subref-part ::= 'destroyer'
///  sil-decl-subref-part ::= 'globalaccessor'
///  sil-decl-uncurry-level ::= [0-9]+
///  sil-decl-lang ::= 'foreign'
bool SILParser::parseSILDeclRef(SILDeclRef &Result,
                                SmallVectorImpl<ValueDecl *> &values) {
  ValueDecl *VD;
  if (parseSILDottedPath(VD, values))
    return true;

  // Initialize Kind, uncurryLevel and IsObjC.
  SILDeclRef::Kind Kind = SILDeclRef::Kind::Func;
  unsigned uncurryLevel = 0;
  bool IsObjC = false;

  if (!P.consumeIf(tok::sil_exclamation)) {
    // Construct SILDeclRef.
    Result = SILDeclRef(VD, Kind, /*isCurried=*/false, IsObjC);
    if (uncurryLevel < Result.getParameterListCount() - 1)
      Result = Result.asCurried();
    return false;
  }

  // Handle sil-constant-kind-and-uncurry-level.
  // ParseState indicates the value we just handled.
  // 1 means we just handled Kind, 2 means we just handled uncurryLevel.
  // We accept func|getter|setter|...|foreign or an integer when ParseState is
  // 0; accept foreign or an integer when ParseState is 1; accept foreign when
  // ParseState is 2.
  unsigned ParseState = 0;
  Identifier Id;
  do {
    if (P.Tok.is(tok::identifier)) {
      auto IdLoc = P.Tok.getLoc();
      if (parseSILIdentifier(Id, diag::expected_sil_constant))
        return true;
      Optional<AccessorKind> accessorKind;
      if (!ParseState && Id.str() == "func") {
        Kind = SILDeclRef::Kind::Func;
        ParseState = 1;
      } else if (!ParseState &&
                 (accessorKind = getAccessorKind(Id.str())).hasValue()) {
        // Drill down to the corresponding accessor for each declaration,
        // compacting away decls that lack it.
        size_t destI = 0;
        for (size_t srcI = 0, e = values.size(); srcI != e; ++srcI) {
          if (auto storage = dyn_cast<AbstractStorageDecl>(values[srcI]))
            if (auto accessor = storage->getOpaqueAccessor(*accessorKind))
              values[destI++] = accessor;
        }
        values.resize(destI);

        // Complain if none of the decls had a corresponding accessor.
        if (destI == 0) {
          P.diagnose(IdLoc, diag::referenced_value_no_accessor, 0);
          return true;
        }

        Kind = SILDeclRef::Kind::Func;
        VD = values[0];
        ParseState = 1;
      } else if (!ParseState && Id.str() == "allocator") {
        Kind = SILDeclRef::Kind::Allocator;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "initializer") {
        Kind = SILDeclRef::Kind::Initializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "enumelt") {
        Kind = SILDeclRef::Kind::EnumElement;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "destroyer") {
        Kind = SILDeclRef::Kind::Destroyer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "deallocator") {
        Kind = SILDeclRef::Kind::Deallocator;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "globalaccessor") {
        Kind = SILDeclRef::Kind::GlobalAccessor;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "ivardestroyer") {
        Kind = SILDeclRef::Kind::IVarDestroyer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "ivarinitializer") {
        Kind = SILDeclRef::Kind::IVarInitializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "defaultarg") {
        Kind = SILDeclRef::Kind::IVarInitializer;
        ParseState = 1;
      } else if (!ParseState && Id.str() == "propertyinit") {
        Kind = SILDeclRef::Kind::StoredPropertyInitializer;
        ParseState = 1;
      } else if (Id.str() == "foreign") {
        IsObjC = true;
        break;
      } else
        break;
    } else if (ParseState < 2 && P.Tok.is(tok::integer_literal)) {
      parseIntegerLiteral(P.Tok.getText(), 0, uncurryLevel);
      P.consumeToken(tok::integer_literal);
      ParseState = 2;
    } else
      break;

  } while (P.consumeIf(tok::period));

  // Construct SILDeclRef.
  Result = SILDeclRef(VD, Kind, /*isCurried=*/false, IsObjC);
  if (uncurryLevel < Result.getParameterListCount() - 1)
    Result = Result.asCurried();
  return false;
}

/// parseValueName - Parse a value name without a type available yet.
///
///     sil-value-name:
///       sil-local-name
///       'undef'
///
bool SILParser::parseValueName(UnresolvedValueName &Result) {
  Result.Name = P.Tok.getText();

  if (P.Tok.is(tok::kw_undef)) {
    Result.NameLoc = P.consumeToken(tok::kw_undef);
    return false;
  }

  // Parse the local-name.
  if (P.parseToken(tok::sil_local_name, Result.NameLoc,
                   diag::expected_sil_value_name))
    return true;

  return false;
}

/// parseValueRef - Parse a value, given a contextual type.
///
///     sil-value-ref:
///       sil-local-name
///
bool SILParser::parseValueRef(SILValue &Result, SILType Ty,
                              SILLocation Loc, SILBuilder &B) {
  UnresolvedValueName Name;
  if (parseValueName(Name)) return true;
  Result = getLocalValue(Name, Ty, Loc, B);
  return false;
}

/// parseTypedValueRef - Parse a type/value reference pair.
///
///    sil-typed-valueref:
///       sil-value-ref ':' sil-type
///
bool SILParser::parseTypedValueRef(SILValue &Result, SourceLoc &Loc,
                                   SILBuilder &B) {
  Loc = P.Tok.getLoc();

  UnresolvedValueName Name;
  SILType Ty;
  if (parseValueName(Name) ||
      P.parseToken(tok::colon, diag::expected_sil_colon_value_ref) ||
      parseSILType(Ty))
    return true;
  
  Result = getLocalValue(Name, Ty, RegularLocation(Loc), B);
  return false;
}

/// Look up whether the given string corresponds to a SIL opcode.
static Optional<SILInstructionKind> getOpcodeByName(StringRef OpcodeName) {
  return llvm::StringSwitch<Optional<SILInstructionKind>>(OpcodeName)
  #define FULL_INST(Id, TextualName, Parent, MemBehavior, MayRelease) \
    .Case(#TextualName, SILInstructionKind::Id)
  #include "swift/SIL/SILNodes.def"
    .Default(None);
}

/// getInstructionKind - This method maps the string form of a SIL instruction
/// opcode to an enum.
bool SILParser::parseSILOpcode(SILInstructionKind &Opcode, SourceLoc &OpcodeLoc,
                               StringRef &OpcodeName) {
  OpcodeLoc = P.Tok.getLoc();
  OpcodeName = P.Tok.getText();
  // Parse this textually to avoid Swift keywords (like 'return') from
  // interfering with opcode recognition.
  auto MaybeOpcode = getOpcodeByName(OpcodeName);
  if (!MaybeOpcode) {
    P.diagnose(OpcodeLoc, diag::expected_sil_instr_opcode);
    return true;
  }

  Opcode = MaybeOpcode.getValue();
  P.consumeToken();
  return false;
}

static bool peekSILDebugLocation(Parser &P) {
  auto T = P.peekToken().getText();
  return P.Tok.is(tok::comma) && (T == "loc" || T == "scope");
}

bool SILParser::parseSILDebugVar(SILDebugVariable &Var) {
  while (P.Tok.is(tok::comma) && !peekSILDebugLocation(P)) {
    P.consumeToken();
    StringRef Key = P.Tok.getText();
    if (Key == "name") {
      P.consumeToken();
      if (P.Tok.getKind() != tok::string_literal) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
        return true;
      }
      // Drop the double quotes.
      StringRef Val = P.Tok.getText().drop_front().drop_back();
      Var.Name = Val;
    } else if (Key == "argno") {
      P.consumeToken();
      if (P.Tok.getKind() != tok::integer_literal) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
        return true;
      }
      uint16_t ArgNo;
      if (parseIntegerLiteral(P.Tok.getText(), 0, ArgNo))
        return true;
      Var.ArgNo = ArgNo;
    } else if (Key == "let") {
      Var.Constant = true;
    } else if (Key == "var") {
      Var.Constant = false; 
    } else if (Key == "loc") {
      Var.Constant = false; 
    } else {
      P.diagnose(P.Tok, diag::sil_dbg_unknown_key, Key);
      return true;
    }
    P.consumeToken();
  }
  return false;
}

bool SILParser::parseSILBBArgsAtBranch(SmallVector<SILValue, 6> &Args,
                                       SILBuilder &B) {
  if (P.Tok.is(tok::l_paren)) {
    SourceLoc LParenLoc = P.consumeToken(tok::l_paren);
    SourceLoc RParenLoc;

    if (P.parseList(tok::r_paren, LParenLoc, RParenLoc,
                    /*AllowSepAfterLast=*/false,
                    diag::sil_basicblock_arg_rparen,
                    SyntaxKind::Unknown,
                    [&]() -> ParserStatus {
                      SILValue Arg;
                      SourceLoc ArgLoc;
                      if (parseTypedValueRef(Arg, ArgLoc, B))
                        return makeParserError();
                      Args.push_back(Arg);
                      return makeParserSuccess();
                    }).isError())
      return true;
  }
  return false;
}

/// Bind any unqualified 'Self' references to the given protocol's 'Self'
/// generic parameter.
///
/// FIXME: This is a hack to work around the lack of a DeclContext for
/// witness tables.
static void bindProtocolSelfInTypeRepr(TypeLoc &TL, ProtocolDecl *proto) {
  if (auto typeRepr = TL.getTypeRepr()) {
    // AST walker to update 'Self' references.
    class BindProtocolSelf : public ASTWalker {
      ProtocolDecl *proto;
      GenericTypeParamDecl *selfParam;
      Identifier selfId;

    public:
      BindProtocolSelf(ProtocolDecl *proto)
        : proto(proto),
          selfParam(proto->getProtocolSelfType()->getDecl()),
          selfId(proto->getASTContext().Id_Self) {
      }

      virtual bool walkToTypeReprPre(TypeRepr *T) override {
        if (auto ident = dyn_cast<IdentTypeRepr>(T)) {
          auto firstComponent = ident->getComponentRange().front();
          if (firstComponent->getIdentifier() == selfId)
            firstComponent->setValue(selfParam, proto);
        }

        return true;
      }
    };

    typeRepr->walk(BindProtocolSelf(proto));
  }
}

/// Parse the substitution list for an apply instruction or
/// specialized protocol conformance.
bool SILParser::parseSubstitutions(SmallVectorImpl<ParsedSubstitution> &parsed,
                                   GenericEnvironment *GenericEnv,
                                   ProtocolDecl *defaultForProto) {
  // Check for an opening '<' bracket.
  if (!P.Tok.isContextualPunctuator("<"))
    return false;
  
  P.consumeToken();
  
  // Parse a list of Substitutions.
  do {
    SourceLoc Loc = P.Tok.getLoc();

    // Parse substitution as AST type.
    ParserResult<TypeRepr> TyR = P.parseType();
    if (TyR.isNull())
      return true;
    TypeLoc Ty = TyR.get();
    if (defaultForProto)
      bindProtocolSelfInTypeRepr(Ty, defaultForProto);
    if (performTypeLocChecking(Ty, /*IsSILType=*/ false, GenericEnv,
                               defaultForProto))
      return true;
    parsed.push_back({Loc, Ty.getType()});
  } while (P.consumeIf(tok::comma));
  
  // Consume the closing '>'.
  if (!P.Tok.isContextualPunctuator(">")) {
    P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ">");
    return true;
  }
  P.consumeToken();
  
  return false;
}

/// Collect conformances by looking up the conformance from replacement
/// type and protocol decl.
static bool getConformancesForSubstitution(Parser &P,
              ExistentialLayout::ProtocolTypeArrayRef protocols,
              Type subReplacement,
              SourceLoc loc,
              SmallVectorImpl<ProtocolConformanceRef> &conformances) {
  auto M = P.SF.getParentModule();

  for (auto protoTy : protocols) {
    auto conformance = M->lookupConformance(subReplacement,
                                            protoTy->getDecl());
    if (conformance) {
      conformances.push_back(*conformance);
      continue;
    }

    P.diagnose(loc, diag::sil_substitution_mismatch, subReplacement,
               protoTy);
    return true;
  }

  return false;
}

/// Reconstruct an AST substitution map from parsed substitutions.
SubstitutionMap getApplySubstitutionsFromParsed(
                             SILParser &SP,
                             GenericEnvironment *env,
                             ArrayRef<ParsedSubstitution> parses) {
  if (parses.empty()) {
    assert(!env);
    return SubstitutionMap();
  }

  assert(env);

  auto loc = parses[0].loc;

  // Ensure that we have the right number of type arguments.
  auto genericSig = env->getGenericSignature();
  if (parses.size() != genericSig->getGenericParams().size()) {
    bool hasTooFew = parses.size() < genericSig->getGenericParams().size();
    SP.P.diagnose(loc,
                  hasTooFew ? diag::sil_missing_substitutions
                            : diag::sil_too_many_substitutions);
    return SubstitutionMap();
  }

  bool failed = false;
  auto subMap = SubstitutionMap::get(
    genericSig,
    [&](SubstitutableType *type) -> Type {
      auto genericParam = dyn_cast<GenericTypeParamType>(type);
      if (!genericParam) return nullptr;

      auto index = genericSig->getGenericParamOrdinal(genericParam);
      assert(index < genericSig->getGenericParams().size());
      assert(index < parses.size());

      // Provide the replacement type.
      return parses[index].replacement;
    },
    [&](CanType dependentType, Type replacementType,
        ProtocolDecl *proto) ->Optional<ProtocolConformanceRef> {
      auto M = SP.P.SF.getParentModule();
      auto conformance = M->lookupConformance(replacementType, proto);
      if (conformance) return conformance;

      SP.P.diagnose(loc, diag::sil_substitution_mismatch, replacementType,
                    proto->getDeclaredType());
      failed = true;

      return ProtocolConformanceRef(proto);
    });

  return failed ? SubstitutionMap() : subMap;
}

static ArrayRef<ProtocolConformanceRef>
collectExistentialConformances(Parser &P, CanType conformingType, SourceLoc loc,
                               CanType protocolType) {
  auto layout = protocolType.getExistentialLayout();

  if (layout.requiresClass()) {
    if (!conformingType->mayHaveSuperclass() &&
        !conformingType->isObjCExistentialType()) {
      P.diagnose(loc, diag::sil_not_class, conformingType);
    }
  }

  // FIXME: Check superclass also.

  auto protocols = layout.getProtocols();
  if (protocols.empty())
    return {};

  SmallVector<ProtocolConformanceRef, 2> conformances;
  getConformancesForSubstitution(P, protocols, conformingType,
                                 loc, conformances);

  return P.Context.AllocateCopy(conformances);
}

/// sil-loc ::= 'loc' string-literal ':' [0-9]+ ':' [0-9]+
bool SILParser::parseSILLocation(SILLocation &Loc) {
  SILLocation::DebugLoc L;
  if (parseVerbatim("loc"))
    return true;

  if (P.Tok.getKind() != tok::string_literal) {
    P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
    return true;
  }
  // Drop the double quotes.
  StringRef File = P.Tok.getText().drop_front().drop_back();
  L.Filename = P.Context.getIdentifier(File).str().data();
  P.consumeToken(tok::string_literal);
  if (P.parseToken(tok::colon, diag::expected_colon_in_sil_location))
    return true;
  if (parseInteger(L.Line, diag::sil_invalid_line_in_sil_location))
    return true;
  if (P.parseToken(tok::colon, diag::expected_colon_in_sil_location))
    return true;
  if (parseInteger(L.Column, diag::sil_invalid_column_in_sil_location))
    return true;

  Loc.setDebugInfoLoc(L);
  return false;
}

bool SILParser::parseScopeRef(SILDebugScope *&DS) {
  unsigned Slot;
  SourceLoc SlotLoc = P.Tok.getLoc();
  if (parseInteger(Slot, diag::sil_invalid_scope_slot))
    return true;

  DS = TUState.ScopeSlots[Slot];
  if (!DS) {
    P.diagnose(SlotLoc, diag::sil_scope_undeclared, Slot);
    return true;
  }
  return false;
}

///  (',' sil-loc)? (',' sil-scope-ref)?
bool SILParser::parseSILDebugLocation(SILLocation &L, SILBuilder &B,
                                      bool parsedComma) {
  // Parse the debug information, if any.
  if (P.Tok.is(tok::comma)) {
    P.consumeToken();
    parsedComma = true;
  }
  if (!parsedComma)
    return false;

  bool requireScope = false;
  if (P.Tok.getText() == "loc") {
    if (parseSILLocation(L))
      return true;

    if (P.Tok.is(tok::comma)) {
      P.consumeToken();
      requireScope = true;
    }
  }
  if (P.Tok.getText() == "scope" || requireScope) {
    parseVerbatim("scope");
    SILDebugScope *DS = nullptr;
    if (parseScopeRef(DS))
      return true;
    if (DS)
      B.setCurrentDebugScope(DS);
  }
  return false;
}

static bool parseLoadOwnershipQualifier(LoadOwnershipQualifier &Result,
                                        SILParser &P) {
  StringRef Str;
  // If we do not parse '[' ... ']', we have unqualified. Set value and return.
  if (!parseSILOptional(Str, P)) {
    Result = LoadOwnershipQualifier::Unqualified;
    return false;
  }

  // Then try to parse one of our other qualifiers. We do not support parsing
  // unqualified here so we use that as our fail value.
  auto Tmp = llvm::StringSwitch<LoadOwnershipQualifier>(Str)
                 .Case("take", LoadOwnershipQualifier::Take)
                 .Case("copy", LoadOwnershipQualifier::Copy)
                 .Case("trivial", LoadOwnershipQualifier::Trivial)
                 .Default(LoadOwnershipQualifier::Unqualified);

  // Thus return true (following the conventions in this file) if we fail.
  if (Tmp == LoadOwnershipQualifier::Unqualified)
    return true;

  // Otherwise, assign Result and return false.
  Result = Tmp;
  return false;
}

static bool parseStoreOwnershipQualifier(StoreOwnershipQualifier &Result,
                                         SILParser &P) {
  StringRef Str;
  // If we do not parse '[' ... ']', we have unqualified. Set value and return.
  if (!parseSILOptional(Str, P)) {
    Result = StoreOwnershipQualifier::Unqualified;
    return false;
  }

  // Then try to parse one of our other qualifiers. We do not support parsing
  // unqualified here so we use that as our fail value.
  auto Tmp = llvm::StringSwitch<StoreOwnershipQualifier>(Str)
                 .Case("init", StoreOwnershipQualifier::Init)
                 .Case("assign", StoreOwnershipQualifier::Assign)
                 .Case("trivial", StoreOwnershipQualifier::Trivial)
                 .Default(StoreOwnershipQualifier::Unqualified);

  // Thus return true (following the conventions in this file) if we fail.
  if (Tmp == StoreOwnershipQualifier::Unqualified)
    return true;

  // Otherwise, assign Result and return false.
  Result = Tmp;
  return false;
}

static bool parseAssignOwnershipQualifier(AssignOwnershipQualifier &Result,
                                          SILParser &P) {
  StringRef Str;
  // If we do not parse '[' ... ']', we have unknown. Set value and return.
  if (!parseSILOptional(Str, P)) {
    Result = AssignOwnershipQualifier::Unknown;
    return false;
  }

  // Then try to parse one of our other initialization kinds. We do not support
  // parsing unknown here so we use that as our fail value.
  auto Tmp = llvm::StringSwitch<AssignOwnershipQualifier>(Str)
                 .Case("reassign", AssignOwnershipQualifier::Reassign)
                 .Case("reinit", AssignOwnershipQualifier::Reinit)
                 .Case("init", AssignOwnershipQualifier::Init)
                 .Default(AssignOwnershipQualifier::Unknown);

  // Thus return true (following the conventions in this file) if we fail.
  if (Tmp == AssignOwnershipQualifier::Unknown)
    return true;

  // Otherwise, assign Result and return false.
  Result = Tmp;
  return false;
}

bool SILParser::parseSILDeclRef(SILDeclRef &Member, bool FnTypeRequired) {
  SourceLoc TyLoc;
  SmallVector<ValueDecl *, 4> values;
  if (parseSILDeclRef(Member, values))
    return true;

  // : ( or : < means that what follows is function type.
  if (!P.Tok.is(tok::colon))
    return false;

  if (FnTypeRequired &&
      !P.peekToken().is(tok::l_paren) &&
      !P.peekToken().isContextualPunctuator("<"))
    return false;

  // Type of the SILDeclRef is optional to be compatible with the old format.
  if (!P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")) {
    // Parse the type for SILDeclRef.
    Optional<Scope> GenericsScope;
    GenericsScope.emplace(&P, ScopeKind::Generics);
    ParserResult<TypeRepr> TyR = P.parseType();
    GenericsScope.reset();
    if (TyR.isNull())
      return true;
    TypeLoc Ty = TyR.get();

    // The type can be polymorphic.
    GenericEnvironment *genericEnv = nullptr;
    if (auto fnType = dyn_cast<FunctionTypeRepr>(TyR.get())) {
      if (auto generics = fnType->getGenericParams()) {
        assert(!Ty.wasValidated() && Ty.getType().isNull());

        genericEnv = handleSILGenericParams(P.Context, generics, &P.SF);
        fnType->setGenericEnvironment(genericEnv);
      }
    }

    if (performTypeLocChecking(Ty, /*IsSILType=*/ false, genericEnv))
      return true;

    // Pick the ValueDecl that has the right type.
    ValueDecl *TheDecl = nullptr;
    auto declTy = Ty.getType()->getCanonicalType();
    for (unsigned I = 0, E = values.size(); I < E; I++) {
      auto *decl = values[I];

      auto lookupTy =
        decl->getInterfaceType()
            ->removeArgumentLabels(decl->getNumCurryLevels());
      if (declTy == lookupTy->getCanonicalType()) {
        TheDecl = decl;
        // Update SILDeclRef to point to the right Decl.
        Member.loc = decl;
        break;
      }
      if (values.size() == 1 && !TheDecl) {
        P.diagnose(TyLoc, diag::sil_member_decl_type_mismatch, declTy,
                   lookupTy);
        return true;
      }
    }
    if (!TheDecl) {
      P.diagnose(TyLoc, diag::sil_member_decl_not_found);
      return true;
    }
  }
  return false;
}

bool
SILParser::parseKeyPathPatternComponent(KeyPathPatternComponent &component,
                                        SmallVectorImpl<SILType> &operandTypes,
                                        SourceLoc componentLoc,
                                        Identifier componentKind,
                                        SILLocation InstLoc,
                                        GenericEnvironment *patternEnv) {
   auto parseComponentIndices =
     [&](SmallVectorImpl<KeyPathPatternComponent::Index> &indexes) -> bool {
       while (true) {
         unsigned index;
         CanType formalTy;
         SILType loweredTy;
         if (P.parseToken(tok::oper_prefix,
                          diag::expected_tok_in_sil_instr, "%")
             || P.parseToken(tok::sil_dollar,
                             diag::expected_tok_in_sil_instr, "$"))
           return true;
         
         if (!P.Tok.is(tok::integer_literal)
             || parseIntegerLiteral(P.Tok.getText(), 0, index))
           return true;
         
         P.consumeToken(tok::integer_literal);
         
         SourceLoc formalTyLoc;
         SourceLoc loweredTyLoc;
         GenericEnvironment *ignoredParsedEnv;
         if (P.parseToken(tok::colon,
                          diag::expected_tok_in_sil_instr, ":")
             || P.parseToken(tok::sil_dollar,
                             diag::expected_tok_in_sil_instr, "$")
             || parseASTType(formalTy, formalTyLoc, patternEnv)
             || P.parseToken(tok::colon,
                             diag::expected_tok_in_sil_instr, ":")
             || parseSILType(loweredTy, loweredTyLoc,
                             ignoredParsedEnv, patternEnv))
           return true;
         
         if (patternEnv)
           loweredTy = SILType::getPrimitiveType(
             loweredTy.getASTType()->mapTypeOutOfContext()
               ->getCanonicalType(),
             loweredTy.getCategory());

         // Formal type must be hashable.
         auto proto = P.Context.getProtocol(KnownProtocolKind::Hashable);
         Type contextFormalTy = formalTy;
         if (patternEnv)
           contextFormalTy = patternEnv->mapTypeIntoContext(formalTy);
         auto lookup = P.SF.getParentModule()->lookupConformance(
                                                 contextFormalTy, proto);
         if (!lookup) {
           P.diagnose(formalTyLoc,
                      diag::sil_keypath_index_not_hashable,
                      formalTy);
           return true;
         }
         auto conformance = ProtocolConformanceRef(*lookup);
         
         indexes.push_back({index, formalTy, loweredTy, conformance});
         
         if (operandTypes.size() <= index)
           operandTypes.resize(index+1);
         if (operandTypes[index] && operandTypes[index] != loweredTy) {
           P.diagnose(loweredTyLoc,
                      diag::sil_keypath_index_operand_type_conflict,
                      index,
                      operandTypes[index].getASTType(),
                      loweredTy.getASTType());
           return true;
         }
         operandTypes[index] = loweredTy;
         
         if (P.consumeIf(tok::comma))
           continue;
         if (P.consumeIf(tok::r_square))
           break;
         return true;
       }
       return false;
     };
  
  if (componentKind.str() == "stored_property") {
    ValueDecl *prop;
    CanType ty;
    if (parseSILDottedPath(prop)
        || P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || P.parseToken(tok::sil_dollar,
                        diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ty, patternEnv))
      return true;
    component =
      KeyPathPatternComponent::forStoredProperty(cast<VarDecl>(prop), ty);
    return false;
  } else if (componentKind.str() == "gettable_property"
             || componentKind.str() == "settable_property") {
    bool isSettable = componentKind.str()[0] == 's';
    
    CanType componentTy;
    if (P.parseToken(tok::sil_dollar,diag::expected_tok_in_sil_instr,"$")
        || parseASTType(componentTy, patternEnv)
        || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;
    
    SILFunction *idFn = nullptr;
    SILDeclRef idDecl;
    VarDecl *idProperty = nullptr;
    SILFunction *getter = nullptr;
    SILFunction *setter = nullptr;
    SILFunction *equals = nullptr;
    SILFunction *hash = nullptr;
    AbstractStorageDecl *externalDecl = nullptr;
    SubstitutionMap externalSubs;
    SmallVector<KeyPathPatternComponent::Index, 4> indexes;
    while (true) {
      Identifier subKind;
      SourceLoc subKindLoc;
      if (parseSILIdentifier(subKind, subKindLoc,
                             diag::sil_keypath_expected_component_kind))
        return true;

      if (subKind.str() == "id") {
        // The identifier can be either a function ref, a SILDeclRef
        // to a class or protocol method, or a decl ref to a property:
        // @static_fn_ref : $...
        // #Type.method!whatever : (T) -> ...
        // ##Type.property
        if (P.Tok.is(tok::at_sign)) {
          if (parseSILFunctionRef(InstLoc, idFn))
            return true;
        } else if (P.Tok.is(tok::pound)) {
          if (P.peekToken().is(tok::pound)) {
            ValueDecl *propertyValueDecl;
            P.consumeToken(tok::pound);
            if (parseSILDottedPath(propertyValueDecl))
              return true;
            idProperty = cast<VarDecl>(propertyValueDecl);
          } else if (parseSILDeclRef(idDecl, /*fnType*/ true))
            return true;
        } else {
          P.diagnose(subKindLoc, diag::expected_tok_in_sil_instr, "# or @");
          return true;
        }
      } else if (subKind.str() == "getter" || subKind.str() == "setter") {
        bool isSetter = subKind.str()[0] == 's';
        if (parseSILFunctionRef(InstLoc, isSetter ? setter : getter))
          return true;
      } else if (subKind.str() == "indices") {
        if (P.parseToken(tok::l_square,
                         diag::expected_tok_in_sil_instr, "[")
            || parseComponentIndices(indexes))
          return true;
      } else if (subKind.str() == "indices_equals") {
        if (parseSILFunctionRef(InstLoc, equals))
          return true;
      } else if (subKind.str() == "indices_hash") {
        if (parseSILFunctionRef(InstLoc, hash))
          return true;
      } else if (subKind.str() == "external") {
        ValueDecl *parsedExternalDecl;
        SmallVector<ParsedSubstitution, 4> parsedSubs;

        if (parseSILDottedPath(parsedExternalDecl)
            || parseSubstitutions(parsedSubs, patternEnv))
          return true;

        externalDecl = cast<AbstractStorageDecl>(parsedExternalDecl);

        if (!parsedSubs.empty()) {
          auto genericEnv = externalDecl->getInnermostDeclContext()
                                        ->getGenericEnvironmentOfContext();
          if (!genericEnv) {
            P.diagnose(P.Tok,
                       diag::sil_substitutions_on_non_polymorphic_type);
            return true;
          }
          externalSubs = getApplySubstitutionsFromParsed(*this, genericEnv,
                                                         parsedSubs);
          if (!externalSubs) return true;

          // Map the substitutions out of the pattern context so that they
          // use interface types.
          externalSubs =
            externalSubs.mapReplacementTypesOutOfContext().getCanonical();
        }

      } else {
        P.diagnose(subKindLoc, diag::sil_keypath_unknown_component_kind,
                   subKind);
        return true;
      }
      
      if (!P.consumeIf(tok::comma))
        break;
    }
    
    if ((idFn == nullptr && idDecl.isNull() && idProperty == nullptr)
        || getter == nullptr
        || (isSettable && setter == nullptr)) {
      P.diagnose(componentLoc,
                 diag::sil_keypath_computed_property_missing_part,
                 isSettable);
      return true;
    }
    
    if ((idFn != nullptr) + (!idDecl.isNull()) + (idProperty != nullptr)
          != 1) {
      P.diagnose(componentLoc,
                 diag::sil_keypath_computed_property_missing_part,
                 isSettable);
      return true;
    }
    
    KeyPathPatternComponent::ComputedPropertyId id;
    if (idFn)
      id = idFn;
    else if (!idDecl.isNull())
      id = idDecl;
    else if (idProperty)
      id = idProperty;
    else
      llvm_unreachable("no id?!");
    
    auto indexesCopy = P.Context.AllocateCopy(indexes);
    
    if (!indexes.empty() && (!equals || !hash)) {
      P.diagnose(componentLoc,
                 diag::sil_keypath_computed_property_missing_part,
                 isSettable);
    }
    
    if (isSettable) {
      component = KeyPathPatternComponent::forComputedSettableProperty(
                             id, getter, setter,
                             indexesCopy, equals, hash,
                             externalDecl, externalSubs, componentTy);
    } else {
      component = KeyPathPatternComponent::forComputedGettableProperty(
                             id, getter,
                             indexesCopy, equals, hash,
                             externalDecl, externalSubs, componentTy);
    }
    return false;
  } else if (componentKind.str() == "optional_wrap"
               || componentKind.str() == "optional_chain"
               || componentKind.str() == "optional_force") {
    CanType ty;
    if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ty, patternEnv))
      return true;
    KeyPathPatternComponent::Kind kind;
    
    if (componentKind.str() == "optional_wrap") {
      kind = KeyPathPatternComponent::Kind::OptionalWrap;
    } else if (componentKind.str() == "optional_chain") {
      kind = KeyPathPatternComponent::Kind::OptionalChain;
    } else if (componentKind.str() == "optional_force") {
      kind = KeyPathPatternComponent::Kind::OptionalForce;
    } else {
      llvm_unreachable("unpossible");
    }
    
    component = KeyPathPatternComponent::forOptional(kind, ty);
    return false;
  } else if (componentKind.str() == "tuple_element") {
    unsigned tupleIndex;
    CanType ty;

    if (P.parseToken(tok::pound, diag::expected_sil_constant)
        || parseInteger(tupleIndex, diag::expected_sil_tuple_index)
        || P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ty, patternEnv))
      return true;
      
    component = KeyPathPatternComponent::forTupleElement(tupleIndex, ty);
    return false;
  } else {
    P.diagnose(componentLoc, diag::sil_keypath_unknown_component_kind,
               componentKind);
    return true;
  }
}

/// sil-instruction-result ::= sil-value-name '='
/// sil-instruction-result ::= '(' sil-value-name? ')'
/// sil-instruction-result ::= '(' sil-value-name (',' sil-value-name)* ')'
/// sil-instruction-source-info ::= (',' sil-scope-ref)? (',' sil-loc)?
/// sil-instruction-def ::=
///   (sil-instruction-result '=')? sil-instruction sil-instruction-source-info
bool SILParser::parseSILInstruction(SILBuilder &B) {
  // We require SIL instructions to be at the start of a line to assist
  // recovery.
  if (!P.Tok.isAtStartOfLine()) {
    P.diagnose(P.Tok, diag::expected_sil_instr_start_of_line);
    return true;
  }

  SmallVector<std::pair<StringRef, SourceLoc>, 4> resultNames;
  SourceLoc resultClauseBegin;

  // If the instruction has a name '%foo =', parse it.
  if (P.Tok.is(tok::sil_local_name)) {
    resultClauseBegin = P.Tok.getLoc();
    resultNames.push_back(std::make_pair(P.Tok.getText(), P.Tok.getLoc()));
    P.consumeToken(tok::sil_local_name);

  // If the instruction has a '(%foo, %bar) = ', parse it.
  } else if (P.consumeIf(tok::l_paren)) {
    resultClauseBegin = P.PreviousLoc;

    if (!P.consumeIf(tok::r_paren)) {
      while (true) {
        if (!P.Tok.is(tok::sil_local_name)) {
          P.diagnose(P.Tok, diag::expected_sil_value_name);
          return true;
        }

        resultNames.push_back(std::make_pair(P.Tok.getText(), P.Tok.getLoc()));
        P.consumeToken(tok::sil_local_name);

        if (P.consumeIf(tok::comma))
          continue;
        if (P.consumeIf(tok::r_paren))
          break;

        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ",");
        return true;
      }
    }
  }

  if (resultClauseBegin.isValid()) {
    if (P.parseToken(tok::equal, diag::expected_equal_in_sil_instr))
      return true;
  }

  SILInstructionKind Opcode;
  SourceLoc OpcodeLoc;
  StringRef OpcodeName;
  
  // Parse the opcode name.
  if (parseSILOpcode(Opcode, OpcodeLoc, OpcodeName))
    return true;

  SmallVector<SILValue, 4> OpList;
  SILValue Val;
  SILType Ty;
  SILLocation InstLoc = RegularLocation(OpcodeLoc);

  auto parseFormalTypeAndValue = [&](CanType &formalType,
                                     SILValue &value) -> bool {
    return (parseASTType(formalType) || parseVerbatim("in")
            || parseTypedValueRef(value, B));
  };

  OpenedExistentialAccess AccessKind;
  auto parseOpenExistAddrKind = [&]() -> bool {
    Identifier accessKindToken;
    SourceLoc accessKindLoc;
    if (parseSILIdentifier(accessKindToken, accessKindLoc,
                           diag::expected_tok_in_sil_instr,
                           "opened existential access kind")) {
      return true;
    }
    auto kind =
        llvm::StringSwitch<Optional<OpenedExistentialAccess>>(
            accessKindToken.str())
            .Case("mutable_access", OpenedExistentialAccess::Mutable)
            .Case("immutable_access", OpenedExistentialAccess::Immutable)
            .Default(None);

    if (kind) {
      AccessKind = kind.getValue();
      return false;
    }
    P.diagnose(accessKindLoc, diag::expected_tok_in_sil_instr,
               "opened existential access kind");
    return true;
  };

  CanType SourceType, TargetType;
  SILValue SourceAddr, DestAddr;
  auto parseSourceAndDestAddress = [&] {
    return parseFormalTypeAndValue(SourceType, SourceAddr)
           || parseVerbatim("to")
           || parseFormalTypeAndValue(TargetType, DestAddr);
  };

  Identifier SuccessBBName, FailureBBName;
  SourceLoc SuccessBBLoc, FailureBBLoc;
  auto parseConditionalBranchDestinations = [&] {
    return P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
           || parseSILIdentifier(SuccessBBName, SuccessBBLoc,
                                 diag::expected_sil_block_name)
           || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
           || parseSILIdentifier(FailureBBName, FailureBBLoc,
                                 diag::expected_sil_block_name)
           || parseSILDebugLocation(InstLoc, B);
  };

  // Validate the opcode name, and do opcode-specific parsing logic based on the
  // opcode we find.
  SILInstruction *ResultVal;
  switch (Opcode) {
  case SILInstructionKind::AllocBoxInst: {
    SILType Ty;
    if (parseSILType(Ty)) return true;
    SILDebugVariable VarInfo;
    if (parseSILDebugVar(VarInfo))
      return true;
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createAllocBox(InstLoc, Ty.castTo<SILBoxType>(), VarInfo);
    break;
  }
  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::TryApplyInst:
    if (parseCallInstruction(InstLoc, Opcode, B, ResultVal))
      return true;
    break;
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::EndApplyInst: {
    UnresolvedValueName argName;
    if (parseValueName(argName)) return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    SILType expectedTy = SILType::getSILTokenType(P.Context);
    SILValue op = getLocalValue(argName, expectedTy, InstLoc, B);

    if (Opcode == SILInstructionKind::AbortApplyInst) {
      ResultVal = B.createAbortApply(InstLoc, op);
    } else {
      ResultVal = B.createEndApply(InstLoc, op);
    }
    break;
  }
  case SILInstructionKind::IntegerLiteralInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;
    
    bool Negative = false;
    if (P.Tok.isAnyOperator() && P.Tok.getText() == "-") {
      Negative = true;
      P.consumeToken();
    }
    if (P.Tok.getKind() != tok::integer_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
      return true;
    }
    
    auto intTy = Ty.getAs<AnyBuiltinIntegerType>();
    if (!intTy) {
      P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
      return true;
    }

    StringRef text = prepareIntegerLiteralForParsing(P.Tok.getText());

    bool error;
    APInt value = intTy->getWidth().parse(text, 0, Negative, &error);
    if (error) {
      P.diagnose(P.Tok, diag::sil_integer_literal_not_well_formed, intTy);
      return true;
    }

    P.consumeToken(tok::integer_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createIntegerLiteral(InstLoc, Ty, value);
    break;
  }
  case SILInstructionKind::FloatLiteralInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;
   
    // The value is expressed as bits.
    if (P.Tok.getKind() != tok::integer_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
      return true;
    }
    
    auto floatTy = Ty.getAs<BuiltinFloatType>();
    if (!floatTy) {
      P.diagnose(P.Tok, diag::sil_float_literal_not_float_type);
      return true;
    }

    StringRef text = prepareIntegerLiteralForParsing(P.Tok.getText());
    
    APInt bits(floatTy->getBitWidth(), 0);
    bool error = text.getAsInteger(0, bits);
    assert(!error && "float_literal token did not parse as APInt?!");
    (void)error;
    
    if (bits.getBitWidth() != floatTy->getBitWidth())
      bits = bits.zextOrTrunc(floatTy->getBitWidth());
    
    APFloat value(floatTy->getAPFloatSemantics(), bits);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createFloatLiteral(InstLoc, Ty, value);
    P.consumeToken(tok::integer_literal);
    break;
  }
  case SILInstructionKind::StringLiteralInst: {
    if (P.Tok.getKind() != tok::identifier) {
      P.diagnose(P.Tok, diag::sil_string_no_encoding);
      return true;
    }

    StringLiteralInst::Encoding encoding;
    if (P.Tok.getText() == "utf8") {
      encoding = StringLiteralInst::Encoding::UTF8;
    } else if (P.Tok.getText() == "utf16") {
      encoding = StringLiteralInst::Encoding::UTF16;
    } else if (P.Tok.getText() == "objc_selector") {
      encoding = StringLiteralInst::Encoding::ObjCSelector;
    } else if (P.Tok.getText() == "bytes") {
      encoding = StringLiteralInst::Encoding::Bytes;
    } else {
      P.diagnose(P.Tok, diag::sil_string_invalid_encoding, P.Tok.getText());
      return true;
    }
    P.consumeToken(tok::identifier);

    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
      return true;
    }

    // Parse the string.
    SmallVector<Lexer::StringSegment, 1> segments;
    P.L->getStringLiteralSegments(P.Tok, segments);
    assert(segments.size() == 1);

    P.consumeToken(tok::string_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    SmallVector<char, 128> stringBuffer;

    if (encoding == StringLiteralInst::Encoding::Bytes) {
      // Decode hex bytes.
      CharSourceRange rawStringRange(segments.front().Loc,
                                     segments.front().Length);
      StringRef rawString = P.SourceMgr.extractText(rawStringRange);
      if (rawString.size() & 1) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,
                   "even number of hex bytes");
        return true;
      }
      while (!rawString.empty()) {
        unsigned byte1 = llvm::hexDigitValue(rawString[0]);
        unsigned byte2 = llvm::hexDigitValue(rawString[1]);
        if (byte1 == -1U || byte2 == -1U) {
          P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,
                     "hex bytes should contain 0-9, a-f, A-F only");
          return true;
        }
        stringBuffer.push_back((unsigned char)(byte1 << 4) | byte2);
        rawString = rawString.drop_front(2);
      }

      ResultVal = B.createStringLiteral(InstLoc, stringBuffer, encoding);
      break;
    }

    StringRef string = P.L->getEncodedStringSegment(segments.front(),
                                                    stringBuffer);
    ResultVal = B.createStringLiteral(InstLoc, string, encoding);
    break;
  }

  case SILInstructionKind::CondFailInst: {

    if (parseTypedValueRef(Val, B))
      return true;

    SmallVector<char, 128> stringBuffer;
    StringRef message;
    if (P.consumeIf(tok::comma)) {
      // Parse the string.
      if (P.Tok.getKind() != tok::string_literal) {
        P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "string");
        return true;
      }
      SmallVector<Lexer::StringSegment, 1> segments;
      P.L->getStringLiteralSegments(P.Tok, segments);
      assert(segments.size() == 1);

      P.consumeToken(tok::string_literal);
      message = P.L->getEncodedStringSegment(segments.front(), stringBuffer);
    }
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createCondFail(InstLoc, Val, message);
    break;
  }

  case SILInstructionKind::AllocValueBufferInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        parseVerbatim("in") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createAllocValueBuffer(InstLoc, Ty, Val);
    break;
  }
  case SILInstructionKind::ProjectValueBufferInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        parseVerbatim("in") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createProjectValueBuffer(InstLoc, Ty, Val);
    break;
  }
  case SILInstructionKind::DeallocValueBufferInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        parseVerbatim("in") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocValueBuffer(InstLoc, Ty, Val);
    break;
  }

  case SILInstructionKind::ProjectBoxInst: {
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;
    
    if (!P.Tok.is(tok::integer_literal)) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "integer");
      return true;
    }
    
    unsigned Index;
    bool error = parseIntegerLiteral(P.Tok.getText(), 0, Index);
    assert(!error && "project_box index did not parse as integer?!");
    (void)error;

    P.consumeToken(tok::integer_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    
    ResultVal = B.createProjectBox(InstLoc, Val, Index);
    break;
  }
      
  case SILInstructionKind::ProjectExistentialBoxInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        parseVerbatim("in") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createProjectExistentialBox(InstLoc, Ty, Val);
    break;
  }
      
  case SILInstructionKind::FunctionRefInst: {
    SILFunction *Fn;
    if (parseSILFunctionRef(InstLoc, Fn) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createFunctionRef(InstLoc, Fn);
    break;
  }
  case SILInstructionKind::DynamicFunctionRefInst: {
    SILFunction *Fn;
    if (parseSILFunctionRef(InstLoc, Fn) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    // Set a forward reference's dynamic property for the first time.
    if (!Fn->isDynamicallyReplaceable()) {
      if (!Fn->empty()) {
        P.diagnose(P.Tok, diag::expected_dynamic_func_attr);
        return true;
      }
      Fn->setIsDynamic();
    }
    ResultVal = B.createDynamicFunctionRef(InstLoc, Fn);
    break;
  }
  case SILInstructionKind::PreviousDynamicFunctionRefInst: {
    SILFunction *Fn;
    if (parseSILFunctionRef(InstLoc, Fn) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createPreviousDynamicFunctionRef(InstLoc, Fn);
    break;
  }
  case SILInstructionKind::BuiltinInst: {
    if (P.Tok.getKind() != tok::string_literal) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,"builtin name");
      return true;
    }
    StringRef Str = P.Tok.getText();
    Identifier Id = P.Context.getIdentifier(Str.substr(1, Str.size()-2));
    P.consumeToken(tok::string_literal);
    
    // Find the builtin in the Builtin module
    SmallVector<ValueDecl*, 2> foundBuiltins;
    P.Context.TheBuiltinModule->lookupMember(foundBuiltins,
                                             P.Context.TheBuiltinModule, Id,
                                             Identifier());
    if (foundBuiltins.empty()) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr,"builtin name");
      return true;
    }
    assert(foundBuiltins.size() == 1 && "ambiguous builtin name?!");

    auto *builtinFunc = cast<FuncDecl>(foundBuiltins[0]);
    GenericEnvironment *genericEnv = builtinFunc->getGenericEnvironment();
    
    SmallVector<ParsedSubstitution, 4> parsedSubs;
    SubstitutionMap subMap;
    if (parseSubstitutions(parsedSubs))
      return true;
    
    if (!parsedSubs.empty()) {
      if (!genericEnv) {
        P.diagnose(P.Tok, diag::sil_substitutions_on_non_polymorphic_type);
        return true;
      }
      subMap = getApplySubstitutionsFromParsed(*this, genericEnv, parsedSubs);
      if (!subMap)
        return true;
    }
    
    if (P.Tok.getKind() != tok::l_paren) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "(");
      return true;
    }
    P.consumeToken(tok::l_paren);

    SmallVector<SILValue, 4> Args;
    while (true) {
      if (P.consumeIf(tok::r_paren))
        break;

      SILValue Val;
      if (parseTypedValueRef(Val, B))
        return true;
      Args.push_back(Val);
      if (P.consumeIf(tok::comma))
        continue;
      if (P.consumeIf(tok::r_paren))
        break;
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ")' or ',");
      return true;
    }
    
    if (P.Tok.getKind() != tok::colon) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, ":");
      return true;
    }
    P.consumeToken(tok::colon);
    
    SILType ResultTy;
    if (parseSILType(ResultTy))
      return true;
    
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createBuiltin(InstLoc, Id, ResultTy, subMap, Args);
    break;
  }
  case SILInstructionKind::OpenExistentialAddrInst:
    if (parseOpenExistAddrKind() || parseTypedValueRef(Val, B)
        || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createOpenExistentialAddr(InstLoc, Val, Ty, AccessKind);
    break;

  case SILInstructionKind::OpenExistentialBoxInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createOpenExistentialBox(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::OpenExistentialBoxValueInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createOpenExistentialBoxValue(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::OpenExistentialMetatypeInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createOpenExistentialMetatype(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::OpenExistentialRefInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createOpenExistentialRef(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::OpenExistentialValueInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createOpenExistentialValue(InstLoc, Val, Ty);
    break;

#define UNARY_INSTRUCTION(ID) \
  case SILInstructionKind::ID##Inst:                   \
    if (parseTypedValueRef(Val, B)) return true; \
    if (parseSILDebugLocation(InstLoc, B)) return true; \
    ResultVal = B.create##ID(InstLoc, Val);   \
    break;

#define REFCOUNTING_INSTRUCTION(ID)                                            \
  case SILInstructionKind::ID##Inst: {                                                  \
    Atomicity atomicity = Atomicity::Atomic;                                   \
    StringRef Optional;                                                        \
    if (parseSILOptional(Optional, *this)) {                                   \
      if (Optional == "nonatomic") {                                           \
        atomicity = Atomicity::NonAtomic;                                      \
      } else {                                                                 \
        return true;                                                           \
      }                                                                        \
    }                                                                          \
    if (parseTypedValueRef(Val, B))                                            \
      return true;                                                             \
    if (parseSILDebugLocation(InstLoc, B))                                     \
      return true;                                                             \
    ResultVal = B.create##ID(InstLoc, Val, atomicity);                         \
  } break;

    UNARY_INSTRUCTION(ClassifyBridgeObject)
    UNARY_INSTRUCTION(ValueToBridgeObject)
    UNARY_INSTRUCTION(FixLifetime)
    UNARY_INSTRUCTION(EndLifetime)
    UNARY_INSTRUCTION(CopyBlock)
    UNARY_INSTRUCTION(IsUnique)
    UNARY_INSTRUCTION(DestroyAddr)
    UNARY_INSTRUCTION(CopyValue)
    UNARY_INSTRUCTION(DestroyValue)
    UNARY_INSTRUCTION(EndBorrow)
    UNARY_INSTRUCTION(DestructureStruct)
    UNARY_INSTRUCTION(DestructureTuple)
    REFCOUNTING_INSTRUCTION(UnmanagedReleaseValue)
    REFCOUNTING_INSTRUCTION(UnmanagedRetainValue)
    REFCOUNTING_INSTRUCTION(UnmanagedAutoreleaseValue)
    REFCOUNTING_INSTRUCTION(StrongRetain)
    REFCOUNTING_INSTRUCTION(StrongRelease)
    REFCOUNTING_INSTRUCTION(AutoreleaseValue)
    REFCOUNTING_INSTRUCTION(SetDeallocating)
    REFCOUNTING_INSTRUCTION(ReleaseValue)
    REFCOUNTING_INSTRUCTION(RetainValue)
    REFCOUNTING_INSTRUCTION(ReleaseValueAddr)
    REFCOUNTING_INSTRUCTION(RetainValueAddr)
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    REFCOUNTING_INSTRUCTION(StrongRetain##Name) \
    REFCOUNTING_INSTRUCTION(Name##Retain) \
    REFCOUNTING_INSTRUCTION(Name##Release) \
    UNARY_INSTRUCTION(Copy##Name##Value)
#include "swift/AST/ReferenceStorage.def"
#undef UNARY_INSTRUCTION
#undef REFCOUNTING_INSTRUCTION

  case SILInstructionKind::IsEscapingClosureInst: {
    bool IsObjcVerifcationType = false;
    if (parseSILOptional(IsObjcVerifcationType, *this, "objc"))
      return true;
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createIsEscapingClosure(
        InstLoc, Val,
        IsObjcVerifcationType ? IsEscapingClosureInst::ObjCEscaping
                              : IsEscapingClosureInst::WithoutActuallyEscaping);
    break;
  }

 case SILInstructionKind::DebugValueInst:
 case SILInstructionKind::DebugValueAddrInst: {
   SILDebugVariable VarInfo;
   if (parseTypedValueRef(Val, B) ||
       parseSILDebugVar(VarInfo) ||
       parseSILDebugLocation(InstLoc, B))
     return true;
   if (Opcode == SILInstructionKind::DebugValueInst)
     ResultVal = B.createDebugValue(InstLoc, Val, VarInfo);
   else
     ResultVal = B.createDebugValueAddr(InstLoc, Val, VarInfo);
   break;
 }

 // unchecked_ownership_conversion <reg> : <type>, <ownership> to <ownership>
 case SILInstructionKind::UncheckedOwnershipConversionInst: {
   ValueOwnershipKind LHSKind = ValueOwnershipKind::Any;
   ValueOwnershipKind RHSKind = ValueOwnershipKind::Any;
   SourceLoc Loc;

   if (parseTypedValueRef(Val, Loc, B) ||
       P.parseToken(tok::comma, diag::expected_sil_colon,
                    "unchecked_ownership_conversion value ownership kind "
                    "conversion specification") ||
       parseSILOwnership(LHSKind) || parseVerbatim("to") ||
       parseSILOwnership(RHSKind) || parseSILDebugLocation(InstLoc, B)) {
     return true;
   }

   if (Val.getOwnershipKind() != LHSKind) {
     return true;
   }

   ResultVal = B.createUncheckedOwnershipConversion(InstLoc, Val, RHSKind);
   break;
 }

 case SILInstructionKind::LoadInst: {
   LoadOwnershipQualifier Qualifier;
   SourceLoc AddrLoc;

   if (parseLoadOwnershipQualifier(Qualifier, *this) ||
       parseTypedValueRef(Val, AddrLoc, B) || parseSILDebugLocation(InstLoc, B))
     return true;

   ResultVal = B.createLoad(InstLoc, Val, Qualifier);
   break;
 }

 case SILInstructionKind::LoadBorrowInst: {
   SourceLoc AddrLoc;

   if (parseTypedValueRef(Val, AddrLoc, B) || parseSILDebugLocation(InstLoc, B))
     return true;

   ResultVal = B.createLoadBorrow(InstLoc, Val);
   break;
 }

 case SILInstructionKind::BeginBorrowInst: {
   SourceLoc AddrLoc;

   if (parseTypedValueRef(Val, AddrLoc, B) || parseSILDebugLocation(InstLoc, B))
     return true;

   ResultVal = B.createBeginBorrow(InstLoc, Val);
   break;
 }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Load##Name##Inst: { \
    bool isTake = false; \
    SourceLoc addrLoc; \
    if (parseSILOptional(isTake, *this, "take") || \
        parseTypedValueRef(Val, addrLoc, B) || \
        parseSILDebugLocation(InstLoc, B)) \
      return true; \
    if (!Val->getType().is<Name##StorageType>()) { \
      P.diagnose(addrLoc, diag::sil_operand_not_ref_storage_address, \
                 "source", OpcodeName, ReferenceOwnership::Name); \
    } \
    ResultVal = B.createLoad##Name(InstLoc, Val, IsTake_t(isTake)); \
    break; \
  }
#include "swift/AST/ReferenceStorage.def"

  case SILInstructionKind::CopyBlockWithoutEscapingInst: {
    SILValue Closure;
    if (parseTypedValueRef(Val, B) ||
        parseVerbatim("withoutEscaping") ||
        parseTypedValueRef(Closure, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createCopyBlockWithoutEscaping(InstLoc, Val, Closure);
    break;
  }

  case SILInstructionKind::MarkDependenceInst: {
    SILValue Base;
    if (parseTypedValueRef(Val, B) ||
        parseVerbatim("on") ||
        parseTypedValueRef(Base, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createMarkDependence(InstLoc, Val, Base);
    break;
  }

  case SILInstructionKind::KeyPathInst: {
    SmallVector<KeyPathPatternComponent, 4> components;
    SILType Ty;
    if (parseSILType(Ty)
        || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    GenericParamList *generics = nullptr;
    GenericEnvironment *patternEnv = nullptr;
    CanType rootType;
    StringRef objcString;
    SmallVector<SILType, 4> operandTypes;
    {
      Scope genericsScope(&P, ScopeKind::Generics);
      generics = P.maybeParseGenericParams().getPtrOrNull();
      patternEnv = handleSILGenericParams(P.Context, generics, &P.SF);
      
      if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
        return true;
      
      while (true) {
        Identifier componentKind;
        SourceLoc componentLoc;
        if (parseSILIdentifier(componentKind, componentLoc,
                               diag::sil_keypath_expected_component_kind))
          return true;

        
        if (componentKind.str() == "root") {
          if (P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$")
              || parseASTType(rootType, patternEnv))
            return true;
        } else if (componentKind.str() == "objc") {
          auto tok = P.Tok;
          if (P.parseToken(tok::string_literal, diag::expected_tok_in_sil_instr,
                           "string literal"))
            return true;
          
          auto objcStringValue = tok.getText().drop_front().drop_back();
          objcString = StringRef(
            P.Context.AllocateCopy<char>(objcStringValue.begin(),
                                         objcStringValue.end()),
            objcStringValue.size());
        } else {
          KeyPathPatternComponent component;
          if (parseKeyPathPatternComponent(component, operandTypes,
                                           componentLoc, componentKind,
                                           InstLoc, patternEnv))
            return true;
          components.push_back(component);
        }
        
        if (!P.consumeIf(tok::semi))
          break;
      }
      
      if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")") ||
          parseSILDebugLocation(InstLoc, B))
        return true;
    }
    
    if (rootType.isNull())
      P.diagnose(InstLoc.getSourceLoc(), diag::sil_keypath_no_root);
    
    SmallVector<ParsedSubstitution, 4> parsedSubs;
    if (parseSubstitutions(parsedSubs, ContextGenericEnv))
      return true;
    
    SubstitutionMap subMap;
    if (!parsedSubs.empty()) {
      if (!patternEnv) {
        P.diagnose(InstLoc.getSourceLoc(),
                   diag::sil_substitutions_on_non_polymorphic_type);
        return true;
      }

      subMap = getApplySubstitutionsFromParsed(*this, patternEnv, parsedSubs);
      if (!subMap)
        return true;
    }
    
    SmallVector<SILValue, 4> operands;
    
    if (P.consumeIf(tok::l_paren)) {
      Lowering::GenericContextScope scope(SILMod.Types,
        patternEnv ? patternEnv->getGenericSignature()->getCanonicalSignature()
                   : nullptr);
      while (true) {
        SILValue v;
        
        if (operands.size() >= operandTypes.size()
            || !operandTypes[operands.size()]) {
          P.diagnose(P.Tok, diag::sil_keypath_no_use_of_operand_in_pattern,
                     operands.size());
          return true;
        }
        
        auto ty = operandTypes[operands.size()].subst(SILMod, subMap);
        
        if (parseValueRef(v, ty, RegularLocation(P.Tok.getLoc()), B))
          return true;
        operands.push_back(v);
        
        if (P.consumeIf(tok::comma))
          continue;
        if (P.consumeIf(tok::r_paren))
          break;
        return true;
      }
    }
    
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    CanGenericSignature canSig = nullptr;
    if (patternEnv && patternEnv->getGenericSignature()) {
      canSig = patternEnv->getGenericSignature()->getCanonicalSignature();
    }
    CanType leafType;
    if (!components.empty())
      leafType = components.back().getComponentType();
    else
      leafType = rootType;
    auto pattern = KeyPathPattern::get(B.getModule(), canSig,
                     rootType, leafType,
                     components, objcString);

    ResultVal = B.createKeyPath(InstLoc, pattern, subMap, operands, Ty);
    break;
  }

  // Conversion instructions.
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::UncheckedTrivialBitCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::AddressToPointerInst:
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::BridgeObjectToWordInst:
  case SILInstructionKind::RefToRawPointerInst:
  case SILInstructionKind::RawPointerToRefInst:
#define LOADABLE_REF_STORAGE(Name, ...) \
  case SILInstructionKind::RefTo##Name##Inst: \
  case SILInstructionKind::Name##ToRefInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::ThinFunctionToPointerInst:
  case SILInstructionKind::PointerToThinFunctionInst:
  case SILInstructionKind::ThinToThickFunctionInst:
  case SILInstructionKind::ThickToObjCMetatypeInst:
  case SILInstructionKind::ObjCToThickMetatypeInst:
  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::ConvertEscapeToNoEscapeInst:
  case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
  case SILInstructionKind::ObjCMetatypeToObjectInst: {
    SILType Ty;
    Identifier ToToken;
    SourceLoc ToLoc;
    bool not_guaranteed = false;
    bool without_actually_escaping = false;
    if (Opcode == SILInstructionKind::ConvertEscapeToNoEscapeInst) {
      StringRef attrName;
      if (parseSILOptional(attrName, *this)) {
        if (attrName.equals("not_guaranteed"))
          not_guaranteed = true;
        else
          return true;
      }
    }
    if (parseTypedValueRef(Val, B)
        || parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                              "to"))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }
    if (Opcode == SILInstructionKind::ConvertFunctionInst) {
      StringRef attrName;
      if (parseSILOptional(attrName, *this)) {
        if (attrName.equals("without_actually_escaping"))
          without_actually_escaping = true;
        else
          return true;
      }
    }
    if (parseSILType(Ty) || parseSILDebugLocation(InstLoc, B))
      return true;

    switch (Opcode) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::UncheckedRefCastInst:
      ResultVal = B.createUncheckedRefCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UncheckedAddrCastInst:
      ResultVal = B.createUncheckedAddrCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UncheckedTrivialBitCastInst:
      ResultVal = B.createUncheckedTrivialBitCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UncheckedBitwiseCastInst:
      ResultVal = B.createUncheckedBitwiseCast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::UpcastInst:
      ResultVal = B.createUpcast(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ConvertFunctionInst:
      ResultVal =
          B.createConvertFunction(InstLoc, Val, Ty, without_actually_escaping);
      break;
    case SILInstructionKind::ConvertEscapeToNoEscapeInst:
      ResultVal =
          B.createConvertEscapeToNoEscape(InstLoc, Val, Ty, !not_guaranteed);
      break;
    case SILInstructionKind::AddressToPointerInst:
      ResultVal = B.createAddressToPointer(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::BridgeObjectToRefInst:
      ResultVal = B.createBridgeObjectToRef(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::BridgeObjectToWordInst:
      ResultVal = B.createBridgeObjectToWord(InstLoc, Val);
      break;
    case SILInstructionKind::RefToRawPointerInst:
      ResultVal = B.createRefToRawPointer(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::RawPointerToRefInst:
      ResultVal = B.createRawPointerToRef(InstLoc, Val, Ty);
      break;
#define LOADABLE_REF_STORAGE(Name, ...) \
    case SILInstructionKind::RefTo##Name##Inst: \
      ResultVal = B.createRefTo##Name(InstLoc, Val, Ty); \
      break; \
    case SILInstructionKind::Name##ToRefInst: \
      ResultVal = B.create##Name##ToRef(InstLoc, Val, Ty); \
      break;
#include "swift/AST/ReferenceStorage.def"
    case SILInstructionKind::ThinFunctionToPointerInst:
      ResultVal = B.createThinFunctionToPointer(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::PointerToThinFunctionInst:
      ResultVal = B.createPointerToThinFunction(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ThinToThickFunctionInst:
      ResultVal = B.createThinToThickFunction(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ThickToObjCMetatypeInst:
      ResultVal = B.createThickToObjCMetatype(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ObjCToThickMetatypeInst:
      ResultVal = B.createObjCToThickMetatype(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ObjCMetatypeToObjectInst:
      ResultVal = B.createObjCMetatypeToObject(InstLoc, Val, Ty);
      break;
    case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
      ResultVal = B.createObjCExistentialMetatypeToObject(InstLoc, Val, Ty);
      break;
    }
    break;
  }
  case SILInstructionKind::PointerToAddressInst: {
    SILType Ty;
    Identifier ToToken;
    SourceLoc ToLoc;
    StringRef attr;
    if (parseTypedValueRef(Val, B) ||
        parseSILIdentifier(ToToken, ToLoc,
                           diag::expected_tok_in_sil_instr, "to"))
      return true;
    if (parseSILOptional(attr, *this) && attr.empty())
      return true;
    if (parseSILType(Ty) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    bool isStrict = attr.equals("strict");
    bool isInvariant = attr.equals("invariant");

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    ResultVal = B.createPointerToAddress(InstLoc, Val, Ty,
                                         isStrict, isInvariant);
    break;
  }
  case SILInstructionKind::RefToBridgeObjectInst: {
    SILValue BitsVal;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(BitsVal, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createRefToBridgeObject(InstLoc, Val, BitsVal);
    break;
  }

  case SILInstructionKind::CheckedCastAddrBranchInst: {
    Identifier consumptionKindToken;
    SourceLoc consumptionKindLoc;
    if (parseSILIdentifier(consumptionKindToken, consumptionKindLoc,
                           diag::expected_tok_in_sil_instr,
                           "cast consumption kind")) {
      return true;
    }
    // NOTE: BorrowAlways is not a supported cast kind for address types, so we
    // purposely do not parse it here.
    auto kind = llvm::StringSwitch<Optional<CastConsumptionKind>>(
                    consumptionKindToken.str())
                    .Case("take_always", CastConsumptionKind::TakeAlways)
                    .Case("take_on_success", CastConsumptionKind::TakeOnSuccess)
                    .Case("copy_on_success", CastConsumptionKind::CopyOnSuccess)
                    .Default(None);

    if (!kind) {
      P.diagnose(consumptionKindLoc, diag::expected_tok_in_sil_instr,
                 "cast consumption kind");
      return true;
    }
    auto consumptionKind = kind.getValue();

    if (parseSourceAndDestAddress() || parseConditionalBranchDestinations()
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createCheckedCastAddrBranch(
        InstLoc, consumptionKind, SourceAddr, SourceType, DestAddr, TargetType,
        getBBForReference(SuccessBBName, SuccessBBLoc),
        getBBForReference(FailureBBName, FailureBBLoc));
    break;
  }
  case SILInstructionKind::UncheckedRefCastAddrInst:
    if (parseSourceAndDestAddress() || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createUncheckedRefCastAddr(InstLoc, SourceAddr, SourceType,
                                             DestAddr, TargetType);
    break;

  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
    if (parseSourceAndDestAddress() || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createUnconditionalCheckedCastAddr(
        InstLoc, SourceAddr, SourceType, DestAddr, TargetType);
    break;

  case SILInstructionKind::UnconditionalCheckedCastValueInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createUnconditionalCheckedCastValue(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::UnconditionalCheckedCastInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createUnconditionalCheckedCast(InstLoc, Val, Ty);
    break;

  case SILInstructionKind::CheckedCastBranchInst: {
    bool isExact = false;
    if (Opcode == SILInstructionKind::CheckedCastBranchInst &&
        parseSILOptional(isExact, *this, "exact"))
      return true;

    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseConditionalBranchDestinations())
      return true;

    ResultVal = B.createCheckedCastBranch(
        InstLoc, isExact, Val, Ty,
        getBBForReference(SuccessBBName, SuccessBBLoc),
        getBBForReference(FailureBBName, FailureBBLoc));
    break;
  }
  case SILInstructionKind::CheckedCastValueBranchInst:
    if (parseTypedValueRef(Val, B) || parseVerbatim("to") || parseSILType(Ty)
        || parseConditionalBranchDestinations())
      return true;

    ResultVal = B.createCheckedCastValueBranch(
        InstLoc, Val, Ty, getBBForReference(SuccessBBName, SuccessBBLoc),
        getBBForReference(FailureBBName, FailureBBLoc));
    break;

  case SILInstructionKind::MarkUninitializedInst: {
    if (P.parseToken(tok::l_square, diag::expected_tok_in_sil_instr, "["))
      return true;
    
    Identifier KindId;
    SourceLoc KindLoc = P.Tok.getLoc();
    if (P.consumeIf(tok::kw_var))
      KindId = P.Context.getIdentifier("var");
    else if (P.parseIdentifier(KindId, KindLoc,
                               diag::expected_tok_in_sil_instr, "kind"))
      return true;
    
    if (P.parseToken(tok::r_square, diag::expected_tok_in_sil_instr, "]"))
      return true;

    MarkUninitializedInst::Kind Kind;
    if (KindId.str() == "var")
      Kind = MarkUninitializedInst::Var;
    else if (KindId.str() == "rootself")
      Kind = MarkUninitializedInst::RootSelf;
    else if (KindId.str() == "crossmodulerootself")
      Kind = MarkUninitializedInst::CrossModuleRootSelf;
    else if (KindId.str() == "derivedself")
      Kind = MarkUninitializedInst::DerivedSelf;
    else if (KindId.str() == "derivedselfonly")
      Kind = MarkUninitializedInst::DerivedSelfOnly;
    else if (KindId.str() == "delegatingself")
      Kind = MarkUninitializedInst::DelegatingSelf;
    else if (KindId.str() == "delegatingselfallocated")
      Kind = MarkUninitializedInst::DelegatingSelfAllocated;
    else {
      P.diagnose(KindLoc, diag::expected_tok_in_sil_instr,
                 "var, rootself, crossmodulerootself, derivedself, "
                 "derivedselfonly, delegatingself, or delegatingselfallocated");
      return true;
    }

    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createMarkUninitialized(InstLoc, Val, Kind);
    break;
  }
  
  case SILInstructionKind::MarkFunctionEscapeInst: {
    SmallVector<SILValue, 4> OpList;
    do {
      if (parseTypedValueRef(Val, B)) return true;
      OpList.push_back(Val);
    } while (!peekSILDebugLocation(P) && P.consumeIf(tok::comma));

    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createMarkFunctionEscape(InstLoc, OpList);
    break;
  }

  case SILInstructionKind::AssignInst:
  case SILInstructionKind::StoreInst: {
    UnresolvedValueName From;
    SourceLoc ToLoc, AddrLoc;
    Identifier ToToken;
    SILValue AddrVal;
    StoreOwnershipQualifier StoreQualifier;
    AssignOwnershipQualifier AssignQualifier;
    bool IsStore = Opcode == SILInstructionKind::StoreInst;
    bool IsAssign = Opcode == SILInstructionKind::AssignInst;
    if (parseValueName(From) ||
        parseSILIdentifier(ToToken, ToLoc, diag::expected_tok_in_sil_instr,
                           "to"))
      return true;

    if (IsStore && parseStoreOwnershipQualifier(StoreQualifier, *this))
      return true;

    if (IsAssign && parseAssignOwnershipQualifier(AssignQualifier, *this))
      return true;

    if (parseTypedValueRef(AddrVal, AddrLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    if (!AddrVal->getType().isAddress()) {
      P.diagnose(AddrLoc, diag::sil_operand_not_address, "destination",
                 OpcodeName);
      return true;
    }

    SILType ValType = AddrVal->getType().getObjectType();

    if (IsStore) {
      ResultVal = B.createStore(InstLoc,
                                getLocalValue(From, ValType, InstLoc, B),
                                AddrVal, StoreQualifier);
    } else {
      assert(IsAssign);

      ResultVal = B.createAssign(InstLoc,
                                getLocalValue(From, ValType, InstLoc, B),
                                AddrVal, AssignQualifier);
    }

    break;
  }

  case SILInstructionKind::AssignByWrapperInst: {
    SILValue Src, DestAddr, InitFn, SetFn;
    SourceLoc DestLoc;
    AssignOwnershipQualifier AssignQualifier;
    if (parseTypedValueRef(Src,  B) ||
        parseVerbatim("to") ||
        parseAssignOwnershipQualifier(AssignQualifier, *this) ||
        parseTypedValueRef(DestAddr, DestLoc, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("init") ||
        parseTypedValueRef(InitFn, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("set") ||
        parseTypedValueRef(SetFn, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (!DestAddr->getType().isAddress()) {
      P.diagnose(DestLoc, diag::sil_operand_not_address, "destination",
                 OpcodeName);
      return true;
    }

    ResultVal = B.createAssignByWrapper(InstLoc, Src, DestAddr, InitFn, SetFn,
                                         AssignQualifier);
    break;
  }

  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::BeginUnpairedAccessInst:
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::EndUnpairedAccessInst: {
    ParsedEnum<SILAccessKind> kind;
    ParsedEnum<SILAccessEnforcement> enforcement;
    ParsedEnum<bool> aborting;
    ParsedEnum<bool> noNestedConflict;
    ParsedEnum<bool> fromBuiltin;

    bool isBeginAccess = (Opcode == SILInstructionKind::BeginAccessInst ||
                          Opcode == SILInstructionKind::BeginUnpairedAccessInst);
    bool wantsEnforcement = (isBeginAccess ||
                             Opcode == SILInstructionKind::EndUnpairedAccessInst);

    while (P.consumeIf(tok::l_square)) {
      Identifier ident;
      SourceLoc identLoc;
      if (parseSILIdentifier(ident, identLoc,
                             diag::expected_in_attribute_list)) {
        if (P.consumeIf(tok::r_square)) {
          continue;
        } else {
          return true;
        }
      }
      StringRef attr = ident.str();

      auto setEnforcement = [&](SILAccessEnforcement value) {
        maybeSetEnum(wantsEnforcement, enforcement, value, attr, identLoc);
      };
      auto setKind = [&](SILAccessKind value) {
        maybeSetEnum(isBeginAccess, kind, value, attr, identLoc);
      };
      auto setAborting = [&](bool value) {
        maybeSetEnum(!isBeginAccess, aborting, value, attr, identLoc);
      };
      auto setNoNestedConflict = [&](bool value) {
        maybeSetEnum(isBeginAccess, noNestedConflict, value, attr, identLoc);
      };
      auto setFromBuiltin = [&](bool value) {
        maybeSetEnum(Opcode != SILInstructionKind::EndAccessInst, fromBuiltin,
                     value, attr, identLoc);
      };

      if (attr == "unknown") {
        setEnforcement(SILAccessEnforcement::Unknown);
      } else if (attr == "static") {
        setEnforcement(SILAccessEnforcement::Static);
      } else if (attr == "dynamic") {
        setEnforcement(SILAccessEnforcement::Dynamic);
      } else if (attr == "unsafe") {
        setEnforcement(SILAccessEnforcement::Unsafe);
      } else if (attr == "init") {
        setKind(SILAccessKind::Init);
      } else if (attr == "read") {
        setKind(SILAccessKind::Read);
      } else if (attr == "modify") {
        setKind(SILAccessKind::Modify);
      } else if (attr == "deinit") {
        setKind(SILAccessKind::Deinit);
      } else if (attr == "abort") {
        setAborting(true);
      } else if (attr == "no_nested_conflict") {
        setNoNestedConflict(true);
      } else if (attr == "builtin") {
        setFromBuiltin(true);
      } else {
        P.diagnose(identLoc, diag::unknown_attribute, attr);
      }

      if (!P.consumeIf(tok::r_square))
        return true;
    }

    if (isBeginAccess && !kind.isSet()) {
      P.diagnose(OpcodeLoc, diag::sil_expected_access_kind, OpcodeName);
      kind.Value = SILAccessKind::Read;
    }

    if (wantsEnforcement && !enforcement.isSet()) {
      P.diagnose(OpcodeLoc, diag::sil_expected_access_enforcement, OpcodeName);
      enforcement.Value = SILAccessEnforcement::Unsafe;
    }

    if (!isBeginAccess && !aborting.isSet())
      aborting.Value = false;

    if (isBeginAccess && !noNestedConflict.isSet())
      noNestedConflict.Value = false;

    if (!fromBuiltin.isSet())
      fromBuiltin.Value = false;

    SILValue addrVal;
    SourceLoc addrLoc;
    if (parseTypedValueRef(addrVal, addrLoc, B))
      return true;

    SILValue bufferVal;
    SourceLoc bufferLoc;
    if (Opcode == SILInstructionKind::BeginUnpairedAccessInst &&
        (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
         parseTypedValueRef(bufferVal, bufferLoc, B)))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    if (!addrVal->getType().isAddress()) {
      P.diagnose(addrLoc, diag::sil_operand_not_address, "operand",
                 OpcodeName);
      return true;
    }

    if (Opcode == SILInstructionKind::BeginAccessInst) {
      ResultVal =
          B.createBeginAccess(InstLoc, addrVal, *kind, *enforcement,
                              *noNestedConflict, *fromBuiltin);
    } else if (Opcode == SILInstructionKind::EndAccessInst) {
      ResultVal = B.createEndAccess(InstLoc, addrVal, *aborting);
    } else if (Opcode == SILInstructionKind::BeginUnpairedAccessInst) {
      ResultVal = B.createBeginUnpairedAccess(InstLoc, addrVal, bufferVal,
                                              *kind, *enforcement,
                                              *noNestedConflict, *fromBuiltin);
    } else {
      ResultVal = B.createEndUnpairedAccess(InstLoc, addrVal, *enforcement,
                                            *aborting, *fromBuiltin);
    }
    break;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Store##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StoreBorrowInst: {
    UnresolvedValueName from;
    bool isRefStorage = false;
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    isRefStorage |= Opcode == SILInstructionKind::Store##Name##Inst;
#include "swift/AST/ReferenceStorage.def"

    SourceLoc toLoc, addrLoc;
    Identifier toToken;
    SILValue addrVal;
    bool isInit = false;
    if (parseValueName(from) ||
        parseSILIdentifier(toToken, toLoc,
                           diag::expected_tok_in_sil_instr, "to") ||
        (isRefStorage && parseSILOptional(isInit, *this, "initialization")) ||
        parseTypedValueRef(addrVal, addrLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (toToken.str() != "to") {
      P.diagnose(toLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    if (!addrVal->getType().isAddress()) {
      P.diagnose(addrLoc, diag::sil_operand_not_address,
                 "destination", OpcodeName);
      return true;
    }

    if (Opcode == SILInstructionKind::StoreBorrowInst) {
      SILType valueTy = addrVal->getType().getObjectType();
      ResultVal = B.createStoreBorrow(
          InstLoc, getLocalValue(from, valueTy, InstLoc, B), addrVal);
      break;
    }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    if (Opcode == SILInstructionKind::Store##Name##Inst) { \
      auto refType = addrVal->getType().getAs<Name##StorageType>(); \
      if (!refType) { \
        P.diagnose(addrLoc, diag::sil_operand_not_ref_storage_address, \
                   "destination", OpcodeName, ReferenceOwnership::Name); \
        return true; \
      } \
      auto valueTy =SILType::getPrimitiveObjectType(refType.getReferentType());\
      ResultVal = B.createStore##Name(InstLoc, \
                                      getLocalValue(from, valueTy, InstLoc, B),\
                                      addrVal, IsInitialization_t(isInit)); \
      break; \
    }
#include "swift/AST/ReferenceStorage.def"

    break;
  }
  case SILInstructionKind::AllocStackInst:
  case SILInstructionKind::MetatypeInst: {

    SILType Ty;
    if (parseSILType(Ty))
      return true;

    if (Opcode == SILInstructionKind::AllocStackInst) {
      SILDebugVariable VarInfo;
      if (parseSILDebugVar(VarInfo) ||
          parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createAllocStack(InstLoc, Ty, VarInfo);
    } else {
      assert(Opcode == SILInstructionKind::MetatypeInst);
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createMetatype(InstLoc, Ty);
    }
    break;
  }
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst: {
    bool IsObjC = false;
    bool OnStack = false;
    SmallVector<SILType, 2> ElementTypes;
    SmallVector<SILValue, 2> ElementCounts;
    while (P.consumeIf(tok::l_square)) {
      Identifier Id;
      parseSILIdentifier(Id, diag::expected_in_attribute_list);
      StringRef Optional = Id.str();
      if (Optional == "objc") {
        IsObjC = true;
      } else if (Optional == "stack") {
        OnStack = true;
      } else if (Optional == "tail_elems") {
        SILType ElemTy;
        if (parseSILType(ElemTy) ||
            !P.Tok.isAnyOperator() ||
            P.Tok.getText() != "*")
          return true;
        P.consumeToken();

        SILValue ElemCount;
        if (parseTypedValueRef(ElemCount, B))
          return true;

        ElementTypes.push_back(ElemTy);
        ElementCounts.push_back(ElemCount);
      } else {
        return true;
      }
      P.parseToken(tok::r_square, diag::expected_in_attribute_list);
    }
    SILValue Metadata;
    if (Opcode == SILInstructionKind::AllocRefDynamicInst) {
      if (parseTypedValueRef(Metadata, B) ||
          P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
        return true;
    }

    SILType ObjectType;
    if (parseSILType(ObjectType))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    if (IsObjC && !ElementTypes.empty()) {
      P.diagnose(P.Tok, diag::sil_objc_with_tail_elements);
      return true;
    }
    if (Opcode == SILInstructionKind::AllocRefDynamicInst) {
      if (OnStack)
        return true;

      ResultVal = B.createAllocRefDynamic(InstLoc, Metadata, ObjectType,
                                          IsObjC, ElementTypes, ElementCounts);
    } else {
      ResultVal = B.createAllocRef(InstLoc, ObjectType, IsObjC, OnStack,
                                   ElementTypes, ElementCounts);
    }
    break;
  }

  case SILInstructionKind::DeallocStackInst:
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocStack(InstLoc, Val);
    break;
  case SILInstructionKind::DeallocRefInst: {
    bool OnStack = false;
    if (parseSILOptional(OnStack, *this, "stack"))
      return true;

    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeallocRef(InstLoc, Val, OnStack);
    break;
  }
  case SILInstructionKind::DeallocPartialRefInst: {
    SILValue Metatype, Instance;
    if (parseTypedValueRef(Instance, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(Metatype, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createDeallocPartialRef(InstLoc, Instance, Metatype);
    break;
  }
  case SILInstructionKind::DeallocBoxInst:
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createDeallocBox(InstLoc, Val);
    break;
  case SILInstructionKind::ValueMetatypeInst:
  case SILInstructionKind::ExistentialMetatypeInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    switch (Opcode) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::ValueMetatypeInst:
      ResultVal = B.createValueMetatype(InstLoc, Ty, Val);
      break;
    case SILInstructionKind::ExistentialMetatypeInst:
      ResultVal = B.createExistentialMetatype(InstLoc, Ty, Val);
      break;
    case SILInstructionKind::DeallocBoxInst:
      ResultVal = B.createDeallocBox(InstLoc, Val);
      break;
    }
    break;
  }
  case SILInstructionKind::DeallocExistentialBoxInst: {
    CanType ConcreteTy;
    if (parseTypedValueRef(Val, B)
        || P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",")
        || P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$")
        || parseASTType(ConcreteTy)
        || parseSILDebugLocation(InstLoc, B))
      return true;
    
    ResultVal = B.createDeallocExistentialBox(InstLoc, ConcreteTy, Val);
    break;
  }
  case SILInstructionKind::TupleInst: {
    // Tuple instructions have two different syntaxes, one for simple tuple
    // types, one for complicated ones.
    if (P.Tok.isNot(tok::sil_dollar)) {
      // If there is no type, parse the simple form.
      if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
        return true;
      
      // TODO: Check for a type here.  This is how tuples with "interesting"
      // types are described.
      
      // This form is used with tuples that have elements with no names or
      // default values.
      SmallVector<TupleTypeElt, 4> TypeElts;
      if (P.Tok.isNot(tok::r_paren)) {
        do {
          if (parseTypedValueRef(Val, B)) return true;
          OpList.push_back(Val);
          TypeElts.push_back(Val->getType().getASTType());
        } while (P.consumeIf(tok::comma));
      }
      HadError |= P.parseToken(tok::r_paren,
                               diag::expected_tok_in_sil_instr,")");

      auto Ty = TupleType::get(TypeElts, P.Context);
      auto Ty2 = SILType::getPrimitiveObjectType(Ty->getCanonicalType());
      if (parseSILDebugLocation(InstLoc, B))
        return true;
      ResultVal = B.createTuple(InstLoc, Ty2, OpList);
      break;
    }
    
    // Otherwise, parse the fully general form.
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
      return true;
    
    TupleType *TT = Ty.getAs<TupleType>();
    if (TT == nullptr) {
      P.diagnose(OpcodeLoc, diag::expected_tuple_type_in_tuple);
      return true;
    }
    
    SmallVector<TupleTypeElt, 4> TypeElts;
    if (P.Tok.isNot(tok::r_paren)) {
      do {
        if (TypeElts.size() > TT->getNumElements()) {
          P.diagnose(P.Tok, diag::sil_tuple_inst_wrong_value_count,
                     TT->getNumElements());
          return true;
        }
        Type EltTy = TT->getElement(TypeElts.size()).getType();
        if (parseValueRef(Val,
                 SILType::getPrimitiveObjectType(EltTy->getCanonicalType()),
                          RegularLocation(P.Tok.getLoc()), B))
          return true;
        OpList.push_back(Val);
        TypeElts.push_back(Val->getType().getASTType());
      } while (P.consumeIf(tok::comma));
    }
    HadError |= P.parseToken(tok::r_paren,
                             diag::expected_tok_in_sil_instr,")");

    if (TypeElts.size() != TT->getNumElements()) {
      P.diagnose(OpcodeLoc, diag::sil_tuple_inst_wrong_value_count,
                 TT->getNumElements());
      return true;
    }

    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createTuple(InstLoc, Ty, OpList);
    break;
  }
  case SILInstructionKind::EnumInst: {
    SILType Ty;
    SILDeclRef Elt;
    SILValue Operand;
    if (parseSILType(Ty) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILDeclRef(Elt))
      return true;
    
    if (P.Tok.is(tok::comma) && !peekSILDebugLocation(P)) {
      P.consumeToken(tok::comma);
      if (parseTypedValueRef(Operand, B))
        return true;
    }
    
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createEnum(InstLoc, Operand,
                              cast<EnumElementDecl>(Elt.getDecl()), Ty);
    break;
  }
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
    SILValue Operand;
    SILDeclRef EltRef;
    if (parseTypedValueRef(Operand, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILDeclRef(EltRef) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    EnumElementDecl *Elt = cast<EnumElementDecl>(EltRef.getDecl());
    auto ResultTy = Operand->getType().getEnumElementType(Elt, SILMod);
    
    switch (Opcode) {
    case swift::SILInstructionKind::InitEnumDataAddrInst:
      ResultVal = B.createInitEnumDataAddr(InstLoc, Operand, Elt, ResultTy);
      break;
    case swift::SILInstructionKind::UncheckedTakeEnumDataAddrInst:
      ResultVal = B.createUncheckedTakeEnumDataAddr(InstLoc, Operand, Elt,
                                                    ResultTy);
      break;
    case swift::SILInstructionKind::UncheckedEnumDataInst:
      ResultVal = B.createUncheckedEnumData(InstLoc, Operand, Elt, ResultTy);
      break;
    default:
      llvm_unreachable("switch out of sync");
    }
    break;
  }
  case SILInstructionKind::InjectEnumAddrInst: {
    SILValue Operand;
    SILDeclRef EltRef;
    if (parseTypedValueRef(Operand, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILDeclRef(EltRef) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    EnumElementDecl *Elt = cast<EnumElementDecl>(EltRef.getDecl());
    ResultVal = B.createInjectEnumAddr(InstLoc, Operand, Elt);
    break;
  }
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::TupleExtractInst: {
    SourceLoc NameLoc;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    unsigned Field = 0;
    TupleType *TT = Val->getType().getAs<TupleType>();
    if (P.Tok.isNot(tok::integer_literal) ||
        parseIntegerLiteral(P.Tok.getText(), 10, Field) ||
        Field >= TT->getNumElements()) {
      P.diagnose(P.Tok, diag::sil_tuple_inst_wrong_field);
      return true;
    }
    P.consumeToken(tok::integer_literal);
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    auto ResultTy = TT->getElement(Field).getType()->getCanonicalType();
    if (Opcode == SILInstructionKind::TupleElementAddrInst)
      ResultVal = B.createTupleElementAddr(InstLoc, Val, Field,
                                  SILType::getPrimitiveAddressType(ResultTy));
    else
      ResultVal = B.createTupleExtract(InstLoc, Val, Field,
                          SILType::getPrimitiveObjectType(ResultTy));
    break;
  }
  case SILInstructionKind::ReturnInst: {
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createReturn(InstLoc, Val);
    break;
  }
  case SILInstructionKind::ThrowInst: {
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createThrow(InstLoc, Val);
    break;
  }
  case SILInstructionKind::UnwindInst: {
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createUnwind(InstLoc);
    break;
  }
  case SILInstructionKind::YieldInst: {
    SmallVector<SILValue, 6> values;

    // Parse a parenthesized (unless length-1), comma-separated list
    // of yielded values.
    if (P.consumeIf(tok::l_paren)) {
      if (!P.Tok.is(tok::r_paren)) {
        do {
          if (parseTypedValueRef(Val, B))
            return true;
          values.push_back(Val);
        } while (P.consumeIf(tok::comma));
      }

      if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")"))
        return true;

    } else {
      if (parseTypedValueRef(Val, B))
        return true;
      values.push_back(Val);
    }

    Identifier resumeName, unwindName;
    SourceLoc resumeLoc, unwindLoc;
    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("resume") ||
        parseSILIdentifier(resumeName, resumeLoc,
                           diag::expected_sil_block_name) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("unwind") ||
        parseSILIdentifier(unwindName, unwindLoc,
                           diag::expected_sil_block_name) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    auto resumeBB = getBBForReference(resumeName, resumeLoc);
    auto unwindBB = getBBForReference(unwindName, unwindLoc);
    ResultVal = B.createYield(InstLoc, values, resumeBB, unwindBB);
    break;
  }
  case SILInstructionKind::BranchInst: {
    Identifier BBName;
    SourceLoc NameLoc;
    if (parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name))
      return true;

    SmallVector<SILValue, 6> Args;
    if (parseSILBBArgsAtBranch(Args, B))
      return true;

    if (parseSILDebugLocation(InstLoc, B))
      return true;

    // Note, the basic block here could be a reference to an undefined
    // basic block, which will be parsed later on.
    ResultVal = B.createBranch(InstLoc, getBBForReference(BBName, NameLoc),
                               Args);
    break;
  }
  case SILInstructionKind::CondBranchInst: {
    UnresolvedValueName Cond;
    Identifier BBName, BBName2;
    SourceLoc NameLoc, NameLoc2;
    SmallVector<SILValue, 6> Args, Args2;
    if (parseValueName(Cond) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name) ||
        parseSILBBArgsAtBranch(Args, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILIdentifier(BBName2, NameLoc2,
                           diag::expected_sil_block_name) ||
        parseSILBBArgsAtBranch(Args2, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    auto I1Ty =
      SILType::getBuiltinIntegerType(1, SILMod.getASTContext());
    SILValue CondVal = getLocalValue(Cond, I1Ty, InstLoc, B);
    ResultVal = B.createCondBranch(InstLoc, CondVal,
                                   getBBForReference(BBName, NameLoc),
                                   Args,
                                   getBBForReference(BBName2, NameLoc2),
                                   Args2);
    break;
  }
  case SILInstructionKind::UnreachableInst:
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createUnreachable(InstLoc);
    break;
    
  case SILInstructionKind::ClassMethodInst:
  case SILInstructionKind::SuperMethodInst:
  case SILInstructionKind::ObjCMethodInst:
  case SILInstructionKind::ObjCSuperMethodInst: {
    SILDeclRef Member;
    SILType MethodTy;
    SourceLoc TyLoc;
    SmallVector<ValueDecl *, 4> values;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;

    if (parseSILDeclRef(Member, true))
      return true;

    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILType(MethodTy, TyLoc) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    switch (Opcode) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::ClassMethodInst:
      ResultVal = B.createClassMethod(InstLoc, Val, Member, MethodTy);
      break;
    case SILInstructionKind::SuperMethodInst:
      ResultVal = B.createSuperMethod(InstLoc, Val, Member, MethodTy);
      break;
    case SILInstructionKind::ObjCMethodInst:
      ResultVal = B.createObjCMethod(InstLoc, Val, Member, MethodTy);
      break;
    case SILInstructionKind::ObjCSuperMethodInst:
      ResultVal = B.createObjCSuperMethod(InstLoc, Val, Member, MethodTy);
      break;
    }
    break;
  }
  case SILInstructionKind::WitnessMethodInst: {
    CanType LookupTy;
    SILDeclRef Member;
    SILType MethodTy;
    SourceLoc TyLoc;
    if (P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTType(LookupTy) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ","))
      return true;
    if (parseSILDeclRef(Member, true))
      return true;
    // Optional operand.
    SILValue Operand;
    if (P.Tok.is(tok::comma)) {
      P.consumeToken(tok::comma);
      if (parseTypedValueRef(Operand, B))
        return true;
    }
    if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
        parseSILType(MethodTy, TyLoc) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    // If LookupTy is a non-archetype, look up its conformance.
    ProtocolDecl *proto
      = dyn_cast<ProtocolDecl>(Member.getDecl()->getDeclContext());
    if (!proto) {
      P.diagnose(TyLoc, diag::sil_witness_method_not_protocol);
      return true;
    }
    auto conformance = P.SF.getParentModule()->lookupConformance(LookupTy, proto);
    if (!conformance) {
      P.diagnose(TyLoc, diag::sil_witness_method_type_does_not_conform);
      return true;
    }

    ResultVal = B.createWitnessMethod(InstLoc, LookupTy, *conformance, Member,
                                      MethodTy);
    break;
  }
  case SILInstructionKind::CopyAddrInst: {
    bool IsTake = false, IsInit = false;
    UnresolvedValueName SrcLName;
    SILValue DestLVal;
    SourceLoc ToLoc, DestLoc;
    Identifier ToToken;
    if (parseSILOptional(IsTake, *this, "take") || parseValueName(SrcLName) ||
        parseSILIdentifier(ToToken, ToLoc,
                           diag::expected_tok_in_sil_instr, "to") ||
        parseSILOptional(IsInit, *this, "initialization") ||
        parseTypedValueRef(DestLVal, DestLoc, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }

    if (!DestLVal->getType().isAddress()) {
      P.diagnose(DestLoc, diag::sil_invalid_instr_operands);
      return true;
    }

    SILValue SrcLVal = getLocalValue(SrcLName, DestLVal->getType(), InstLoc, B);
    ResultVal = B.createCopyAddr(InstLoc, SrcLVal, DestLVal,
                                 IsTake_t(IsTake),
                                 IsInitialization_t(IsInit));
    break;
  }
  case SILInstructionKind::BindMemoryInst: {
    SILValue IndexVal;
    Identifier ToToken;
    SourceLoc ToLoc;
    SILType EltTy;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(IndexVal, B) ||
        parseSILIdentifier(ToToken, ToLoc,
                           diag::expected_tok_in_sil_instr, "to") ||
        parseSILType(EltTy) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (ToToken.str() != "to") {
      P.diagnose(ToLoc, diag::expected_tok_in_sil_instr, "to");
      return true;
    }
    ResultVal = B.createBindMemory(InstLoc, Val, IndexVal, EltTy);
    break;
  }
  case SILInstructionKind::ObjectInst:
  case SILInstructionKind::StructInst: {
    SILType Ty;
    if (parseSILType(Ty) ||
        P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
      return true;

    // Parse a list of SILValue.
    bool OpsAreTailElems = false;
    unsigned NumBaseElems = 0;
    if (P.Tok.isNot(tok::r_paren)) {
      do {
        if (Opcode == SILInstructionKind::ObjectInst) {
          if (parseSILOptional(OpsAreTailElems, *this, "tail_elems"))
            return true;
        }
        if (parseTypedValueRef(Val, B)) return true;
        OpList.push_back(Val);
        if (!OpsAreTailElems)
          NumBaseElems = OpList.size();
      } while (P.consumeIf(tok::comma));
    }
    if (P.parseToken(tok::r_paren,
                     diag::expected_tok_in_sil_instr,")") ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    if (Opcode == SILInstructionKind::StructInst) {
      ResultVal = B.createStruct(InstLoc, Ty, OpList);
    } else {
      ResultVal = B.createObject(InstLoc, Ty, OpList, NumBaseElems);
    }
    break;
  }
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::StructExtractInst: {
    ValueDecl *FieldV;
    SourceLoc NameLoc = P.Tok.getLoc();
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILDottedPath(FieldV) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    if (!FieldV || !isa<VarDecl>(FieldV)) {
      P.diagnose(NameLoc, diag::sil_struct_inst_wrong_field);
      return true;
    }
    VarDecl *Field = cast<VarDecl>(FieldV);

    // FIXME: substitution means this type should be explicit to improve
    // performance.
    auto ResultTy = Val->getType().getFieldType(Field, SILMod);
    if (Opcode == SILInstructionKind::StructElementAddrInst)
      ResultVal = B.createStructElementAddr(InstLoc, Val, Field,
                                            ResultTy.getAddressType());
    else
      ResultVal = B.createStructExtract(InstLoc, Val, Field,
                                        ResultTy.getObjectType());
    break;
  }
  case SILInstructionKind::RefElementAddrInst: {
    ValueDecl *FieldV;
    SourceLoc NameLoc;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILDottedPath(FieldV) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    if (!FieldV || !isa<VarDecl>(FieldV)) {
      P.diagnose(NameLoc, diag::sil_ref_inst_wrong_field);
      return true;
    }
    VarDecl *Field = cast<VarDecl>(FieldV);
    auto ResultTy = Val->getType().getFieldType(Field, SILMod);
    ResultVal = B.createRefElementAddr(InstLoc, Val, Field, ResultTy);
    break;
  }
  case SILInstructionKind::RefTailAddrInst: {
    SourceLoc NameLoc;
    SILType ResultObjTy;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILType(ResultObjTy) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    SILType ResultTy = ResultObjTy.getAddressType();
    ResultVal = B.createRefTailAddr(InstLoc, Val, ResultTy);
    break;
  }
  case SILInstructionKind::IndexAddrInst: {
    SILValue IndexVal;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(IndexVal, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createIndexAddr(InstLoc, Val, IndexVal);
    break;
  }
  case SILInstructionKind::TailAddrInst: {
    SILValue IndexVal;
    SILType ResultObjTy;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(IndexVal, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILType(ResultObjTy) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    SILType ResultTy = ResultObjTy.getAddressType();
    ResultVal = B.createTailAddr(InstLoc, Val, IndexVal, ResultTy);
    break;
  }
  case SILInstructionKind::IndexRawPointerInst: {
    SILValue IndexVal;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseTypedValueRef(IndexVal, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createIndexRawPointer(InstLoc, Val, IndexVal);
    break;
  }
  case SILInstructionKind::ObjCProtocolInst: {
    Identifier ProtocolName;
    SILType Ty;
    if (P.parseToken(tok::pound, diag::expected_sil_constant) ||
        parseSILIdentifier(ProtocolName, diag::expected_sil_constant) ||
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
        parseSILType(Ty) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    // Find the decl for the protocol name.
    ValueDecl *VD;
    SmallVector<ValueDecl*, 4> CurModuleResults;
    // Perform a module level lookup on the first component of the
    // fully-qualified name.
    P.SF.getParentModule()->lookupValue(ModuleDecl::AccessPathTy(), ProtocolName,
                                        NLKind::UnqualifiedLookup,
                                        CurModuleResults);
    assert(CurModuleResults.size() == 1);
    VD = CurModuleResults[0];
    ResultVal = B.createObjCProtocol(InstLoc, cast<ProtocolDecl>(VD), Ty);
    break;
  }
  case SILInstructionKind::AllocGlobalInst: {
    Identifier GlobalName;
    SourceLoc IdLoc;
    if (P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
        parseSILIdentifier(GlobalName, IdLoc, diag::expected_sil_value_name) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    // Go through list of global variables in the SILModule.
    SILGlobalVariable *global = SILMod.lookUpGlobalVariable(GlobalName.str());
    if (!global) {
      P.diagnose(IdLoc, diag::sil_global_variable_not_found, GlobalName);
      return true;
    }

    ResultVal = B.createAllocGlobal(InstLoc, global);
    break;
  }
  case SILInstructionKind::GlobalAddrInst:
  case SILInstructionKind::GlobalValueInst: {
    Identifier GlobalName;
    SourceLoc IdLoc;
    SILType Ty;
    if (P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
        parseSILIdentifier(GlobalName, IdLoc, diag::expected_sil_value_name) ||
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
        parseSILType(Ty) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    // Go through list of global variables in the SILModule.
    SILGlobalVariable *global = SILMod.lookUpGlobalVariable(GlobalName.str());
    if (!global) {
      P.diagnose(IdLoc, diag::sil_global_variable_not_found, GlobalName);
      return true;
    }

    SILType expectedType = (Opcode == SILInstructionKind::GlobalAddrInst ?
                            global->getLoweredType().getAddressType() :
                            global->getLoweredType());
    if (expectedType != Ty) {
      P.diagnose(IdLoc, diag::sil_value_use_type_mismatch, GlobalName.str(),
                 global->getLoweredType().getASTType(),
                 Ty.getASTType());
      return true;
    }

    if (Opcode == SILInstructionKind::GlobalAddrInst) {
      ResultVal = B.createGlobalAddr(InstLoc, global);
    } else {
      ResultVal = B.createGlobalValue(InstLoc, global);
    }
    break;
  }
  case SILInstructionKind::SelectEnumInst:
  case SILInstructionKind::SelectEnumAddrInst: {
    if (parseTypedValueRef(Val, B))
      return true;

    SmallVector<std::pair<EnumElementDecl*, UnresolvedValueName>, 4>
      CaseValueNames;
    Optional<UnresolvedValueName> DefaultValueName;
    while (P.consumeIf(tok::comma)) {
      Identifier BBName;
      SourceLoc BBLoc;
      // Parse 'default' sil-value.
     UnresolvedValueName tmp;
      if (P.consumeIf(tok::kw_default)) {
        if (parseValueName(tmp))
          return true;
        DefaultValueName = tmp;
        break;
      }

      // Parse 'case' sil-decl-ref ':' sil-value.
      if (P.consumeIf(tok::kw_case)) {
        SILDeclRef ElemRef;
        if (parseSILDeclRef(ElemRef))
          return true;
        assert(ElemRef.hasDecl() && isa<EnumElementDecl>(ElemRef.getDecl()));
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
        parseValueName(tmp);
        CaseValueNames.push_back(std::make_pair(
                                     cast<EnumElementDecl>(ElemRef.getDecl()),
                                     tmp));
        continue;
      }

      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
      return true;
    }
    
    // Parse the type of the result operands.
    SILType ResultType;
    if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":")
        || parseSILType(ResultType) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    // Resolve the results.
    SmallVector<std::pair<EnumElementDecl*, SILValue>, 4> CaseValues;
    SILValue DefaultValue;
    if (DefaultValueName)
      DefaultValue = getLocalValue(*DefaultValueName, ResultType, InstLoc, B);
    for (auto &caseName : CaseValueNames)
      CaseValues.push_back(std::make_pair(
          caseName.first,
          getLocalValue(caseName.second, ResultType, InstLoc, B)));

    if (Opcode == SILInstructionKind::SelectEnumInst)
      ResultVal = B.createSelectEnum(InstLoc, Val, ResultType,
                                     DefaultValue, CaseValues);
    else
      ResultVal = B.createSelectEnumAddr(InstLoc, Val, ResultType,
                                         DefaultValue, CaseValues);
    break;
  }
      
  case SILInstructionKind::SwitchEnumInst:
  case SILInstructionKind::SwitchEnumAddrInst: {
    if (parseTypedValueRef(Val, B))
      return true;

    SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 4> CaseBBs;
    SILBasicBlock *DefaultBB = nullptr;
    while (!peekSILDebugLocation(P) && P.consumeIf(tok::comma)) {
      Identifier BBName;
      SourceLoc BBLoc;
      // Parse 'default' sil-identifier.
      if (P.consumeIf(tok::kw_default)) {
        parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
        DefaultBB = getBBForReference(BBName, BBLoc);
        break;
      }

      // Parse 'case' sil-decl-ref ':' sil-identifier.
      if (P.consumeIf(tok::kw_case)) {
        SILDeclRef ElemRef;
        if (parseSILDeclRef(ElemRef))
          return true;
        assert(ElemRef.hasDecl() && isa<EnumElementDecl>(ElemRef.getDecl()));
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
        parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
        CaseBBs.push_back( {cast<EnumElementDecl>(ElemRef.getDecl()),
                            getBBForReference(BBName, BBLoc)} );
        continue;
      }

      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
      return true;
    }
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    if (Opcode == SILInstructionKind::SwitchEnumInst)
      ResultVal = B.createSwitchEnum(InstLoc, Val, DefaultBB, CaseBBs);
    else
      ResultVal = B.createSwitchEnumAddr(InstLoc, Val, DefaultBB, CaseBBs);
    break;
  }
  case SILInstructionKind::SwitchValueInst: {
    if (parseTypedValueRef(Val, B))
      return true;

    SmallVector<std::pair<SILValue, SILBasicBlock *>, 4> CaseBBs;
    SILBasicBlock *DefaultBB = nullptr;
    while (!peekSILDebugLocation(P) && P.consumeIf(tok::comma)) {
      Identifier BBName;
      SourceLoc BBLoc;
      SILValue CaseVal;
      
      // Parse 'default' sil-identifier.
      if (P.consumeIf(tok::kw_default)) {
        parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
        DefaultBB = getBBForReference(BBName, BBLoc);
        break;
      }

      // Parse 'case' value-ref ':' sil-identifier.
      if (P.consumeIf(tok::kw_case)) {
        if (parseValueRef(CaseVal, Val->getType(),
                          RegularLocation(P.Tok.getLoc()), B)) {
          // TODO: Issue a proper error message here
          P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "reference to a value");
          return true;
        }

        auto intTy = Val->getType().getAs<BuiltinIntegerType>();
        auto functionTy = Val->getType().getAs<SILFunctionType>();
        if (!intTy && !functionTy) {
          P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
          return true;
        }

        if (intTy) {
          // If it is a switch on an integer type, check that all case values
          // are integer literals or undef.
          if (!isa<SILUndef>(CaseVal)) {
            auto *IL = dyn_cast<IntegerLiteralInst>(CaseVal);
            if (!IL) {
              P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
              return true;
            }
            APInt CaseValue = IL->getValue();

            if (CaseValue.getBitWidth() != intTy->getGreatestWidth())
              CaseVal = B.createIntegerLiteral(
                  IL->getLoc(), Val->getType(),
                  CaseValue.zextOrTrunc(intTy->getGreatestWidth()));
          }
        }

        if (functionTy) {
          // If it is a switch on a function type, check that all case values
          // are function references or undef.
          if (!isa<SILUndef>(CaseVal)) {
            auto *FR = dyn_cast<FunctionRefInst>(CaseVal);
            if (!FR) {
              if (auto *CF = dyn_cast<ConvertFunctionInst>(CaseVal)) {
                FR = dyn_cast<FunctionRefInst>(CF->getOperand());
              }
            }
            if (!FR) {
              P.diagnose(P.Tok, diag::sil_integer_literal_not_integer_type);
              return true;
            }
          }
        }

        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
        parseSILIdentifier(BBName, BBLoc, diag::expected_sil_block_name);
        CaseBBs.push_back({CaseVal, getBBForReference(BBName, BBLoc)});
        continue;
      }

      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
      return true;
    }
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createSwitchValue(InstLoc, Val, DefaultBB, CaseBBs);
    break;
  }
  case SILInstructionKind::SelectValueInst: {
    if (parseTypedValueRef(Val, B))
      return true;

    SmallVector<std::pair<UnresolvedValueName, UnresolvedValueName>, 4>
      CaseValueAndResultNames;
    Optional<UnresolvedValueName> DefaultResultName;
    while (P.consumeIf(tok::comma)) {
      Identifier BBName;
      SourceLoc BBLoc;
      // Parse 'default' sil-value.
      UnresolvedValueName tmp;
      if (P.consumeIf(tok::kw_default)) {
        if (parseValueName(tmp))
          return true;
        DefaultResultName = tmp;
        break;
      }

      // Parse 'case' sil-decl-ref ':' sil-value.
      if (P.consumeIf(tok::kw_case)) {
        UnresolvedValueName casevalue;
        parseValueName(casevalue);
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":");
        parseValueName(tmp);
        CaseValueAndResultNames.push_back(std::make_pair(
                                          casevalue,
                                          tmp));
        continue;
      }

      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "case or default");
      return true;
    }

    if (!DefaultResultName) {
      P.diagnose(P.Tok, diag::expected_tok_in_sil_instr, "default");
      return true;
    }

    // Parse the type of the result operands.
    SILType ResultType;
    if (P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
        parseSILType(ResultType) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    // Resolve the results.
    SmallVector<std::pair<SILValue, SILValue>, 4> CaseValues;
    SILValue DefaultValue;
    if (DefaultResultName)
      DefaultValue = getLocalValue(*DefaultResultName, ResultType, InstLoc, B);
    SILType ValType = Val->getType();
    for (auto &caseName : CaseValueAndResultNames)
      CaseValues.push_back(std::make_pair(
          getLocalValue(caseName.first, ValType, InstLoc, B),
          getLocalValue(caseName.second, ResultType, InstLoc, B)));

    ResultVal = B.createSelectValue(InstLoc, Val, ResultType,
                                    DefaultValue, CaseValues);
    break;
  }
  case SILInstructionKind::DeinitExistentialAddrInst: {
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeinitExistentialAddr(InstLoc, Val);
    break;
  }
  case SILInstructionKind::DeinitExistentialValueInst: {
    if (parseTypedValueRef(Val, B) || parseSILDebugLocation(InstLoc, B))
      return true;
    ResultVal = B.createDeinitExistentialValue(InstLoc, Val);
    break;
  }
  case SILInstructionKind::InitExistentialAddrInst: {
    CanType Ty;
    SourceLoc TyLoc;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTType(Ty, TyLoc) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    // Lower the type at the abstraction level of the existential.
    auto archetype
      = OpenedArchetypeType::get(Val->getType().getASTType())
        ->getCanonicalType();
    
    auto &F = B.getFunction();
    SILType LoweredTy = F.getLoweredType(
        Lowering::AbstractionPattern(archetype), Ty)
      .getAddressType();
    
    // Collect conformances for the type.
    ArrayRef<ProtocolConformanceRef> conformances
      = collectExistentialConformances(P, Ty, TyLoc,
                                       Val->getType().getASTType());
    
    ResultVal = B.createInitExistentialAddr(InstLoc, Val, Ty, LoweredTy,
                                        conformances);
    break;
  }
  case SILInstructionKind::InitExistentialValueInst: {
    CanType FormalConcreteTy;
    SILType ExistentialTy;
    SourceLoc TyLoc;

    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTType(FormalConcreteTy, TyLoc) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILType(ExistentialTy) || parseSILDebugLocation(InstLoc, B))
      return true;

    ArrayRef<ProtocolConformanceRef> conformances =
        collectExistentialConformances(P, FormalConcreteTy, TyLoc,
                                       ExistentialTy.getASTType());

    ResultVal = B.createInitExistentialValue(
        InstLoc, ExistentialTy, FormalConcreteTy, Val, conformances);
    break;
  }
  case SILInstructionKind::AllocExistentialBoxInst: {
    SILType ExistentialTy;
    CanType ConcreteFormalTy;
    SourceLoc TyLoc;
    
    if (parseSILType(ExistentialTy) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTType(ConcreteFormalTy, TyLoc) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    // Collect conformances for the type.
    ArrayRef<ProtocolConformanceRef> conformances
      = collectExistentialConformances(P, ConcreteFormalTy, TyLoc,
                                       ExistentialTy.getASTType());
    
    ResultVal = B.createAllocExistentialBox(InstLoc, ExistentialTy,
                                            ConcreteFormalTy, conformances);
    
    break;
  }
  case SILInstructionKind::InitExistentialRefInst: {
    CanType FormalConcreteTy;
    SILType ExistentialTy;
    SourceLoc TyLoc;

    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
        P.parseToken(tok::sil_dollar, diag::expected_tok_in_sil_instr, "$") ||
        parseASTType(FormalConcreteTy, TyLoc) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILType(ExistentialTy) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    ArrayRef<ProtocolConformanceRef> conformances
      = collectExistentialConformances(P, FormalConcreteTy, TyLoc,
                            ExistentialTy.getASTType());

    // FIXME: Conformances in InitExistentialRefInst is currently not included
    // in SIL.rst.
    ResultVal = B.createInitExistentialRef(InstLoc, ExistentialTy,
                                           FormalConcreteTy, Val,
                                           conformances);
    break;
  }
  case SILInstructionKind::InitExistentialMetatypeInst: {
    SourceLoc TyLoc;
    SILType ExistentialTy;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILType(ExistentialTy, TyLoc) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    auto baseExType = ExistentialTy.getASTType();
    auto formalConcreteType = Val->getType().getASTType();
    while (auto instExType = dyn_cast<ExistentialMetatypeType>(baseExType)) {
      baseExType = instExType.getInstanceType();
      formalConcreteType =
        cast<MetatypeType>(formalConcreteType).getInstanceType();
    }

    ArrayRef<ProtocolConformanceRef> conformances
      = collectExistentialConformances(P, formalConcreteType, TyLoc,
                                       baseExType);
    
    ResultVal = B.createInitExistentialMetatype(InstLoc, Val, ExistentialTy,
                                                conformances);
    break;
  }
  case SILInstructionKind::DynamicMethodBranchInst: {
    SILDeclRef Member;
    Identifier BBName, BBName2;
    SourceLoc NameLoc, NameLoc2;
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILDeclRef(Member) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILIdentifier(BBName2, NameLoc2,
                           diag::expected_sil_block_name) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    ResultVal = B.createDynamicMethodBranch(InstLoc, Val, Member,
                                            getBBForReference(BBName, NameLoc),
                                            getBBForReference(BBName2,
                                                              NameLoc2));
    break;
  }
  case SILInstructionKind::ProjectBlockStorageInst: {
    if (parseTypedValueRef(Val, B) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    ResultVal = B.createProjectBlockStorage(InstLoc, Val);
    break;
  }
  case SILInstructionKind::InitBlockStorageHeaderInst: {
    Identifier invoke, type;
    SourceLoc invokeLoc, typeLoc;
    
    UnresolvedValueName invokeName;
    SILType invokeTy;
    GenericEnvironment *invokeGenericEnv;
    
    SILType blockType;
    SmallVector<ParsedSubstitution, 4> parsedSubs;

    
    if (parseTypedValueRef(Val, B) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILIdentifier(invoke, invokeLoc,
                           diag::expected_tok_in_sil_instr, "invoke") ||
        parseValueName(invokeName) ||
        parseSubstitutions(parsedSubs) ||
        P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
        parseSILType(invokeTy, invokeGenericEnv) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseSILIdentifier(type, typeLoc,
                           diag::expected_tok_in_sil_instr, "type") ||
        parseSILType(blockType) ||
        parseSILDebugLocation(InstLoc, B))
      return true;
    
    if (invoke.str() != "invoke") {
      P.diagnose(invokeLoc, diag::expected_tok_in_sil_instr, "invoke");
      return true;
    }
    if (type.str() != "type") {
      P.diagnose(invokeLoc, diag::expected_tok_in_sil_instr, "type");
      return true;
    }
    
    auto invokeVal = getLocalValue(invokeName, invokeTy, InstLoc, B);

    SubstitutionMap subMap;
    if (!parsedSubs.empty()) {
      if (!invokeGenericEnv) {
        P.diagnose(typeLoc, diag::sil_substitutions_on_non_polymorphic_type);
        return true;
      }

      subMap = getApplySubstitutionsFromParsed(*this, invokeGenericEnv,
                                               parsedSubs);
      if (!subMap)
        return true;
    }
    
    ResultVal = B.createInitBlockStorageHeader(InstLoc, Val, invokeVal,
                                               blockType, subMap);
    break;
  }
  }

  // Match the results clause if we had one.
  if (resultClauseBegin.isValid()) {
    auto results = ResultVal->getResults();
    if (results.size() != resultNames.size()) {
      P.diagnose(resultClauseBegin, diag::wrong_result_count_in_sil_instr,
                 results.size());
    } else {
      for (size_t i : indices(results)) {
        setLocalValue(results[i], resultNames[i].first, resultNames[i].second);
      }
    }
  }

  return false;
}

bool SILParser::parseCallInstruction(SILLocation InstLoc,
                                     SILInstructionKind Opcode, SILBuilder &B,
                                     SILInstruction *&ResultVal) {
  UnresolvedValueName FnName;
  SmallVector<UnresolvedValueName, 4> ArgNames;

  auto PartialApplyConvention = ParameterConvention::Direct_Owned;
  bool IsNonThrowingApply = false;
  bool IsNoEscape = false;
  StringRef AttrName;

  while (parseSILOptional(AttrName, *this)) {
    if (AttrName.equals("nothrow"))
      IsNonThrowingApply = true;
    else if (AttrName.equals("callee_guaranteed"))
      PartialApplyConvention = ParameterConvention::Direct_Guaranteed;
    else if (AttrName.equals("on_stack"))
      IsNoEscape = true;
    else
      return true;
  }
  
  if (parseValueName(FnName))
    return true;
  SmallVector<ParsedSubstitution, 4> parsedSubs;
  if (parseSubstitutions(parsedSubs))
    return true;
    
  if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
    return true;

  if (P.Tok.isNot(tok::r_paren)) {
    do {
      UnresolvedValueName Arg;
      if (parseValueName(Arg)) return true;
      ArgNames.push_back(Arg);
    } while (P.consumeIf(tok::comma));
  }

  SILType Ty;
  SourceLoc TypeLoc;
  GenericEnvironment *GenericEnv = nullptr;
  if (P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")") ||
      P.parseToken(tok::colon, diag::expected_tok_in_sil_instr, ":") ||
      parseSILType(Ty, TypeLoc, GenericEnv))
    return true;

  auto FTI = Ty.getAs<SILFunctionType>();
  if (!FTI) {
    P.diagnose(TypeLoc, diag::expected_sil_type_kind, "be a function");
    return true;
  }

  SubstitutionMap subs;
  if (!parsedSubs.empty()) {
    if (!GenericEnv) {
      P.diagnose(TypeLoc, diag::sil_substitutions_on_non_polymorphic_type);
      return true;
    }
    subs = getApplySubstitutionsFromParsed(*this, GenericEnv, parsedSubs);
    if (!subs)
      return true;
  }

  SILValue FnVal = getLocalValue(FnName, Ty, InstLoc, B);

  SILType FnTy = FnVal->getType();
  CanSILFunctionType substFTI = FTI;
  if (!subs.empty()) {
    auto silFnTy = FnTy.castTo<SILFunctionType>();
    substFTI
      = silFnTy->substGenericArgs(SILMod, subs);
    FnTy = SILType::getPrimitiveObjectType(substFTI);
  }
  SILFunctionConventions substConv(substFTI, B.getModule());

  // Validate the operand count.
  if (substConv.getNumSILArguments() != ArgNames.size() &&
      Opcode != SILInstructionKind::PartialApplyInst) {
    P.diagnose(TypeLoc, diag::expected_sil_type_kind,
               "to have the same number of arg names as arg types");
    return true;
  }

  // Validate the coroutine kind.
  if (Opcode == SILInstructionKind::ApplyInst ||
      Opcode == SILInstructionKind::TryApplyInst) {
    if (FTI->getCoroutineKind() != SILCoroutineKind::None) {
      P.diagnose(TypeLoc, diag::expected_sil_type_kind,
                 "to not be a coroutine");
      return true;
    }
  } else if (Opcode == SILInstructionKind::BeginApplyInst) {
    if (FTI->getCoroutineKind() != SILCoroutineKind::YieldOnce) {
      P.diagnose(TypeLoc, diag::expected_sil_type_kind,
                 "to be a yield_once coroutine");
      return true;
    }
  } else {
    assert(Opcode == SILInstructionKind::PartialApplyInst);
    // partial_apply accepts all kinds of function
  }

  switch (Opcode) {
  default: llvm_unreachable("Unexpected case");
  case SILInstructionKind::ApplyInst : {
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    unsigned ArgNo = 0;
    SmallVector<SILValue, 4> Args;
    for (auto &ArgName : ArgNames) {
      SILType expectedTy = substConv.getSILArgumentType(ArgNo++);
      Args.push_back(getLocalValue(ArgName, expectedTy, InstLoc, B));
    }

    ResultVal = B.createApply(InstLoc, FnVal, subs, Args, IsNonThrowingApply);
    break;
  }
  case SILInstructionKind::BeginApplyInst: {
    if (parseSILDebugLocation(InstLoc, B))
      return true;
    
    unsigned ArgNo = 0;
    SmallVector<SILValue, 4> Args;
    for (auto &ArgName : ArgNames) {
      SILType expectedTy = substConv.getSILArgumentType(ArgNo++);
      Args.push_back(getLocalValue(ArgName, expectedTy, InstLoc, B));
    }

    ResultVal =
      B.createBeginApply(InstLoc, FnVal, subs, Args, IsNonThrowingApply);
    break;
  }
  case SILInstructionKind::PartialApplyInst: {
    if (parseSILDebugLocation(InstLoc, B))
      return true;

    // Compute the result type of the partial_apply, based on which arguments
    // are getting applied.
    SmallVector<SILValue, 4> Args;
    unsigned ArgNo = substConv.getNumSILArguments() - ArgNames.size();
    for (auto &ArgName : ArgNames) {
      SILType expectedTy = substConv.getSILArgumentType(ArgNo++);
      Args.push_back(getLocalValue(ArgName, expectedTy, InstLoc, B));
    }

    // FIXME: Why the arbitrary order difference in IRBuilder type argument?
    ResultVal = B.createPartialApply(
        InstLoc, FnVal, subs, Args, PartialApplyConvention,
        IsNoEscape ? PartialApplyInst::OnStackKind::OnStack
                   : PartialApplyInst::OnStackKind::NotOnStack);
    break;
  }
  case SILInstructionKind::TryApplyInst: {
    Identifier normalBBName, errorBBName;
    SourceLoc normalBBLoc, errorBBLoc;
    if (P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("normal") ||
        parseSILIdentifier(normalBBName, normalBBLoc,
                           diag::expected_sil_block_name) ||
        P.parseToken(tok::comma, diag::expected_tok_in_sil_instr, ",") ||
        parseVerbatim("error") ||
        parseSILIdentifier(errorBBName, errorBBLoc,
                           diag::expected_sil_block_name) ||
        parseSILDebugLocation(InstLoc, B))
      return true;

    unsigned argNo = 0;
    SmallVector<SILValue, 4> args;
    for (auto &argName : ArgNames) {
      SILType expectedTy = substConv.getSILArgumentType(argNo++);
      args.push_back(getLocalValue(argName, expectedTy, InstLoc, B));
    }

    SILBasicBlock *normalBB = getBBForReference(normalBBName, normalBBLoc);
    SILBasicBlock *errorBB = getBBForReference(errorBBName, errorBBLoc);
    ResultVal = B.createTryApply(InstLoc, FnVal, subs, args, normalBB, errorBB);
    break;
  }
  }
  return false;
}

bool SILParser::parseSILFunctionRef(SILLocation InstLoc,
                                    SILFunction *&ResultFn) {
  Identifier Name;
  SILType Ty;
  SourceLoc Loc = P.Tok.getLoc();
  if (parseGlobalName(Name) ||
      P.parseToken(tok::colon, diag::expected_sil_colon_value_ref) ||
      parseSILType(Ty))
    return true;

  auto FnTy = Ty.getAs<SILFunctionType>();
  if (!FnTy || !Ty.isObject()) {
    P.diagnose(Loc, diag::expected_sil_function_type);
    return true;
  }
  
  ResultFn = getGlobalNameForReference(Name, FnTy, Loc);
  return false;
}

/// True if the current token sequence looks like the start of a SIL
/// instruction. This can be one of:
///
/// 1. %name
/// 2. ()
/// 3. (%name1
/// 4. identifier | keyword
///   where the identifier is not followed by a ':' or '(', or it is
///   followed by '(' and is an instruction name.  The exceptions here
///   are for recognizing block names.
bool SILParser::isStartOfSILInstruction() {
  if (P.Tok.is(tok::sil_local_name))
    return true;
  if (P.Tok.is(tok::l_paren) &&
      (P.peekToken().is(tok::sil_local_name) || P.peekToken().is(tok::r_paren)))
    return true;
  if (P.Tok.is(tok::identifier) || P.Tok.isKeyword()) {
    auto &peek = P.peekToken();
    if (peek.is(tok::l_paren))
      return getOpcodeByName(P.Tok.getText()).hasValue();
    return !peek.is(tok::colon);
  }
  return false;
}

///   sil-basic-block:
///     sil-instruction+
///     identifier sil-bb-argument-list? ':' sil-instruction+
///   sil-bb-argument-list:
///     '(' sil-typed-valueref (',' sil-typed-valueref)+ ')'
bool SILParser::parseSILBasicBlock(SILBuilder &B) {
  SILBasicBlock *BB;

  // The basic block name is optional.
  if (P.Tok.is(tok::sil_local_name)) {
    BB = getBBForDefinition(Identifier(), SourceLoc());
  } else {
    Identifier BBName;
    SourceLoc NameLoc;
    if (parseSILIdentifier(BBName, NameLoc, diag::expected_sil_block_name))
      return true;

    BB = getBBForDefinition(BBName, NameLoc);
    // For now, since we always assume that PhiArguments have
    // ValueOwnershipKind::Any, do not parse or do anything special. Eventually
    // we will parse the convention.
    bool IsEntry = BB->isEntry();

    // If there is a basic block argument list, process it.
    if (P.consumeIf(tok::l_paren)) {
      do {
        SILType Ty;
        ValueOwnershipKind OwnershipKind = ValueOwnershipKind::Any;
        SourceLoc NameLoc;
        StringRef Name = P.Tok.getText();
        if (P.parseToken(tok::sil_local_name, NameLoc,
                         diag::expected_sil_value_name) ||
            P.parseToken(tok::colon, diag::expected_sil_colon_value_ref))
          return true;

        // If SILOwnership is enabled and we are not assuming that we are
        // parsing unqualified SIL, look for printed value ownership kinds.
        if (F->hasOwnership() &&
            parseSILOwnership(OwnershipKind))
          return true;

        if (parseSILType(Ty))
          return true;

        SILArgument *Arg;
        if (IsEntry) {
          Arg = BB->createFunctionArgument(Ty);
        } else {
          Arg = BB->createPhiArgument(Ty, OwnershipKind);
        }
        setLocalValue(Arg, Name, NameLoc);
      } while (P.consumeIf(tok::comma));
      
      if (P.parseToken(tok::r_paren, diag::sil_basicblock_arg_rparen))
        return true;
    }
    
    if (P.parseToken(tok::colon, diag::expected_sil_block_colon))
      return true;
  }
  
  // Make sure the block is at the end of the function so that forward
  // references don't affect block layout.
  F->getBlocks().remove(BB);
  F->getBlocks().push_back(BB);

  B.setInsertionPoint(BB);
  do {
    if (parseSILInstruction(B))
      return true;
  } while (isStartOfSILInstruction());

  return false;
}

///   decl-sil:   [[only in SIL mode]]
///     'sil' sil-linkage '@' identifier ':' sil-type decl-sil-body?
///   decl-sil-body:
///     '{' sil-basic-block+ '}'
bool SILParserTUState::parseDeclSIL(Parser &P) {
  // Inform the lexer that we're lexing the body of the SIL declaration.  Do
  // this before we consume the 'sil' token so that all later tokens are
  // properly handled.
  Lexer::SILBodyRAII Tmp(*P.L);

  P.consumeToken(tok::kw_sil);

  SILParser FunctionState(P);

  Optional<SILLinkage> FnLinkage;
  Identifier FnName;
  SILType FnType;
  SourceLoc FnNameLoc;

  Scope S(&P, ScopeKind::TopLevel);
  bool isTransparent = false;
  IsSerialized_t isSerialized = IsNotSerialized;
  bool isCanonical = false;
  IsDynamicallyReplaceable_t isDynamic = IsNotDynamic;
  bool hasOwnershipSSA = false;
  IsThunk_t isThunk = IsNotThunk;
  bool isGlobalInit = false, isWeakLinked = false;
  bool isWithoutActuallyEscapingThunk = false;
  Inline_t inlineStrategy = InlineDefault;
  OptimizationMode optimizationMode = OptimizationMode::NotSet;
  SmallVector<std::string, 1> Semantics;
  SmallVector<ParsedSpecAttr, 4> SpecAttrs;
  ValueDecl *ClangDecl = nullptr;
  EffectsKind MRK = EffectsKind::Unspecified;
  SILFunction *DynamicallyReplacedFunction = nullptr;
  Identifier objCReplacementFor;
  if (parseSILLinkage(FnLinkage, P) ||
      parseDeclSILOptional(
          &isTransparent, &isSerialized, &isCanonical, &hasOwnershipSSA,
          &isThunk, &isDynamic, &DynamicallyReplacedFunction,
          &objCReplacementFor, &isGlobalInit, &inlineStrategy, &optimizationMode, nullptr,
          &isWeakLinked, &isWithoutActuallyEscapingThunk, &Semantics,
          &SpecAttrs, &ClangDecl, &MRK, FunctionState, M) ||
      P.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
      P.parseIdentifier(FnName, FnNameLoc, diag::expected_sil_function_name) ||
      P.parseToken(tok::colon, diag::expected_sil_type))
    return true;
  {
    // Construct a Scope for the function body so TypeAliasDecl can be added to
    // the scope.
    Scope Body(&P, ScopeKind::FunctionBody);
    GenericEnvironment *GenericEnv;
    if (FunctionState.parseSILType(FnType, GenericEnv, true /*IsFuncDecl*/))
      return true;
    auto SILFnType = FnType.getAs<SILFunctionType>();
    if (!SILFnType || !FnType.isObject()) {
      P.diagnose(FnNameLoc, diag::expected_sil_function_type);
      return true;
    }
  
    FunctionState.F =
      FunctionState.getGlobalNameForDefinition(FnName, SILFnType, FnNameLoc);
    FunctionState.F->setBare(IsBare);
    FunctionState.F->setTransparent(IsTransparent_t(isTransparent));
    FunctionState.F->setSerialized(IsSerialized_t(isSerialized));
    FunctionState.F->setWasDeserializedCanonical(isCanonical);
    if (!hasOwnershipSSA)
      FunctionState.F->setOwnershipEliminated();
    FunctionState.F->setThunk(IsThunk_t(isThunk));
    FunctionState.F->setIsDynamic(isDynamic);
    FunctionState.F->setDynamicallyReplacedFunction(
        DynamicallyReplacedFunction);
    if (!objCReplacementFor.empty())
      FunctionState.F->setObjCReplacement(objCReplacementFor);
    FunctionState.F->setGlobalInit(isGlobalInit);
    FunctionState.F->setWeakLinked(isWeakLinked);
    FunctionState.F->setWithoutActuallyEscapingThunk(
      isWithoutActuallyEscapingThunk);
    FunctionState.F->setInlineStrategy(inlineStrategy);
    FunctionState.F->setOptimizationMode(optimizationMode);
    FunctionState.F->setEffectsKind(MRK);
    if (ClangDecl)
      FunctionState.F->setClangNodeOwner(ClangDecl);
    for (auto &Attr : Semantics) {
      FunctionState.F->addSemanticsAttr(Attr);
    }
    // Now that we have a SILFunction parse the body, if present.

    bool isDefinition = false;
    SourceLoc LBraceLoc = P.Tok.getLoc();

    if (P.consumeIf(tok::l_brace)) {
      isDefinition = true;

      FunctionState.ContextGenericEnv = GenericEnv;
      FunctionState.F->setGenericEnvironment(GenericEnv);

      if (GenericEnv && !SpecAttrs.empty()) {
        for (auto &Attr : SpecAttrs) {
          SmallVector<Requirement, 4> requirements;
          // Resolve types and convert requirements.
          FunctionState.convertRequirements(FunctionState.F,
                                            Attr.requirements, requirements);
          FunctionState.F->addSpecializeAttr(SILSpecializeAttr::create(
              FunctionState.F->getModule(), requirements, Attr.exported,
              Attr.kind));
        }
      }

      // Parse the basic block list.
      SILOpenedArchetypesTracker OpenedArchetypesTracker(FunctionState.F);
      SILBuilder B(*FunctionState.F);
      // Track the archetypes just like SILGen. This
      // is required for adding typedef operands to instructions.
      B.setOpenedArchetypesTracker(&OpenedArchetypesTracker);

      // Define a callback to be invoked on the deserialized types.
      auto OldParsedTypeCallback = FunctionState.ParsedTypeCallback;
      SWIFT_DEFER {
        FunctionState.ParsedTypeCallback = OldParsedTypeCallback;
      };

      FunctionState.ParsedTypeCallback = [&OpenedArchetypesTracker](Type ty) {
        OpenedArchetypesTracker.registerUsedOpenedArchetypes(
          ty->getCanonicalType());
      };

      do {
        if (FunctionState.parseSILBasicBlock(B))
          return true;
      } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));

      SourceLoc RBraceLoc;
      P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                           LBraceLoc);

      // Check that there are no unresolved forward definitions of opened
      // archetypes.
      if (OpenedArchetypesTracker.hasUnresolvedOpenedArchetypeDefinitions())
        llvm_unreachable(
            "All forward definitions of opened archetypes should be resolved");
    }

    FunctionState.F->setLinkage(resolveSILLinkage(FnLinkage, isDefinition));
  }

  if (FunctionState.diagnoseProblems())
    return true;

  // If SIL parsing succeeded, verify the generated SIL.
  if (!P.Diags.hadAnyError())
    FunctionState.F->verify();

  return false;
}

///   decl-sil-stage:   [[only in SIL mode]]
///     'sil_stage' ('raw' | 'canonical')
bool SILParserTUState::parseDeclSILStage(Parser &P) {
  SourceLoc stageLoc = P.consumeToken(tok::kw_sil_stage);
  if (!P.Tok.is(tok::identifier)) {
    P.diagnose(P.Tok, diag::expected_sil_stage_name);
    return true;
  }
  SILStage stage;
  if (P.Tok.isContextualKeyword("raw")) {
    stage = SILStage::Raw;
    P.consumeToken();
  } else if (P.Tok.isContextualKeyword("canonical")) {
    stage = SILStage::Canonical;
    P.consumeToken();
  } else if (P.Tok.isContextualKeyword("lowered")) {
    stage = SILStage::Lowered;
    P.consumeToken();
  } else {
    P.diagnose(P.Tok, diag::expected_sil_stage_name);
    P.consumeToken();
    return true;
  }
  
  if (DidParseSILStage) {
    P.diagnose(stageLoc, diag::multiple_sil_stage_decls);
    return false;
  }
  
  M.setStage(stage);
  DidParseSILStage = true;
  return false;
}

/// Lookup a global variable declaration from its demangled name.
///
/// A variable declaration exists for all sil_global variables defined in
/// Swift. A Swift global defined outside this module will be exposed
/// via an addressor rather than as a sil_global. Globals imported
/// from clang will produce a sil_global but will not have any corresponding
/// VarDecl.
///
/// FIXME: lookupGlobalDecl() can handle collisions between private or
/// fileprivate global variables in the same SIL Module, but the typechecker
/// will still incorrectly diagnose this as an "invalid redeclaration" and give
/// all but the first declaration an error type.
static Optional<VarDecl *> lookupGlobalDecl(Identifier GlobalName,
                                            SILLinkage GlobalLinkage,
                                            SILType GlobalType, Parser &P) {
  // Create a set of DemangleOptions to produce the global variable's
  // identifier, which is used as a search key in the declaration context.
  Demangle::DemangleOptions demangleOpts;
  demangleOpts.QualifyEntities = false;
  demangleOpts.ShowPrivateDiscriminators = false;
  demangleOpts.DisplayEntityTypes = false;
  std::string GlobalDeclName = Demangle::demangleSymbolAsString(
    GlobalName.str(), demangleOpts);

  SmallVector<ValueDecl *, 4> CurModuleResults;
  P.SF.getParentModule()->lookupValue(
      {}, P.Context.getIdentifier(GlobalDeclName), NLKind::UnqualifiedLookup,
      CurModuleResults);
  // Bail-out on clang-imported globals.
  if (CurModuleResults.empty())
    return nullptr;

  // private and fileprivate globals of the same name may be merged into a
  // single SIL module. Find the declaration with the correct type and
  // linkage. (If multiple globals have the same type and linkage then it
  // doesn't matter which declaration we use).
  for (ValueDecl *ValDecl : CurModuleResults) {
    auto *VD = cast<VarDecl>(ValDecl);
    CanType DeclTy = VD->getType()->getCanonicalType();
    if (DeclTy == GlobalType.getASTType()
        && getDeclSILLinkage(VD) == GlobalLinkage) {
      return VD;
    }
  }
  return None;
}

/// decl-sil-global: [[only in SIL mode]]
///   'sil_global' sil-linkage @name : sil-type [external]
bool SILParserTUState::parseSILGlobal(Parser &P) {
  // Inform the lexer that we're lexing the body of the SIL declaration.
  Lexer::SILBodyRAII Tmp(*P.L);

  P.consumeToken(tok::kw_sil_global);
  Optional<SILLinkage> GlobalLinkage;
  Identifier GlobalName;
  SILType GlobalType;
  SourceLoc NameLoc;
  IsSerialized_t isSerialized = IsNotSerialized;
  bool isLet = false;

  Scope S(&P, ScopeKind::TopLevel);
  SILParser State(P);
  if (parseSILLinkage(GlobalLinkage, P) ||
      parseDeclSILOptional(nullptr, &isSerialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           &isLet, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, State, M) ||
      P.parseToken(tok::at_sign, diag::expected_sil_value_name) ||
      P.parseIdentifier(GlobalName, NameLoc, diag::expected_sil_value_name) ||
      P.parseToken(tok::colon, diag::expected_sil_type))
    return true;

  if (State.parseSILType(GlobalType))
    return true;

  // Non-external global variables are definitions by default.
  if (!GlobalLinkage.hasValue())
    GlobalLinkage = SILLinkage::DefaultForDefinition;

  // Lookup the global variable declaration for this sil_global.
  auto VD =
      lookupGlobalDecl(GlobalName, GlobalLinkage.getValue(), GlobalType, P);
  if (!VD) {
    P.diagnose(NameLoc, diag::sil_global_variable_not_found, GlobalName);
    return true;
  }
  auto *GV = SILGlobalVariable::create(
      M, GlobalLinkage.getValue(), isSerialized, GlobalName.str(), GlobalType,
      RegularLocation(NameLoc), VD.getValue());

  GV->setLet(isLet);
  // Parse static initializer if exists.
  if (State.P.consumeIf(tok::equal) && State.P.consumeIf(tok::l_brace)) {
    SILBuilder B(GV);
    do {
      State.parseSILInstruction(B);
    } while (! State.P.consumeIf(tok::r_brace));
  }
  return false;
}

/// decl-sil-property: [[only in SIL mode]]
///   'sil_property' sil-decl-ref '(' sil-key-path-pattern-component ')'

bool SILParserTUState::parseSILProperty(Parser &P) {
  Lexer::SILBodyRAII Tmp(*P.L);

  auto loc = P.consumeToken(tok::kw_sil_property);
  auto InstLoc = RegularLocation(loc);
  SILParser SP(P);
  
  IsSerialized_t Serialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &Serialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, SP, M))
    return true;
  
  ValueDecl *VD;
  
  if (SP.parseSILDottedPath(VD))
    return true;
  
  GenericParamList *generics;
  GenericEnvironment *patternEnv;
  Scope toplevelScope(&P, ScopeKind::TopLevel);
  Scope genericsScope(&P, ScopeKind::Generics);
  generics = P.maybeParseGenericParams().getPtrOrNull();
  patternEnv = handleSILGenericParams(P.Context, generics, &P.SF);
  
  if (patternEnv) {
    if (patternEnv->getGenericSignature()->getCanonicalSignature()
           != VD->getInnermostDeclContext()->getGenericSignatureOfContext()
                ->getCanonicalSignature()) {
      P.diagnose(loc, diag::sil_property_generic_signature_mismatch);
      return true;
    }
  } else {
    if (VD->getInnermostDeclContext()->getGenericSignatureOfContext()) {
      P.diagnose(loc, diag::sil_property_generic_signature_mismatch);
      return true;
    }
  }

  Identifier ComponentKind;
  Optional<KeyPathPatternComponent> Component;
  SourceLoc ComponentLoc;
  SmallVector<SILType, 4> OperandTypes;

  if (P.parseToken(tok::l_paren, diag::expected_tok_in_sil_instr, "("))
    return true;
  
  if (!P.consumeIf(tok::r_paren)) {
    KeyPathPatternComponent parsedComponent;
    if (P.parseIdentifier(ComponentKind, ComponentLoc,
                          diag::expected_tok_in_sil_instr, "component kind")
        || SP.parseKeyPathPatternComponent(parsedComponent, OperandTypes,
                 ComponentLoc, ComponentKind, InstLoc,
                 patternEnv)
        || P.parseToken(tok::r_paren, diag::expected_tok_in_sil_instr, ")"))
      return true;
    
    Component = std::move(parsedComponent);
  }
  
  SILProperty::create(M, Serialized,
                      cast<AbstractStorageDecl>(VD), Component);
  return false;
}

/// decl-sil-vtable: [[only in SIL mode]]
///   'sil_vtable' ClassName decl-sil-vtable-body
/// decl-sil-vtable-body:
///   '{' sil-vtable-entry* '}'
/// sil-vtable-entry:
///   SILDeclRef ':' SILFunctionName
bool SILParserTUState::parseSILVTable(Parser &P) {
  P.consumeToken(tok::kw_sil_vtable);
  SILParser VTableState(P);

  IsSerialized_t Serialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &Serialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           VTableState, M))
    return true;

  // Parse the class name.
  Identifier Name;
  SourceLoc Loc;
  if (VTableState.parseSILIdentifier(Name, Loc,
                                     diag::expected_sil_value_name))
    return true;

  // Find the class decl.
  llvm::PointerUnion<ValueDecl*, ModuleDecl *> Res = lookupTopDecl(P, Name);
  assert(Res.is<ValueDecl*>() && "Class look-up should return a Decl");
  ValueDecl *VD = Res.get<ValueDecl*>();
  if (!VD) {
    P.diagnose(Loc, diag::sil_vtable_class_not_found, Name);
    return true;
  }

  auto *theClass = dyn_cast<ClassDecl>(VD);
  if (!theClass) {
    P.diagnose(Loc, diag::sil_vtable_class_not_found, Name);
    return true;
  }

  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII Tmp(*P.L);
  Scope S(&P, ScopeKind::TopLevel);
  // Parse the entry list.
  std::vector<SILVTable::Entry> vtableEntries;
  if (P.Tok.isNot(tok::r_brace)) {
    do {
      SILDeclRef Ref;
      Identifier FuncName;
      SourceLoc FuncLoc;
      if (VTableState.parseSILDeclRef(Ref, true))
        return true;
      SILFunction *Func = nullptr;
      if (P.Tok.is(tok::kw_nil)) {
        P.consumeToken();
      } else {
        if (P.parseToken(tok::colon, diag::expected_sil_vtable_colon) ||
            P.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
            VTableState.parseSILIdentifier(FuncName, FuncLoc,
                                           diag::expected_sil_value_name))
        return true;
        Func = M.lookUpFunction(FuncName.str());
        if (!Func) {
          P.diagnose(FuncLoc, diag::sil_vtable_func_not_found, FuncName);
          return true;
        }
      }

      auto Kind = SILVTable::Entry::Kind::Normal;
      if (P.Tok.is(tok::l_square)) {
        P.consumeToken(tok::l_square);
        if (P.Tok.isNot(tok::identifier)) {
          P.diagnose(P.Tok.getLoc(), diag::sil_vtable_bad_entry_kind);
          return true;
        }

        if (P.Tok.getText() == "override") {
          P.consumeToken();
          Kind = SILVTable::Entry::Kind::Override;
        } else if (P.Tok.getText() == "inherited") {
          P.consumeToken();
          Kind = SILVTable::Entry::Kind::Inherited;
        } else {
          P.diagnose(P.Tok.getLoc(), diag::sil_vtable_bad_entry_kind);
          return true;
        }

        if (P.parseToken(tok::r_square, diag::sil_vtable_expect_rsquare))
          return true;
      }

      vtableEntries.emplace_back(Ref, Func, Kind);
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);

  SILVTable::create(M, theClass, Serialized, vtableEntries);
  return false;
}

static ProtocolDecl *parseProtocolDecl(Parser &P, SILParser &SP) {
  Identifier DeclName;
  SourceLoc DeclLoc;
  if (SP.parseSILIdentifier(DeclName, DeclLoc, diag::expected_sil_value_name))
    return nullptr;

  // Find the protocol decl. The protocol can be imported.
  llvm::PointerUnion<ValueDecl*, ModuleDecl *> Res = lookupTopDecl(P, DeclName);
  assert(Res.is<ValueDecl*>() && "Protocol look-up should return a Decl");
  ValueDecl *VD = Res.get<ValueDecl*>();
  if (!VD) {
    P.diagnose(DeclLoc, diag::sil_witness_protocol_not_found, DeclName);
    return nullptr;
  }
  auto *proto = dyn_cast<ProtocolDecl>(VD);
  if (!proto)
    P.diagnose(DeclLoc, diag::sil_witness_protocol_not_found, DeclName);
  return proto;
}

static AssociatedTypeDecl *parseAssociatedTypeDecl(Parser &P, SILParser &SP,
                                                   ProtocolDecl *proto) {
  Identifier DeclName;
  SourceLoc DeclLoc;
  if (SP.parseSILIdentifier(DeclName, DeclLoc, diag::expected_sil_value_name))
    return nullptr;
  // We can return multiple decls, for now, we use the first lookup result.
  // One example is two decls when searching for Generator of Sequence:
  // one from Sequence, the other from _Sequence_Type.
  SmallVector<ValueDecl *, 4> values;
  auto VD = lookupMember(P, proto->getInterfaceType(), DeclName, DeclLoc,
                         values, true/*ExpectMultipleResults*/);
  if (!VD) {
    P.diagnose(DeclLoc, diag::sil_witness_assoc_not_found, DeclName);
    return nullptr;
  }
  return dyn_cast<AssociatedTypeDecl>(VD);
}

static bool parseAssociatedTypePath(SILParser &SP,
                                    SmallVectorImpl<Identifier> &path) {
  do {
    Identifier name;
    SourceLoc loc;
    if (SP.parseSILIdentifier(name, loc, diag::expected_sil_value_name))
      return false;
    path.push_back(name);
  } while (SP.P.consumeIf(tok::period));

  return true;
}

static bool matchesAssociatedTypePath(CanType assocType,
                                      ArrayRef<Identifier> path) {
  if (auto memberType = dyn_cast<DependentMemberType>(assocType)) {
    return (!path.empty() &&
            memberType->getName() == path.back() &&
            matchesAssociatedTypePath(memberType.getBase(), path.drop_back()));
  } else {
    assert(isa<GenericTypeParamType>(assocType));
    return path.empty();
  }
}

static CanType parseAssociatedTypePath(Parser &P, SILParser &SP,
                                       ProtocolDecl *proto) {
  SourceLoc loc = SP.P.Tok.getLoc();
  SmallVector<Identifier, 4> path;
  if (!parseAssociatedTypePath(SP, path))
    return CanType();

  // This is only used for parsing associated conformances, so we can
  // go ahead and just search the requirement signature for something that
  // matches the path.
  for (auto &reqt : proto->getRequirementSignature()) {
    if (reqt.getKind() != RequirementKind::Conformance)
      continue;
    CanType assocType = reqt.getFirstType()->getCanonicalType();
    if (matchesAssociatedTypePath(assocType, path))
      return assocType;
  }

  SmallString<128> name;
  name += path[0].str();
  for (auto elt : makeArrayRef(path).slice(1)) {
    name += '.';
    name += elt.str();
  }
  P.diagnose(loc, diag::sil_witness_assoc_conf_not_found, name);
  return CanType();
}

static bool isSelfConformance(Type conformingType, ProtocolDecl *protocol) {
  if (auto protoTy = conformingType->getAs<ProtocolType>())
    return protoTy->getDecl() == protocol;
  return false;
}

static Optional<ProtocolConformanceRef> parseRootProtocolConformance(Parser &P,
           SILParser &SP, Type ConformingTy, ProtocolDecl *&proto,
           ConformanceContext context) {
  Identifier ModuleKeyword, ModuleName;
  SourceLoc Loc, KeywordLoc;
  proto = parseProtocolDecl(P, SP);
  if (!proto)
    return None;
      
  if (P.parseIdentifier(ModuleKeyword, KeywordLoc,
                        diag::expected_tok_in_sil_instr, "module") ||
      SP.parseSILIdentifier(ModuleName, Loc,
                            diag::expected_sil_value_name))
    return None;

  if (ModuleKeyword.str() != "module") {
    P.diagnose(KeywordLoc, diag::expected_tok_in_sil_instr, "module");
    return None;
  }

  // Calling lookupConformance on a BoundGenericType will return a specialized
  // conformance. We use UnboundGenericType to find the normal conformance.
  Type lookupTy = ConformingTy;
  if (auto bound = lookupTy->getAs<BoundGenericType>())
    lookupTy = bound->getDecl()->getDeclaredType();
  auto lookup = P.SF.getParentModule()->lookupConformance(lookupTy, proto);
  if (!lookup) {
    P.diagnose(KeywordLoc, diag::sil_witness_protocol_conformance_not_found);
    return None;
  }

  // Use a concrete self-conformance if we're parsing this for a witness table.
  if (context == ConformanceContext::WitnessTable &&
      !lookup->isConcrete() &&
      isSelfConformance(ConformingTy, proto)) {
    lookup = ProtocolConformanceRef(P.Context.getSelfConformance(proto));
  }

  return lookup;
}

///  protocol-conformance ::= normal-protocol-conformance
///  protocol-conformance ::=
///    generic-parameter-list? type: 'inherit' '(' protocol-conformance ')'
///  protocol-conformance ::=
///    generic-parameter-list? type: 'specialize' '<' substitution* '>'
///    '(' protocol-conformance ')'
///  normal-protocol-conformance ::=
///    generic-parameter-list? type: protocolName module ModuleName
/// Note that generic-parameter-list is already parsed before calling this.
Optional<ProtocolConformanceRef> SILParser::parseProtocolConformance(
           ProtocolDecl *&proto,
           GenericEnvironment *&genericEnv,
           ConformanceContext context,
           ProtocolDecl *defaultForProto) {
  // Parse generic params for the protocol conformance. We need to make sure
  // they have the right scope.
  Optional<Scope> GenericsScope;
  if (context == ConformanceContext::Ordinary)
    GenericsScope.emplace(&P, ScopeKind::Generics);

  // Make sure we don't leave it uninitialized in the caller
  genericEnv = nullptr;

  auto *genericParams = P.maybeParseGenericParams().getPtrOrNull();
  if (genericParams) {
    genericEnv = handleSILGenericParams(P.Context, genericParams, &P.SF);
  }

  auto retVal = parseProtocolConformanceHelper(proto, genericEnv, context,
                                               defaultForProto);

  if (GenericsScope) {
    GenericsScope.reset();
  }
  return retVal;
}

Optional<ProtocolConformanceRef> SILParser::parseProtocolConformanceHelper(
                                    ProtocolDecl *&proto,
                                    GenericEnvironment *witnessEnv,
                                    ConformanceContext context,
                                    ProtocolDecl *defaultForProto) {
  // Parse AST type.
  ParserResult<TypeRepr> TyR = P.parseType();
  if (TyR.isNull())
    return None;
  TypeLoc Ty = TyR.get();
  if (defaultForProto) {
    bindProtocolSelfInTypeRepr(Ty, defaultForProto);
  }

  if (performTypeLocChecking(Ty, /*IsSILType=*/ false, witnessEnv,
                             defaultForProto))
    return None;
  auto ConformingTy = Ty.getType();

  if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
    return None;

  if (P.Tok.is(tok::identifier) && P.Tok.getText() == "specialize") {
    P.consumeToken();

    // Parse substitutions for specialized conformance.
    SmallVector<ParsedSubstitution, 4> parsedSubs;
    if (parseSubstitutions(parsedSubs, witnessEnv, defaultForProto))
      return None;

    if (P.parseToken(tok::l_paren, diag::expected_sil_witness_lparen))
      return None;
    ProtocolDecl *dummy;
    GenericEnvironment *specializedEnv;
    auto genericConform =
        parseProtocolConformance(dummy, specializedEnv,
                                 ConformanceContext::Ordinary,
                                 defaultForProto);
    if (!genericConform || !genericConform->isConcrete())
      return None;
    if (P.parseToken(tok::r_paren, diag::expected_sil_witness_rparen))
      return None;

    SubstitutionMap subMap =
      getApplySubstitutionsFromParsed(*this, specializedEnv, parsedSubs);
    if (!subMap)
      return None;

    auto result = P.Context.getSpecializedConformance(
      ConformingTy, genericConform->getConcrete(), subMap);
    return ProtocolConformanceRef(result);
  }

  if (P.Tok.is(tok::identifier) && P.Tok.getText() == "inherit") {
    P.consumeToken();

    if (P.parseToken(tok::l_paren, diag::expected_sil_witness_lparen))
      return None;
    auto baseConform = parseProtocolConformance(defaultForProto,
                                                ConformanceContext::Ordinary);
    if (!baseConform || !baseConform->isConcrete())
      return None;
    if (P.parseToken(tok::r_paren, diag::expected_sil_witness_rparen))
      return None;

    auto result = P.Context.getInheritedConformance(ConformingTy,
                                                    baseConform->getConcrete());
    return ProtocolConformanceRef(result);
  }

  auto retVal =
    parseRootProtocolConformance(P, *this, ConformingTy, proto, context);
  return retVal;
}

/// Parser a single SIL vtable entry and add it to either \p witnessEntries
/// or \c conditionalConformances.
static bool parseSILVTableEntry(
         Parser &P,
         SILModule &M,
         ProtocolDecl *proto,
         GenericEnvironment *witnessEnv,
         SILParser &witnessState,
         bool isDefaultWitnessTable,
         std::vector<SILWitnessTable::Entry> &witnessEntries,
         std::vector<SILWitnessTable::ConditionalConformance>
           &conditionalConformances) {
  ProtocolDecl *defaultForProto = isDefaultWitnessTable ? proto : nullptr;
  Identifier EntryKeyword;
  SourceLoc KeywordLoc;
  if (P.parseIdentifier(EntryKeyword, KeywordLoc,
        diag::expected_tok_in_sil_instr,
        "method, associated_type, associated_type_protocol, base_protocol"
        ", no_default"))
    return true;

  if (EntryKeyword.str() == "no_default") {
    witnessEntries.push_back(SILDefaultWitnessTable::Entry());
    return false;
  }

  if (EntryKeyword.str() == "base_protocol") {
    ProtocolDecl *proto = parseProtocolDecl(P, witnessState);
    if (!proto)
      return true;
    if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;
    auto conform =
      witnessState.parseProtocolConformance(defaultForProto,
                                            ConformanceContext::Ordinary);
    if (!conform || !conform->isConcrete()) // Ignore this witness entry for now.
      return false;

    witnessEntries.push_back(SILWitnessTable::BaseProtocolWitness{
      proto, conform->getConcrete()
    });
    return false;
  }

  if (EntryKeyword.str() == "associated_type_protocol" ||
      EntryKeyword.str() == "conditional_conformance") {
    if (P.parseToken(tok::l_paren, diag::expected_sil_witness_lparen))
      return true;
    CanType assocOrSubject;
    if (EntryKeyword.str() == "associated_type_protocol") {
      assocOrSubject = parseAssociatedTypePath(P, witnessState, proto);
    } else {
      // Parse AST type.
      ParserResult<TypeRepr> TyR = P.parseType();
      if (TyR.isNull())
        return true;
      TypeLoc Ty = TyR.get();
      if (isDefaultWitnessTable)
        bindProtocolSelfInTypeRepr(Ty, proto);
      if (swift::performTypeLocChecking(P.Context, Ty,
                                        /*isSILMode=*/false,
                                        /*isSILType=*/false,
                                        witnessEnv,
                                        &P.SF))
        return true;

      assocOrSubject = Ty.getType()->getCanonicalType();
    }
    if (!assocOrSubject)
      return true;
    if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;
    ProtocolDecl *proto = parseProtocolDecl(P, witnessState);
    if (!proto)
      return true;
    if (P.parseToken(tok::r_paren, diag::expected_sil_witness_rparen) ||
        P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;

    ProtocolConformanceRef conformance(proto);
    if (P.Tok.getText() != "dependent") {
      auto concrete =
        witnessState.parseProtocolConformance(defaultForProto,
                                              ConformanceContext::Ordinary);
      if (!concrete && !concrete->isConcrete()) // Ignore this for now.
        return false;
      conformance = *concrete;
    } else {
      P.consumeToken();
    }

    if (EntryKeyword.str() == "associated_type_protocol")
      witnessEntries.push_back(
          SILWitnessTable::AssociatedTypeProtocolWitness{assocOrSubject,
                                                         proto,
                                                         conformance});
    else
      conditionalConformances.push_back(
          SILWitnessTable::ConditionalConformance{assocOrSubject,
                                                  conformance});

    return false;
  }

  if (EntryKeyword.str() == "associated_type") {
    AssociatedTypeDecl *assoc = parseAssociatedTypeDecl(P, witnessState,
                                                        proto);
    if (!assoc)
      return true;
    if (P.parseToken(tok::colon, diag::expected_sil_witness_colon))
      return true;

    // Parse AST type.
    ParserResult<TypeRepr> TyR = P.parseType();
    if (TyR.isNull())
      return true;
    TypeLoc Ty = TyR.get();
    if (isDefaultWitnessTable)
      bindProtocolSelfInTypeRepr(Ty, proto);
    if (swift::performTypeLocChecking(P.Context, Ty,
                                      /*isSILMode=*/false,
                                      /*isSILType=*/false,
                                      witnessEnv,
                                      &P.SF))
      return true;

    witnessEntries.push_back(SILWitnessTable::AssociatedTypeWitness{
      assoc, Ty.getType()->getCanonicalType()
    });
    return false;
  }

  if (EntryKeyword.str() != "method") {
    P.diagnose(KeywordLoc, diag::expected_tok_in_sil_instr, "method");
    return true;
  }

  SILDeclRef Ref;
  Identifier FuncName;
  SourceLoc FuncLoc;
  if (witnessState.parseSILDeclRef(Ref, true) ||
      P.parseToken(tok::colon, diag::expected_sil_witness_colon))
    return true;

  SILFunction *Func = nullptr;
  if (P.Tok.is(tok::kw_nil)) {
    P.consumeToken();
  } else {
    if (P.parseToken(tok::at_sign, diag::expected_sil_function_name) ||
        witnessState.parseSILIdentifier(FuncName, FuncLoc,
                                        diag::expected_sil_value_name))
      return true;

    Func = M.lookUpFunction(FuncName.str());
    if (!Func) {
      P.diagnose(FuncLoc, diag::sil_witness_func_not_found, FuncName);
      return true;
    }
  }
  witnessEntries.push_back(SILWitnessTable::MethodWitness{
    Ref, Func
  });

  return false;
}

/// decl-sil-witness ::= 'sil_witness_table' sil-linkage?
///                      normal-protocol-conformance decl-sil-witness-body
/// normal-protocol-conformance ::=
///   generic-parameter-list? type: protocolName module ModuleName
/// decl-sil-witness-body:
///   '{' sil-witness-entry* '}'
/// sil-witness-entry:
///   method SILDeclRef ':' @SILFunctionName
///   associated_type AssociatedTypeDeclName: Type
///   associated_type_protocol (AssocName: ProtocolName):
///                              protocol-conformance|dependent
///   base_protocol ProtocolName: protocol-conformance
bool SILParserTUState::parseSILWitnessTable(Parser &P) {
  P.consumeToken(tok::kw_sil_witness_table);
  SILParser WitnessState(P);
  
  // Parse the linkage.
  Optional<SILLinkage> Linkage;
  parseSILLinkage(Linkage, P);
  
  IsSerialized_t isSerialized = IsNotSerialized;
  if (parseDeclSILOptional(nullptr, &isSerialized, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           WitnessState, M))
    return true;

  Scope S(&P, ScopeKind::TopLevel);
  // We should use WitnessTableBody. This ensures that the generic params
  // are visible.
  Optional<Scope> BodyScope;
  BodyScope.emplace(&P, ScopeKind::FunctionBody);

  // Parse the protocol conformance.
  ProtocolDecl *proto;
  GenericEnvironment *witnessEnv;
  auto conf = WitnessState.parseProtocolConformance(proto,
                                                    witnessEnv,
                                              ConformanceContext::WitnessTable,
                                                    nullptr);
  WitnessState.ContextGenericEnv = witnessEnv;

  // FIXME: should we really allow a specialized or inherited conformance here?
  auto theConformance =
    (conf && conf->isConcrete())
      ? conf->getConcrete()->getRootConformance()
      : nullptr;

  SILWitnessTable *wt = nullptr;
  if (theConformance) {
    wt = M.lookUpWitnessTable(theConformance, false);
    assert((!wt || wt->isDeclaration()) &&
           "Attempting to create duplicate witness table.");
  }

  // If we don't have an lbrace, then this witness table is a declaration.
  if (P.Tok.getKind() != tok::l_brace) {
    // Default to public external linkage.
    if (!Linkage)
      Linkage = SILLinkage::PublicExternal;
    // We ignore empty witness table without normal protocol conformance.
    if (!wt && theConformance)
      wt = SILWitnessTable::create(M, *Linkage, theConformance);
    BodyScope.reset();
    return false;
  }

  if (!theConformance) {
    P.diagnose(P.Tok, diag::sil_witness_protocol_conformance_not_found);
    return true;
  }

  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII Tmp(*P.L);
  // Parse the entry list.
  std::vector<SILWitnessTable::Entry> witnessEntries;
  std::vector<SILWitnessTable::ConditionalConformance> conditionalConformances;

  if (P.Tok.isNot(tok::r_brace)) {
    do {
      if (parseSILVTableEntry(P, M, proto, witnessEnv, WitnessState, false,
                              witnessEntries, conditionalConformances))
        return true;
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);
  
  // Default to public linkage.
  if (!Linkage)
    Linkage = SILLinkage::Public;

  if (!wt)
    wt = SILWitnessTable::create(M, *Linkage, theConformance);
  else
    wt->setLinkage(*Linkage);
  wt->convertToDefinition(witnessEntries, conditionalConformances,
                          isSerialized);
  BodyScope.reset();
  return false;
}

/// decl-sil-default-witness ::= 'sil_default_witness_table' 
///                              sil-linkage identifier
///                              decl-sil-default-witness-body
/// decl-sil-default-witness-body:
///   '{' sil-default-witness-entry* '}'
/// sil-default-witness-entry:
///   sil-witness-entry
///   'no_default'
bool SILParserTUState::parseSILDefaultWitnessTable(Parser &P) {
  P.consumeToken(tok::kw_sil_default_witness_table);
  SILParser WitnessState(P);
  
  // Parse the linkage.
  Optional<SILLinkage> Linkage;
  parseSILLinkage(Linkage, P);
  
  Scope S(&P, ScopeKind::TopLevel);
  // We should use WitnessTableBody. This ensures that the generic params
  // are visible.
  Optional<Scope> BodyScope;
  BodyScope.emplace(&P, ScopeKind::FunctionBody);

  // Parse the protocol.
  ProtocolDecl *protocol = parseProtocolDecl(P, WitnessState);
  if (!protocol)
    return true;

  // Parse the body.
  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  // We need to turn on InSILBody to parse SILDeclRef.
  Lexer::SILBodyRAII Tmp(*P.L);

  // Parse the entry list.
  std::vector<SILWitnessTable::Entry> witnessEntries;
  std::vector<SILWitnessTable::ConditionalConformance> conditionalConformances;

  if (P.Tok.isNot(tok::r_brace)) {
    do {
      if (parseSILVTableEntry(P, M, protocol, protocol->getGenericEnvironment(),
                              WitnessState, true, witnessEntries,
                              conditionalConformances))
        return true;
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);
  
  // Default to public linkage.
  if (!Linkage)
    Linkage = SILLinkage::Public;

  SILDefaultWitnessTable::create(M, *Linkage, protocol, witnessEntries);
  BodyScope.reset();
  return false;
}

llvm::Optional<llvm::coverage::Counter> SILParser::parseSILCoverageExpr(
    llvm::coverage::CounterExpressionBuilder &Builder) {
  if (P.Tok.is(tok::integer_literal)) {
    unsigned CounterId;
    if (parseInteger(CounterId, diag::sil_coverage_invalid_counter))
      return None;
    return llvm::coverage::Counter::getCounter(CounterId);
  }

  if (P.Tok.is(tok::identifier)) {
    Identifier Zero;
    SourceLoc Loc;
    if (parseSILIdentifier(Zero, Loc, diag::sil_coverage_invalid_counter))
      return None;
    if (Zero.str() != "zero") {
      P.diagnose(Loc, diag::sil_coverage_invalid_counter);
      return None;
    }
    return llvm::coverage::Counter::getZero();
  }

  if (P.Tok.is(tok::l_paren)) {
    P.consumeToken(tok::l_paren);
    auto LHS = parseSILCoverageExpr(Builder);
    if (!LHS)
      return None;
    Identifier Operator;
    SourceLoc Loc;
    if (P.parseAnyIdentifier(Operator, Loc,
                             diag::sil_coverage_invalid_operator))
      return None;
    if (Operator.str() != "+" && Operator.str() != "-") {
      P.diagnose(Loc, diag::sil_coverage_invalid_operator);
      return None;
    }
    auto RHS = parseSILCoverageExpr(Builder);
    if (!RHS)
      return None;
    if (P.parseToken(tok::r_paren, diag::sil_coverage_expected_rparen))
      return None;

    if (Operator.str() == "+")
      return Builder.add(*LHS, *RHS);
    return Builder.subtract(*LHS, *RHS);
  }

  P.diagnose(P.Tok, diag::sil_coverage_invalid_counter);
  return None;
}

/// decl-sil-coverage-map ::= 'sil_coverage_map' CoveredName PGOFuncName CoverageHash
///                           decl-sil-coverage-body
/// decl-sil-coverage-body:
///   '{' sil-coverage-entry* '}'
/// sil-coverage-entry:
///   sil-coverage-loc ':' sil-coverage-expr
/// sil-coverage-loc:
///   StartLine ':' StartCol '->' EndLine ':' EndCol
/// sil-coverage-expr:
///   ...
bool SILParserTUState::parseSILCoverageMap(Parser &P) {
  P.consumeToken(tok::kw_sil_coverage_map);
  SILParser State(P);

  // Parse the filename.
  Identifier Filename;
  SourceLoc FileLoc;
  if (State.parseSILIdentifier(Filename, FileLoc,
                               diag::expected_sil_value_name))
    return true;

  // Parse the covered name.
  if (!P.Tok.is(tok::string_literal)) {
    P.diagnose(P.Tok, diag::sil_coverage_expected_quote);
    return true;
  }
  StringRef FuncName = P.Tok.getText().drop_front().drop_back();
  P.consumeToken();

  // Parse the PGO func name.
  if (!P.Tok.is(tok::string_literal)) {
    P.diagnose(P.Tok, diag::sil_coverage_expected_quote);
    return true;
  }
  StringRef PGOFuncName = P.Tok.getText().drop_front().drop_back();
  P.consumeToken();

  uint64_t Hash;
  if (State.parseInteger(Hash, diag::sil_coverage_invalid_hash))
    return true;

  if (!P.Tok.is(tok::l_brace)) {
    P.diagnose(P.Tok, diag::sil_coverage_expected_lbrace);
    return true;
  }
  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  llvm::coverage::CounterExpressionBuilder Builder;
  std::vector<SILCoverageMap::MappedRegion> Regions;
  bool BodyHasError = false;
  if (P.Tok.isNot(tok::r_brace)) {
    do {
      unsigned StartLine, StartCol, EndLine, EndCol;
      if (State.parseInteger(StartLine, diag::sil_coverage_expected_loc) ||
          P.parseToken(tok::colon, diag::sil_coverage_expected_loc) ||
          State.parseInteger(StartCol, diag::sil_coverage_expected_loc) ||
          P.parseToken(tok::arrow, diag::sil_coverage_expected_arrow) ||
          State.parseInteger(EndLine, diag::sil_coverage_expected_loc) ||
          P.parseToken(tok::colon, diag::sil_coverage_expected_loc) ||
          State.parseInteger(EndCol, diag::sil_coverage_expected_loc)) {
        BodyHasError = true;
        break;
      }

      if (P.parseToken(tok::colon, diag::sil_coverage_expected_colon)) {
        BodyHasError = true;
        break;
      }

      auto Counter = State.parseSILCoverageExpr(Builder);
      if (!Counter) {
        BodyHasError = true;
        break;
      }

      Regions.emplace_back(StartLine, StartCol, EndLine, EndCol, *Counter);
    } while (P.Tok.isNot(tok::r_brace) && P.Tok.isNot(tok::eof));
  }
  if (BodyHasError)
    P.skipUntilDeclRBrace();

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);

  if (!BodyHasError)
    SILCoverageMap::create(M, Filename.str(), FuncName.str(), PGOFuncName.str(),
                           Hash, Regions, Builder.getExpressions());
  return false;
}

/// sil-scope-ref ::= 'scope' [0-9]+
/// sil-scope ::= 'sil_scope' [0-9]+ '{'
///                 debug-loc
///                 'parent' scope-parent
///                 ('inlined_at' sil-scope-ref)?
///               '}'
/// scope-parent ::= sil-function-name ':' sil-type
/// scope-parent ::= sil-scope-ref
/// debug-loc ::= 'loc' string-literal ':' [0-9]+ ':' [0-9]+
bool SILParserTUState::parseSILScope(Parser &P) {
  P.consumeToken(tok::kw_sil_scope);
  SILParser ScopeState(P);

  SourceLoc SlotLoc = P.Tok.getLoc();
  unsigned Slot;
  if (ScopeState.parseInteger(Slot, diag::sil_invalid_scope_slot))
    return true;

  SourceLoc LBraceLoc = P.Tok.getLoc();
  P.consumeToken(tok::l_brace);

  StringRef Key = P.Tok.getText();
  RegularLocation Loc{SILLocation::DebugLoc()};
  if (Key == "loc")
    if (ScopeState.parseSILLocation(Loc))
      return true;
  ScopeState.parseVerbatim("parent");
  Identifier FnName;
  SILDebugScope *Parent = nullptr;
  SILFunction *ParentFn = nullptr;
  if (P.Tok.is(tok::integer_literal)) {
    /// scope-parent ::= sil-scope-ref
    if (ScopeState.parseScopeRef(Parent))
      return true;
  } else {
    /// scope-parent ::= sil-function-name
    SILType Ty;
    SourceLoc FnLoc = P.Tok.getLoc();
    // We need to turn on InSILBody to parse the function reference.
    Lexer::SILBodyRAII Tmp(*P.L);
    GenericEnvironment *IgnoredEnv;
    Scope S(&P, ScopeKind::TopLevel);
    Scope Body(&P, ScopeKind::FunctionBody);
    if ((ScopeState.parseGlobalName(FnName)) ||
        P.parseToken(tok::colon, diag::expected_sil_colon_value_ref) ||
        ScopeState.parseSILType(Ty, IgnoredEnv, true))
      return true;

    // The function doesn't exist yet. Create a zombie forward declaration.
    auto FnTy = Ty.getAs<SILFunctionType>();
    if (!FnTy || !Ty.isObject()) {
      P.diagnose(FnLoc, diag::expected_sil_function_type);
      return true;
    }
    ParentFn = ScopeState.getGlobalNameForReference(FnName, FnTy, FnLoc, true);
    ScopeState.TUState.PotentialZombieFns.insert(ParentFn);
  }

  SILDebugScope *InlinedAt = nullptr;
  if (P.Tok.getText() == "inlined_at") {
    P.consumeToken();
    if (ScopeState.parseScopeRef(InlinedAt))
      return true;
  }

  SourceLoc RBraceLoc;
  P.parseMatchingToken(tok::r_brace, RBraceLoc, diag::expected_sil_rbrace,
                       LBraceLoc);

  auto &Scope = ScopeSlots[Slot];
  if (Scope) {
    P.diagnose(SlotLoc, diag::sil_scope_redefined, Slot);
    return true;
  }

  Scope = new (M) SILDebugScope(Loc, ParentFn, Parent, InlinedAt);
  return false;
}
