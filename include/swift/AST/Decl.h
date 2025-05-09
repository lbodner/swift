//===--- Decl.h - Swift Language Declaration ASTs ---------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the Decl class and subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DECL_H
#define SWIFT_DECL_H

#include "swift/AST/AccessScope.h"
#include "swift/AST/Attr.h"
#include "swift/AST/CaptureInfo.h"
#include "swift/AST/ClangNode.h"
#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DefaultArgumentKind.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/GenericParamKey.h"
#include "swift/AST/IfConfigClause.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/StorageImpl.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/AST/TypeWalker.h"
#include "swift/AST/Types.h"
#include "swift/AST/Witness.h"
#include "swift/Basic/ArrayRefView.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/OptionalEnum.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/TrailingObjects.h"
#include <type_traits>

namespace swift {
  enum class AccessSemantics : unsigned char;
  class AccessorDecl;
  class ApplyExpr;
  class AvailabilityContext;
  class GenericEnvironment;
  class ArchetypeType;
  class ASTContext;
  struct ASTNode;
  class ASTPrinter;
  class ASTWalker;
  class ConstructorDecl;
  class DestructorDecl;
  class DiagnosticEngine;
  class DynamicSelfType;
  class Type;
  class Expr;
  class DeclRefExpr;
  class ForeignErrorConvention;
  class LiteralExpr;
  class BraceStmt;
  class DeclAttributes;
  class GenericContext;
  class GenericSignature;
  class GenericTypeParamDecl;
  class GenericTypeParamType;
  class ModuleDecl;
  class EnumCaseDecl;
  class EnumElementDecl;
  class ParameterList;
  class ParameterTypeFlags;
  class Pattern;
  struct PrintOptions;
  struct PropertyWrapperBackingPropertyInfo;
  struct PropertyWrapperTypeInfo;
  struct PropertyWrapperMutability;
  class ProtocolDecl;
  class ProtocolType;
  struct RawComment;
  enum class ResilienceExpansion : unsigned;
  class TypeAliasDecl;
  class Stmt;
  class SubscriptDecl;
  class UnboundGenericType;
  class ValueDecl;
  class VarDecl;
  class OpaqueReturnTypeRepr;

enum class DeclKind : uint8_t {
#define DECL(Id, Parent) Id,
#define LAST_DECL(Id) Last_Decl = Id,
#define DECL_RANGE(Id, FirstId, LastId) \
  First_##Id##Decl = FirstId, Last_##Id##Decl = LastId,
#include "swift/AST/DeclNodes.def"
};
enum : unsigned { NumDeclKindBits =
  countBitsUsed(static_cast<unsigned>(DeclKind::Last_Decl)) };


/// Fine-grained declaration kind that provides a description of the
/// kind of entity a declaration represents, as it would be used in
/// diagnostics.
///
/// For example, \c FuncDecl is a single declaration class, but it has
/// several descriptive entries depending on whether it is an
/// operator, global function, local function, method, (observing)
/// accessor, etc.
enum class DescriptiveDeclKind : uint8_t {
  Import,
  Extension,
  EnumCase,
  TopLevelCode,
  IfConfig,
  PoundDiagnostic,
  PatternBinding,
  Var,
  Param,
  Let,
  Property,
  StaticProperty,
  ClassProperty,
  InfixOperator,
  PrefixOperator,
  PostfixOperator,
  PrecedenceGroup,
  TypeAlias,
  GenericTypeParam,
  AssociatedType,
  Type,
  Enum,
  Struct,
  Class,
  Protocol,
  GenericEnum,
  GenericStruct,
  GenericClass,
  GenericType,
  Subscript,
  StaticSubscript,
  ClassSubscript,
  Constructor,
  Destructor,
  LocalFunction,
  GlobalFunction,
  OperatorFunction,
  Method,
  StaticMethod,
  ClassMethod,
  Getter,
  Setter,
  Addressor,
  MutableAddressor,
  ReadAccessor,
  ModifyAccessor,
  WillSet,
  DidSet,
  EnumElement,
  Module,
  MissingMember,
  Requirement,
  OpaqueResultType,
  OpaqueVarType
};

/// Keeps track of stage of circularity checking for the given protocol.
enum class CircularityCheck {
  /// Circularity has not yet been checked.
  Unchecked,
  /// We're currently checking circularity.
  Checking,
  /// Circularity has already been checked.
  Checked
};

/// Describes which spelling was used in the source for the 'static' or 'class'
/// keyword.
enum class StaticSpellingKind : uint8_t {
  None,
  KeywordStatic,
  KeywordClass,
};

/// Keeps track of whether an enum has cases that have associated values.
enum class AssociatedValueCheck {
  /// We have not yet checked.
  Unchecked,
  /// The enum contains no cases or all cases contain no associated values.
  NoAssociatedValues,
  /// The enum contains at least one case with associated values.
  HasAssociatedValues,
};

/// Diagnostic printing of \c StaticSpellingKind.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, StaticSpellingKind SSK);

/// Encapsulation of the overload signature of a given declaration,
/// which is used to determine uniqueness of a declaration within a
/// given context.
///
/// Two definitions in the same context may not have the same overload
/// signature.
struct OverloadSignature {
  /// The full name of the declaration.
  DeclName Name;

  /// The kind of unary operator.
  UnaryOperatorKind UnaryOperator;

  /// Whether this is an instance member.
  unsigned IsInstanceMember : 1;

  /// Whether this is a variable.
  unsigned IsVariable : 1;

  /// Whether this is a function.
  unsigned IsFunction : 1;

  /// Whether this is a enum element.
  unsigned IsEnumElement : 1;

  /// Whether this is a nominal type.
  unsigned IsNominal : 1;

  /// Whether this is a type alias.
  unsigned IsTypeAlias : 1;

  /// Whether this signature is part of a protocol extension.
  unsigned InProtocolExtension : 1;

  /// Whether this signature is of a member defined in an extension of a generic
  /// type.
  unsigned InExtensionOfGenericType : 1;
  
  /// Whether this declaration has an opaque return type.
  unsigned HasOpaqueReturnType : 1;

  OverloadSignature()
      : UnaryOperator(UnaryOperatorKind::None), IsInstanceMember(false),
        IsVariable(false), IsFunction(false), InProtocolExtension(false),
        InExtensionOfGenericType(false), HasOpaqueReturnType(false) {}
};

/// Determine whether two overload signatures conflict.
///
/// \param sig1 The overload signature of the first declaration.
/// \param sig2 The overload signature of the second declaration.
/// \param skipProtocolExtensionCheck If \c true, members of protocol extensions
///        will be allowed to conflict with members of protocol declarations.
bool conflicting(const OverloadSignature& sig1, const OverloadSignature& sig2,
                 bool skipProtocolExtensionCheck = false);

/// Determine whether two overload signatures and overload types conflict.
///
/// \param ctx The AST context.
/// \param sig1 The overload signature of the first declaration.
/// \param sig1Type The overload type of the first declaration.
/// \param sig2 The overload signature of the second declaration.
/// \param sig2Type The overload type of the second declaration.
/// \param wouldConflictInSwift5 If non-null, the referenced boolean will be set
///        to \c true iff the function returns \c false for this version of
///        Swift, but the given overloads will conflict in Swift 5 mode.
/// \param skipProtocolExtensionCheck If \c true, members of protocol extensions
///        will be allowed to conflict with members of protocol declarations.
bool conflicting(ASTContext &ctx,
                 const OverloadSignature& sig1, CanType sig1Type,
                 const OverloadSignature& sig2, CanType sig2Type,
                 bool *wouldConflictInSwift5 = nullptr,
                 bool skipProtocolExtensionCheck = false);

/// Decl - Base class for all declarations in Swift.
class alignas(1 << DeclAlignInBits) Decl {
public:
  enum class ValidationState {
    Unchecked,
    Checking,
    CheckingWithValidSignature,
    Checked,
  };

protected:
  union { uint64_t OpaqueBits;

  SWIFT_INLINE_BITFIELD_BASE(Decl, bitmax(NumDeclKindBits,8)+1+1+1+1+1+1+1,
    Kind : bitmax(NumDeclKindBits,8),

    /// Whether this declaration is invalid.
    Invalid : 1,

    /// Whether this declaration was implicitly created, e.g.,
    /// an implicit constructor in a struct.
    Implicit : 1,

    /// Whether this declaration was mapped directly from a Clang AST.
    ///
    /// Use getClangNode() to retrieve the corresponding Clang AST.
    FromClang : 1,

    /// Whether we've already performed early attribute validation.
    /// FIXME: This is ugly.
    EarlyAttrValidation : 1,

    /// The validation state of this declaration.
    ValidationState : 2,

    /// Whether this declaration was added to the surrounding
    /// DeclContext of an active #if config clause.
    EscapedFromIfConfig : 1
  );

  SWIFT_INLINE_BITFIELD_FULL(PatternBindingDecl, Decl, 1+2+16,
    /// Whether this pattern binding declares static variables.
    IsStatic : 1,

    /// Whether 'static' or 'class' was used.
    StaticSpelling : 2,

    : NumPadBits,

    /// The number of pattern binding declarations.
    NumPatternEntries : 16
  );
  
  SWIFT_INLINE_BITFIELD_FULL(EnumCaseDecl, Decl, 32,
    : NumPadBits,

    /// The number of tail-allocated element pointers.
    NumElements : 32
  );

  SWIFT_INLINE_BITFIELD(ValueDecl, Decl, 1+1+1,
    AlreadyInLookupTable : 1,

    /// Whether we have already checked whether this declaration is a 
    /// redeclaration.
    CheckedRedeclaration : 1,

    /// Whether the decl can be accessed by swift users; for instance,
    /// a.storage for lazy var a is a decl that cannot be accessed.
    IsUserAccessible : 1
  );

  SWIFT_INLINE_BITFIELD(AbstractStorageDecl, ValueDecl, 1+1+1,
    /// Whether a keypath component can directly reference this storage,
    /// or if it must use the overridden declaration instead.
    HasComputedValidKeyPathComponent : 1,
    ValidKeyPathComponent : 1,
    
    /// Whether this property is a type property (currently unfortunately
    /// called 'static').
    IsStatic : 1
  );

  SWIFT_INLINE_BITFIELD(VarDecl, AbstractStorageDecl, 1+1+1+1+1+1,
    /// Encodes whether this is a 'let' binding.
    Introducer : 1,

    /// Whether this declaration was an element of a capture list.
    IsCaptureList : 1,

    /// Whether this vardecl has an initial value bound to it in a way
    /// that isn't represented in the AST with an initializer in the pattern
    /// binding.  This happens in cases like "for i in ...", switch cases, etc.
    HasNonPatternBindingInit : 1,

    /// Whether this is a property used in expressions in the debugger.
    /// It is up to the debugger to instruct SIL how to access this variable.
    IsDebuggerVar : 1,

    /// Whether this is the backing storage for a lazy property.
    IsLazyStorageProperty : 1,

    /// Whether this is the backing storage for a property wrapper.
    IsPropertyWrapperBackingProperty : 1
  );

  SWIFT_INLINE_BITFIELD(ParamDecl, VarDecl, 2+1+NumDefaultArgumentKindBits,
    /// The specifier associated with this parameter.  This determines
    /// the storage semantics of the value e.g. mutability.
    Specifier : 2,

    /// True if the type is implicitly specified in the source, but this has an
    /// apparently valid typeRepr.  This is used in accessors, which look like:
    ///    set (value) {
    /// but need to get the typeRepr from the property as a whole so Sema can
    /// resolve the type.
    IsTypeLocImplicit : 1,

    /// Information about a symbolic default argument, like #file.
    defaultArgumentKind : NumDefaultArgumentKindBits
  );

  SWIFT_INLINE_BITFIELD(SubscriptDecl, VarDecl, 2,
    StaticSpelling : 2
  );
  SWIFT_INLINE_BITFIELD(AbstractFunctionDecl, ValueDecl, 3+8+1+1+1+1+1+1+1+1,
    /// \see AbstractFunctionDecl::BodyKind
    BodyKind : 3,

    /// Import as member status.
    IAMStatus : 8,

    /// Whether the function has an implicit 'self' parameter.
    HasImplicitSelfDecl : 1,

    /// Whether we are overridden later.
    Overridden : 1,

    /// Whether the function body throws.
    Throws : 1,

    /// Whether this function requires a new vtable entry.
    NeedsNewVTableEntry : 1,

    /// Whether NeedsNewVTableEntry is valid.
    HasComputedNeedsNewVTableEntry : 1,

    /// Whether this member was synthesized as part of a derived
    /// protocol conformance.
    Synthesized : 1,

    /// Whether this member's body consists of a single expression.
    HasSingleExpressionBody : 1
  );

  SWIFT_INLINE_BITFIELD(FuncDecl, AbstractFunctionDecl, 1+2+1+1+2,
    /// Whether this function is a 'static' method.
    IsStatic : 1,

    /// Whether 'static' or 'class' was used.
    StaticSpelling : 2,

    /// Whether we are statically dispatched even if overridable
    ForcedStaticDispatch : 1,

    /// Whether we've computed the 'self' access kind yet.
    SelfAccessComputed : 1,

    /// Backing bits for 'self' access kind.
    SelfAccess : 2
  );

  SWIFT_INLINE_BITFIELD(AccessorDecl, FuncDecl, 4+1+1,
    /// The kind of accessor this is.
    AccessorKind : 4,

    /// Whether the accessor is transparent.
    IsTransparent : 1,

    /// Whether we have computed the above.
    IsTransparentComputed : 1
  );

  SWIFT_INLINE_BITFIELD(ConstructorDecl, AbstractFunctionDecl, 3+2+1,
    /// The body initialization kind (+1), or zero if not yet computed.
    ///
    /// This value is cached but is not serialized, because it is a property
    /// of the definition of the constructor that is useful only to semantic
    /// analysis and SIL generation.
    ComputedBodyInitKind : 3,

    /// The failability of this initializer, which is an OptionalTypeKind.
    Failability : 2,

    /// Whether this initializer is a stub placed into a subclass to
    /// catch invalid delegations to a designated initializer not
    /// overridden by the subclass. A stub will always trap at runtime.
    ///
    /// Initializer stubs can be invoked from Objective-C or through
    /// the Objective-C runtime; there is no way to directly express
    /// an object construction that will invoke a stub.
    HasStubImplementation : 1
  );

  SWIFT_INLINE_BITFIELD_EMPTY(TypeDecl, ValueDecl);
  SWIFT_INLINE_BITFIELD_EMPTY(AbstractTypeParamDecl, TypeDecl);

  SWIFT_INLINE_BITFIELD_FULL(GenericTypeParamDecl, AbstractTypeParamDecl, 16+16,
    : NumPadBits,

    Depth : 16,
    Index : 16
  );

  SWIFT_INLINE_BITFIELD_EMPTY(GenericTypeDecl, TypeDecl);

  SWIFT_INLINE_BITFIELD(TypeAliasDecl, GenericTypeDecl, 1+1,
    /// Whether the typealias forwards perfectly to its underlying type.
    IsCompatibilityAlias : 1,
    /// Whether this was a global typealias synthesized by the debugger.
    IsDebuggerAlias : 1
  );

  SWIFT_INLINE_BITFIELD(NominalTypeDecl, GenericTypeDecl, 1+1,
    /// Whether we have already added implicitly-defined initializers
    /// to this declaration.
    AddedImplicitInitializers : 1,

    /// Whether there is are lazily-loaded conformances for this nominal type.
    HasLazyConformances : 1
  );

  SWIFT_INLINE_BITFIELD_FULL(ProtocolDecl, NominalTypeDecl, 1+1+1+1+1+1+1+2+1+1+8+16,
    /// Whether the \c RequiresClass bit is valid.
    RequiresClassValid : 1,

    /// Whether this is a class-bounded protocol.
    RequiresClass : 1,

    /// Whether the \c ExistentialConformsToSelf bit is valid.
    ExistentialConformsToSelfValid : 1,

    /// Whether the existential of this protocol conforms to itself.
    ExistentialConformsToSelf : 1,

    /// Whether the \c ExistentialTypeSupported bit is valid.
    ExistentialTypeSupportedValid : 1,

    /// Whether the existential of this protocol can be represented.
    ExistentialTypeSupported : 1,

    /// True if the protocol has requirements that cannot be satisfied (e.g.
    /// because they could not be imported from Objective-C).
    HasMissingRequirements : 1,

    /// The stage of the circularity check for this protocol.
    Circularity : 2,

    /// Whether we've computed the inherited protocols list yet.
    InheritedProtocolsValid : 1,

    /// Whether we have a lazy-loaded requirement signature.
    HasLazyRequirementSignature : 1,

    : NumPadBits,

    /// If this is a compiler-known protocol, this will be a KnownProtocolKind
    /// value, plus one. Otherwise, it will be 0.
    KnownProtocol : 8, // '8' for speed. This only needs 6.

    /// The number of requirements in the requirement signature.
    NumRequirementsInSignature : 16
  );

  SWIFT_INLINE_BITFIELD(ClassDecl, NominalTypeDecl, 2+1+2+1+7+1+1+1+1,
    /// The stage of the inheritance circularity check for this class.
    Circularity : 2,

    /// Whether this class inherits its superclass's convenience initializers.
    InheritsSuperclassInits : 1,

    /// \see ClassDecl::ForeignKind
    RawForeignKind : 2,
    
    /// Whether this class contains a destructor decl.
    ///
    /// A fully type-checked class always contains a destructor member, even if
    /// it is implicit. This bit is used during parsing and type-checking to
    /// control inserting the implicit destructor.
    HasDestructorDecl : 1,

    /// Information about the class's ancestry.
    Ancestry : 7,

    /// Whether we have computed the above field or not.
    AncestryComputed : 1,

    HasMissingDesignatedInitializers : 1,
    HasMissingVTableEntries : 1,

    /// Whether instances of this class are incompatible
    /// with weak and unowned references.
    IsIncompatibleWithWeakReferences : 1
  );

  SWIFT_INLINE_BITFIELD(StructDecl, NominalTypeDecl, 1,
    /// True if this struct has storage for fields that aren't accessible in
    /// Swift.
    HasUnreferenceableStorage : 1
  );
  
  SWIFT_INLINE_BITFIELD(EnumDecl, NominalTypeDecl, 2+2+1,
    /// The stage of the raw type circularity check for this class.
    Circularity : 2,

    /// True if the enum has cases and at least one case has associated values.
    HasAssociatedValues : 2,
    /// True if the enum has at least one case that has some availability
    /// attribute.  A single bit because it's lazily computed along with the
    /// HasAssociatedValues bit.
    HasAnyUnavailableValues : 1
  );

  SWIFT_INLINE_BITFIELD(ModuleDecl, TypeDecl, 1+1+1+1+1+1+1+1,
    /// If the module was or is being compiled with `-enable-testing`.
    TestingEnabled : 1,

    /// If the module failed to load
    FailedToLoad : 1,

    /// Whether the module is resilient.
    ///
    /// \sa ResilienceStrategy
    RawResilienceStrategy : 1,

    /// Whether all imports have been resolved. Used to detect circular imports.
    HasResolvedImports : 1,

    /// If the module was or is being compiled with `-enable-private-imports`.
    PrivateImportsEnabled : 1,

    /// If the module is compiled with `-enable-implicit-dynamic`.
    ImplicitDynamicEnabled : 1,

    /// Whether the module is a system module.
    IsSystemModule : 1,

    /// Whether the module was imported from Clang (or, someday, maybe another
    /// language).
    IsNonSwiftModule : 1
  );

  SWIFT_INLINE_BITFIELD(PrecedenceGroupDecl, Decl, 1+2,
    /// Is this an assignment operator?
    IsAssignment : 1,

    /// The group's associativity.  A value of the Associativity enum.
    Associativity : 2
  );

  SWIFT_INLINE_BITFIELD(ImportDecl, Decl, 3+8,
    ImportKind : 3,

    /// The number of elements in this path.
    NumPathElements : 8
  );

  SWIFT_INLINE_BITFIELD(ExtensionDecl, Decl, 3+1,
    /// An encoding of the default and maximum access level for this extension.
    ///
    /// This is encoded as (1 << (maxAccess-1)) | (1 << (defaultAccess-1)),
    /// which works because the maximum is always greater than or equal to the
    /// default, and 'private' is never used. 0 represents an uncomputed value.
    DefaultAndMaxAccessLevel : 3,

    /// Whether there is are lazily-loaded conformances for this extension.
    HasLazyConformances : 1
  );

  SWIFT_INLINE_BITFIELD(IfConfigDecl, Decl, 1,
    /// Whether this decl is missing its closing '#endif'.
    HadMissingEnd : 1
  );

  SWIFT_INLINE_BITFIELD(PoundDiagnosticDecl, Decl, 1+1,
    /// `true` if the diagnostic is an error, `false` if it's a warning.
    IsError : 1,

    /// Whether this diagnostic has already been emitted.
    HasBeenEmitted : 1
  );

  SWIFT_INLINE_BITFIELD(MissingMemberDecl, Decl, 1+2,
    NumberOfFieldOffsetVectorEntries : 1,
    NumberOfVTableEntries : 2
  );

  } Bits;

  // Storage for the declaration attributes.
  DeclAttributes Attrs;
  
  /// The next declaration in the list of declarations within this
  /// member context.
  Decl *NextDecl = nullptr;
  
  friend class DeclIterator;
  friend class IterableDeclContext;
  friend class MemberLookupTable;

private:
  llvm::PointerUnion<DeclContext *, ASTContext *> Context;

  Decl(const Decl&) = delete;
  void operator=(const Decl&) = delete;

protected:

  Decl(DeclKind kind, llvm::PointerUnion<DeclContext *, ASTContext *> context)
    : Context(context) {
    Bits.OpaqueBits = 0;
    Bits.Decl.Kind = unsigned(kind);
    Bits.Decl.Invalid = false;
    Bits.Decl.Implicit = false;
    Bits.Decl.FromClang = false;
    Bits.Decl.EarlyAttrValidation = false;
    Bits.Decl.ValidationState = unsigned(ValidationState::Unchecked);
    Bits.Decl.EscapedFromIfConfig = false;
  }

  /// Get the Clang node associated with this declaration.
  ClangNode getClangNodeImpl() const;

  /// Set the Clang node associated with this declaration.
  void setClangNode(ClangNode Node);

  void updateClangNode(ClangNode node) {
    assert(hasClangNode());
    setClangNode(node);
  }
  friend class ClangImporter;

  DeclContext *getDeclContextForModule() const;

public:
  DeclKind getKind() const { return DeclKind(Bits.Decl.Kind); }

  /// Retrieve the name of the given declaration kind.
  ///
  /// This name should only be used for debugging dumps and other
  /// developer aids, and should never be part of a diagnostic or exposed
  /// to the user of the compiler in any way.
  static StringRef getKindName(DeclKind K);

  /// Retrieve the descriptive kind for this declaration.
  DescriptiveDeclKind getDescriptiveKind() const;

  /// Produce a name for the given descriptive declaration kind, which
  /// is suitable for use in diagnostics.
  static StringRef getDescriptiveKindName(DescriptiveDeclKind K);

  /// Whether swift users should be able to access this decl. For instance,
  /// var a.storage for lazy var a is an inaccessible decl. An inaccessible decl
  /// has to be implicit; but an implicit decl does not have to be inaccessible,
  /// for instance, self.
  bool isUserAccessible() const;

  /// Determine if the decl can have a comment.  If false, a comment will
  /// not be serialized.
  bool canHaveComment() const;

  LLVM_READONLY
  DeclContext *getDeclContext() const {
    if (auto dc = Context.dyn_cast<DeclContext *>())
      return dc;

    return getDeclContextForModule();
  }
  void setDeclContext(DeclContext *DC);

  /// Retrieve the innermost declaration context corresponding to this
  /// declaration, which will either be the declaration itself (if it's
  /// also a declaration context) or its declaration context.
  DeclContext *getInnermostDeclContext() const;

  /// Retrieve the module in which this declaration resides.
  LLVM_READONLY
  ModuleDecl *getModuleContext() const;

  /// getASTContext - Return the ASTContext that this decl lives in.
  LLVM_READONLY
  ASTContext &getASTContext() const {
    if (auto dc = Context.dyn_cast<DeclContext *>())
      return dc->getASTContext();

    return *Context.get<ASTContext *>();
  }

  const DeclAttributes &getAttrs() const {
    return Attrs;
  }

  DeclAttributes &getAttrs() {
    return Attrs;
  }

  /// Returns the starting location of the entire declaration.
  SourceLoc getStartLoc() const { return getSourceRange().Start; }

  /// Returns the end location of the entire declaration.
  SourceLoc getEndLoc() const { return getSourceRange().End; }

  /// Returns the preferred location when referring to declarations
  /// in diagnostics.
  SourceLoc getLoc() const;

  /// Returns the source range of the entire declaration.
  SourceRange getSourceRange() const;

  /// Returns the source range of the declaration including its attributes.
  SourceRange getSourceRangeIncludingAttrs() const;

  SourceLoc TrailingSemiLoc;

  LLVM_ATTRIBUTE_DEPRECATED(
      void dump() const LLVM_ATTRIBUTE_USED,
      "only for use within the debugger");
  LLVM_ATTRIBUTE_DEPRECATED(
      void dump(const char *filename) const LLVM_ATTRIBUTE_USED,
      "only for use within the debugger");
  void dump(raw_ostream &OS, unsigned Indent = 0) const;

  /// Pretty-print the given declaration.
  ///
  /// \param OS Output stream to which the declaration will be printed.
  void print(raw_ostream &OS) const;
  void print(raw_ostream &OS, const PrintOptions &Opts) const;

  /// Pretty-print the given declaration.
  ///
  /// \param Printer ASTPrinter object.
  ///
  /// \param Opts Options to control how pretty-printing is performed.
  ///
  /// \returns true if the declaration was printed or false if the print options
  /// required the declaration to be skipped from printing.
  bool print(ASTPrinter &Printer, const PrintOptions &Opts) const;

  /// Determine whether this declaration should be printed when
  /// encountered in its declaration context's list of members.
  bool shouldPrintInContext(const PrintOptions &PO) const;

  bool walk(ASTWalker &walker);

  /// Return whether this declaration has been determined invalid.
  bool isInvalid() const { return Bits.Decl.Invalid; }
  
  /// Mark this declaration invalid.
  void setInvalid(bool isInvalid = true) { Bits.Decl.Invalid = isInvalid; }

  /// Determine whether this declaration was implicitly generated by the
  /// compiler (rather than explicitly written in source code).
  bool isImplicit() const { return Bits.Decl.Implicit; }

  /// Mark this declaration as implicit.
  void setImplicit(bool implicit = true) { Bits.Decl.Implicit = implicit; }

  /// Whether we have already done early attribute validation.
  bool didEarlyAttrValidation() const { return Bits.Decl.EarlyAttrValidation; }

  /// Set whether we've performed early attribute validation.
  void setEarlyAttrValidation(bool validated = true) {
    Bits.Decl.EarlyAttrValidation = validated;
  }
  
  /// Get the validation state.
  ValidationState getValidationState() const {
    return ValidationState(Bits.Decl.ValidationState);
  }

private:
  friend class DeclValidationRAII;

  /// Set the validation state.
  void setValidationState(ValidationState VS) {
    assert(VS > getValidationState() && "Validation is unidirectional");
    Bits.Decl.ValidationState = unsigned(VS);
  }

public:
  /// Whether the declaration is in the middle of validation or not.
  bool isBeingValidated() const {
    switch (getValidationState()) {
    case ValidationState::Unchecked:
    case ValidationState::Checked:
      return false;
    case ValidationState::Checking:
    case ValidationState::CheckingWithValidSignature:
      return true;
    }
    llvm_unreachable("Unknown ValidationState");
  }

  /// Update the validation state for the declaration to allow access to the
  /// generic signature.
  void setSignatureIsValidated() {
    assert(getValidationState() == ValidationState::Checking);
    setValidationState(ValidationState::CheckingWithValidSignature);
  }

  bool hasValidationStarted() const {
    return getValidationState() > ValidationState::Unchecked;
  }

  /// Manually indicate that validation is complete for the declaration. For
  /// example: during importing, code synthesis, or derived conformances.
  ///
  /// For normal code validation, please use DeclValidationRAII instead.
  ///
  /// FIXME -- Everything should use DeclValidationRAII instead of this.
  void setValidationToChecked() {
    if (!isBeingValidated())
      Bits.Decl.ValidationState = unsigned(ValidationState::Checked);
  }

  bool escapedFromIfConfig() const {
    return Bits.Decl.EscapedFromIfConfig;
  }

  void setEscapedFromIfConfig(bool Escaped) {
    Bits.Decl.EscapedFromIfConfig = Escaped;
  }

  /// \returns the unparsed comment attached to this declaration.
  RawComment getRawComment() const;

  Optional<StringRef> getGroupName() const;

  Optional<StringRef> getSourceFileName() const;

  Optional<unsigned> getSourceOrder() const;

  /// \returns the brief comment attached to this declaration.
  StringRef getBriefComment() const;

  /// Returns true if there is a Clang AST node associated
  /// with self.
  bool hasClangNode() const {
    return Bits.Decl.FromClang;
  }

  /// Retrieve the Clang AST node from which this declaration was
  /// synthesized, if any.
  LLVM_READONLY
  ClangNode getClangNode() const {
    if (!Bits.Decl.FromClang)
      return ClangNode();

    return getClangNodeImpl();
  }

  /// Retrieve the Clang declaration from which this declaration was
  /// synthesized, if any.
  LLVM_READONLY
  const clang::Decl *getClangDecl() const {
    if (!Bits.Decl.FromClang)
      return nullptr;

    return getClangNodeImpl().getAsDecl();
  }

  /// Retrieve the Clang macro from which this declaration was
  /// synthesized, if any.
  LLVM_READONLY
  const clang::MacroInfo *getClangMacro() {
    if (!Bits.Decl.FromClang)
      return nullptr;

    return getClangNodeImpl().getAsMacro();
  }

  /// Return the GenericContext if the Decl has one.
  LLVM_READONLY
  const GenericContext *getAsGenericContext() const;

  bool isPrivateStdlibDecl(bool treatNonBuiltinProtocolsAsPublic = true) const;

  /// Whether this declaration is weak-imported.
  bool isWeakImported(ModuleDecl *fromModule,
                      AvailabilityContext fromContext) const;

  /// Returns true if the nature of this declaration allows overrides.
  /// Note that this does not consider whether it is final or whether
  /// the class it's on is final.
  ///
  /// If this returns true, the decl can be safely casted to ValueDecl.
  bool isPotentiallyOverridable() const;

  /// Emit a diagnostic tied to this declaration.
  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(
      Diag<ArgTypes...> ID,
      typename detail::PassArgument<ArgTypes>::type... Args) const {
    return getDiags().diagnose(this, ID, std::move(Args)...);
  }

  /// Retrieve the diagnostic engine for diagnostics emission.
  LLVM_READONLY
  DiagnosticEngine &getDiags() const;

  // Make vanilla new/delete illegal for Decls.
  void *operator new(size_t Bytes) = delete;
  void operator delete(void *Data) SWIFT_DELETE_OPERATOR_DELETED;

  // Only allow allocation of Decls using the allocator in ASTContext
  // or by doing a placement new.
  void *operator new(size_t Bytes, const ASTContext &C,
                     unsigned Alignment = alignof(Decl));
  void *operator new(size_t Bytes, void *Mem) { 
    assert(Mem); 
    return Mem; 
  }
};

/// Use RAII to track Decl validation progress and non-reentrancy.
class DeclValidationRAII {
  Decl *D;

public:
  DeclValidationRAII(const DeclValidationRAII &) = delete;
  DeclValidationRAII(DeclValidationRAII &&) = delete;
  void operator =(const DeclValidationRAII &) = delete;
  void operator =(DeclValidationRAII &&) = delete;

  DeclValidationRAII(Decl *decl) : D(decl) {
    D->setValidationState(Decl::ValidationState::Checking);
  }

  ~DeclValidationRAII() {
    D->setValidationState(Decl::ValidationState::Checked);
  }
};

/// Allocates memory for a Decl with the given \p baseSize. If necessary,
/// it includes additional space immediately preceding the Decl for a ClangNode.
/// \note \p baseSize does not need to include space for a ClangNode if
/// requested -- the necessary space will be added automatically.
template <typename DeclTy, typename AllocatorTy>
void *allocateMemoryForDecl(AllocatorTy &allocator, size_t baseSize,
                            bool includeSpaceForClangNode) {
  static_assert(alignof(DeclTy) >= sizeof(void *),
                "A pointer must fit in the alignment of the DeclTy!");

  size_t size = baseSize;
  if (includeSpaceForClangNode)
    size += alignof(DeclTy);

  void *mem = allocator.Allocate(size, alignof(DeclTy));
  if (includeSpaceForClangNode)
    mem = reinterpret_cast<char *>(mem) + alignof(DeclTy);
  return mem;
}

enum class RequirementReprKind : unsigned {
  /// A type bound T : P, where T is a type that depends on a generic
  /// parameter and P is some type that should bound T, either as a concrete
  /// supertype or a protocol to which T must conform.
  TypeConstraint,

  /// A same-type requirement T == U, where T and U are types that shall be
  /// equivalent.
  SameType,

  /// A layout bound T : L, where T is a type that depends on a generic
  /// parameter and L is some layout specification that should bound T.
  LayoutConstraint,

  // Note: there is code that packs this enum in a 2-bit bitfield.  Audit users
  // when adding enumerators.
};

/// A single requirement in a 'where' clause, which places additional
/// restrictions on the generic parameters or associated types of a generic
/// function, type, or protocol.
///
/// This always represents a requirement spelled in the source code.  It is
/// never generated implicitly.
///
/// \c GenericParamList assumes these are POD-like.
class RequirementRepr {
  SourceLoc SeparatorLoc;
  RequirementReprKind Kind : 2;
  bool Invalid : 1;
  TypeLoc FirstType;

  /// The second element represents the right-hand side of the constraint.
  /// It can be e.g. a type or a layout constraint.
  union {
    TypeLoc SecondType;
    LayoutConstraintLoc SecondLayout;
  };

  /// Set during deserialization; used to print out the requirements accurately
  /// for the generated interface.
  StringRef AsWrittenString;

  RequirementRepr(SourceLoc SeparatorLoc, RequirementReprKind Kind,
                  TypeLoc FirstType, TypeLoc SecondType)
    : SeparatorLoc(SeparatorLoc), Kind(Kind), Invalid(false),
      FirstType(FirstType), SecondType(SecondType) { }

  RequirementRepr(SourceLoc SeparatorLoc, RequirementReprKind Kind,
                  TypeLoc FirstType, LayoutConstraintLoc SecondLayout)
    : SeparatorLoc(SeparatorLoc), Kind(Kind), Invalid(false),
      FirstType(FirstType), SecondLayout(SecondLayout) { }

  void printImpl(ASTPrinter &OS, bool AsWritten) const;

public:
  /// Construct a new type-constraint requirement.
  ///
  /// \param Subject The type that must conform to the given protocol or
  /// composition, or be a subclass of the given class type.
  /// \param ColonLoc The location of the ':', or an invalid location if
  /// this requirement was implied.
  /// \param Constraint The protocol or protocol composition to which the
  /// subject must conform, or superclass from which the subject must inherit.
  static RequirementRepr getTypeConstraint(TypeLoc Subject,
                                           SourceLoc ColonLoc,
                                           TypeLoc Constraint) {
    return { ColonLoc, RequirementReprKind::TypeConstraint, Subject, Constraint };
  }

  /// Construct a new same-type requirement.
  ///
  /// \param FirstType The first type.
  /// \param EqualLoc The location of the '==' in the same-type constraint, or
  /// an invalid location if this requirement was implied.
  /// \param SecondType The second type.
  static RequirementRepr getSameType(TypeLoc FirstType,
                                     SourceLoc EqualLoc,
                                     TypeLoc SecondType) {
    return { EqualLoc, RequirementReprKind::SameType, FirstType, SecondType };
  }

  /// Construct a new layout-constraint requirement.
  ///
  /// \param Subject The type that must conform to the given layout 
  /// requirement.
  /// \param ColonLoc The location of the ':', or an invalid location if
  /// this requirement was implied.
  /// \param Layout The layout requirement to which the
  /// subject must conform.
  static RequirementRepr getLayoutConstraint(TypeLoc Subject,
                                             SourceLoc ColonLoc,
                                             LayoutConstraintLoc Layout) {
    return {ColonLoc, RequirementReprKind::LayoutConstraint, Subject,
            Layout};
  }

  /// Determine the kind of requirement
  RequirementReprKind getKind() const { return Kind; }

  /// Determine whether this requirement is invalid.
  bool isInvalid() const { return Invalid; }

  /// Mark this requirement invalid.
  void setInvalid() { Invalid = true; }

  /// For a type-bound requirement, return the subject of the
  /// conformance relationship.
  Type getSubject() const {
    assert(getKind() == RequirementReprKind::TypeConstraint ||
           getKind() == RequirementReprKind::LayoutConstraint);
    return FirstType.getType();
  }

  TypeRepr *getSubjectRepr() const {
    assert(getKind() == RequirementReprKind::TypeConstraint ||
           getKind() == RequirementReprKind::LayoutConstraint);
    return FirstType.getTypeRepr();
  }

  TypeLoc &getSubjectLoc() {
    assert(getKind() == RequirementReprKind::TypeConstraint ||
           getKind() == RequirementReprKind::LayoutConstraint);
    return FirstType;
  }

  const TypeLoc &getSubjectLoc() const {
    assert(getKind() == RequirementReprKind::TypeConstraint ||
           getKind() == RequirementReprKind::LayoutConstraint);
    return FirstType;
  }

  /// For a type-bound requirement, return the protocol or to which
  /// the subject conforms or superclass it inherits.
  Type getConstraint() const {
    assert(getKind() == RequirementReprKind::TypeConstraint);
    return SecondType.getType();
  }

  TypeRepr *getConstraintRepr() const {
    assert(getKind() == RequirementReprKind::TypeConstraint);
    return SecondType.getTypeRepr();
  }

  TypeLoc &getConstraintLoc() {
    assert(getKind() == RequirementReprKind::TypeConstraint);
    return SecondType;
  }

  const TypeLoc &getConstraintLoc() const {
    assert(getKind() == RequirementReprKind::TypeConstraint);
    return SecondType;
  }

  LayoutConstraint getLayoutConstraint() const {
    assert(getKind() == RequirementReprKind::LayoutConstraint);
    return SecondLayout.getLayoutConstraint();
  }

  LayoutConstraintLoc &getLayoutConstraintLoc() {
    assert(getKind() == RequirementReprKind::LayoutConstraint);
    return SecondLayout;
  }

  const LayoutConstraintLoc &getLayoutConstraintLoc() const {
    assert(getKind() == RequirementReprKind::LayoutConstraint);
    return SecondLayout;
  }

  /// Retrieve the first type of a same-type requirement.
  Type getFirstType() const {
    assert(getKind() == RequirementReprKind::SameType);
    return FirstType.getType();
  }

  TypeRepr *getFirstTypeRepr() const {
    assert(getKind() == RequirementReprKind::SameType);
    return FirstType.getTypeRepr();
  }

  TypeLoc &getFirstTypeLoc() {
    assert(getKind() == RequirementReprKind::SameType);
    return FirstType;
  }

  const TypeLoc &getFirstTypeLoc() const {
    assert(getKind() == RequirementReprKind::SameType);
    return FirstType;
  }

  /// Retrieve the second type of a same-type requirement.
  Type getSecondType() const {
    assert(getKind() == RequirementReprKind::SameType);
    return SecondType.getType();
  }

  TypeRepr *getSecondTypeRepr() const {
    assert(getKind() == RequirementReprKind::SameType);
    return SecondType.getTypeRepr();
  }

  TypeLoc &getSecondTypeLoc() {
    assert(getKind() == RequirementReprKind::SameType);
    return SecondType;
  }

  const TypeLoc &getSecondTypeLoc() const {
    assert(getKind() == RequirementReprKind::SameType);
    return SecondType;
  }

  /// Retrieve the location of the ':' or '==' in an explicitly-written
  /// conformance or same-type requirement respectively.
  SourceLoc getSeparatorLoc() const {
    return SeparatorLoc;
  }

  SourceRange getSourceRange() const {
    if (getKind() == RequirementReprKind::LayoutConstraint)
      return SourceRange(FirstType.getSourceRange().Start,
                         SecondLayout.getSourceRange().End);
    return SourceRange(FirstType.getSourceRange().Start,
                       SecondType.getSourceRange().End);
  }

  /// Retrieve the first or subject type representation from the \c repr,
  /// or \c nullptr if \c repr is null.
  static TypeRepr *getFirstTypeRepr(const RequirementRepr *repr) {
    if (!repr) return nullptr;
    return repr->FirstType.getTypeRepr();
  }

  /// Retrieve the second or constraint type representation from the \c repr,
  /// or \c nullptr if \c repr is null.
  static TypeRepr *getSecondTypeRepr(const RequirementRepr *repr) {
    if (!repr) return nullptr;
    assert(repr->getKind() == RequirementReprKind::TypeConstraint ||
           repr->getKind() == RequirementReprKind::SameType);
    return repr->SecondType.getTypeRepr();
  }

  LLVM_ATTRIBUTE_DEPRECATED(
      void dump() const LLVM_ATTRIBUTE_USED,
      "only for use within the debugger");
  void print(raw_ostream &OS) const;
  void print(ASTPrinter &Printer) const;
};
  
/// GenericParamList - A list of generic parameters that is part of a generic
/// function or type, along with extra requirements placed on those generic
/// parameters and types derived from them.
class GenericParamList final :
    private llvm::TrailingObjects<GenericParamList, GenericTypeParamDecl *> {
  friend TrailingObjects;

  SourceRange Brackets;
  unsigned NumParams;
  SourceLoc WhereLoc;
  MutableArrayRef<RequirementRepr> Requirements;

  GenericParamList *OuterParameters;

  SourceLoc TrailingWhereLoc;
  unsigned FirstTrailingWhereArg;

  GenericParamList(SourceLoc LAngleLoc,
                   ArrayRef<GenericTypeParamDecl *> Params,
                   SourceLoc WhereLoc,
                   MutableArrayRef<RequirementRepr> Requirements,
                   SourceLoc RAngleLoc);
  
  // Don't copy.
  GenericParamList(const GenericParamList &) = delete;
  GenericParamList &operator=(const GenericParamList &) = delete;
  
public:
  /// create - Create a new generic parameter list within the given AST context.
  ///
  /// \param Context The ASTContext in which the generic parameter list will
  /// be allocated.
  /// \param LAngleLoc The location of the opening angle bracket ('<')
  /// \param Params The list of generic parameters, which will be copied into
  /// ASTContext-allocated memory.
  /// \param RAngleLoc The location of the closing angle bracket ('>')
  static GenericParamList *create(ASTContext &Context,
                                  SourceLoc LAngleLoc,
                                  ArrayRef<GenericTypeParamDecl *> Params,
                                  SourceLoc RAngleLoc);

  /// create - Create a new generic parameter list and "where" clause within
  /// the given AST context.
  ///
  /// \param Context The ASTContext in which the generic parameter list will
  /// be allocated.
  /// \param LAngleLoc The location of the opening angle bracket ('<')
  /// \param Params The list of generic parameters, which will be copied into
  /// ASTContext-allocated memory.
  /// \param WhereLoc The location of the 'where' keyword, if any.
  /// \param Requirements The list of requirements, which will be copied into
  /// ASTContext-allocated memory.
  /// \param RAngleLoc The location of the closing angle bracket ('>')
  static GenericParamList *create(const ASTContext &Context,
                                  SourceLoc LAngleLoc,
                                  ArrayRef<GenericTypeParamDecl *> Params,
                                  SourceLoc WhereLoc,
                                  ArrayRef<RequirementRepr> Requirements,
                                  SourceLoc RAngleLoc);

  MutableArrayRef<GenericTypeParamDecl *> getParams() {
    return {getTrailingObjects<GenericTypeParamDecl *>(), NumParams};
  }

  ArrayRef<GenericTypeParamDecl *> getParams() const {
    return {getTrailingObjects<GenericTypeParamDecl *>(), NumParams};
  }

  using iterator = GenericTypeParamDecl **;
  using const_iterator = const GenericTypeParamDecl * const *;

  unsigned size() const { return NumParams; }
  iterator begin() { return getParams().begin(); }
  iterator end() { return getParams().end(); }
  const_iterator begin() const { return getParams().begin(); }
  const_iterator end() const { return getParams().end(); }

  /// Retrieve the location of the 'where' keyword, or an invalid
  /// location if 'where' was not present.
  SourceLoc getWhereLoc() const { return WhereLoc; }

  /// Retrieve the set of additional requirements placed on these
  /// generic parameters and types derived from them.
  ///
  /// This list may contain both explicitly-written requirements as well as
  /// implicitly-generated requirements, and may be non-empty even if no
  /// 'where' keyword is present.
  MutableArrayRef<RequirementRepr> getRequirements() { return Requirements; }

  /// Retrieve the set of additional requirements placed on these
  /// generic parameters and types derived from them.
  ///
  /// This list may contain both explicitly-written requirements as well as
  /// implicitly-generated requirements, and may be non-empty even if no
  /// 'where' keyword is present.
  ArrayRef<RequirementRepr> getRequirements() const { return Requirements; }

  /// Retrieve only those requirements that are written within the brackets,
  /// which does not include any requirements written in a trailing where
  /// clause.
  ArrayRef<RequirementRepr> getNonTrailingRequirements() const {
    return Requirements.slice(0, FirstTrailingWhereArg);
  }

  /// Retrieve only those requirements written in a trailing where
  /// clause.
  ArrayRef<RequirementRepr> getTrailingRequirements() const {
    return Requirements.slice(FirstTrailingWhereArg);
  }

  /// Determine whether the generic parameters have a trailing where clause.
  bool hasTrailingWhereClause() const {
    return FirstTrailingWhereArg < Requirements.size();
  }

  /// Add a trailing 'where' clause to the list of requirements.
  ///
  /// Trailing where clauses are written outside the angle brackets, after the
  /// main part of a declaration's signature.
  void addTrailingWhereClause(ASTContext &ctx, SourceLoc trailingWhereLoc,
                              ArrayRef<RequirementRepr> trailingRequirements);
  
  /// Retrieve the outer generic parameter list.
  ///
  /// This is used for extensions of nested types, and in SIL mode, where a
  /// single lexical context can have multiple logical generic parameter
  /// lists.
  GenericParamList *getOuterParameters() const { return OuterParameters; }

  /// Set the outer generic parameter list. See \c getOuterParameters
  /// for more information.
  void setOuterParameters(GenericParamList *Outer) { OuterParameters = Outer; }

  SourceLoc getLAngleLoc() const { return Brackets.Start; }
  SourceLoc getRAngleLoc() const { return Brackets.End; }

  SourceRange getSourceRange() const { return Brackets; }

  /// Retrieve the source range covering the where clause.
  SourceRange getWhereClauseSourceRange() const {
    if (WhereLoc.isInvalid())
      return SourceRange();

    auto endLoc = Requirements[FirstTrailingWhereArg-1].getSourceRange().End;
    return SourceRange(WhereLoc, endLoc);
  }

  /// Retrieve the source range covering the trailing where clause.
  SourceRange getTrailingWhereClauseSourceRange() const {
    if (!hasTrailingWhereClause())
      return SourceRange();

    return SourceRange(TrailingWhereLoc,
                       Requirements.back().getSourceRange().End);
  }

  /// Configure the depth of the generic parameters in this list.
  void setDepth(unsigned depth);

  /// Create a copy of the generic parameter list and all of its generic
  /// parameter declarations. The copied generic parameters are re-parented
  /// to the given DeclContext.
  GenericParamList *clone(DeclContext *dc) const;

  void print(raw_ostream &OS);
  void dump();
};
  
/// A trailing where clause.
class alignas(RequirementRepr) TrailingWhereClause final :
    private llvm::TrailingObjects<TrailingWhereClause, RequirementRepr> {
  friend TrailingObjects;

  SourceLoc WhereLoc;

  /// The number of requirements. The actual requirements are tail-allocated.
  unsigned NumRequirements;

  TrailingWhereClause(SourceLoc whereLoc,
                      ArrayRef<RequirementRepr> requirements);

public:
  /// Create a new trailing where clause with the given set of requirements.
  static TrailingWhereClause *create(ASTContext &ctx, SourceLoc whereLoc,
                                     ArrayRef<RequirementRepr> requirements);

  /// Retrieve the location of the 'where' keyword.
  SourceLoc getWhereLoc() const { return WhereLoc; }

  /// Retrieve the set of requirements.
  MutableArrayRef<RequirementRepr> getRequirements() {
    return {getTrailingObjects<RequirementRepr>(), NumRequirements};
  }

  /// Retrieve the set of requirements.
  ArrayRef<RequirementRepr> getRequirements() const {
    return {getTrailingObjects<RequirementRepr>(), NumRequirements};
  }

  /// Compute the source range containing this trailing where clause.
  SourceRange getSourceRange() const {
    return SourceRange(WhereLoc,
                       getRequirements().back().getSourceRange().End);
  }
};

// A private class for forcing exact field layout.
class alignas(8) _GenericContext {
// Not really public. See GenericContext.
public:
  GenericParamList *GenericParams = nullptr;

  /// The trailing where clause.
  ///
  /// Note that this is not currently serialized, because semantic analysis
  /// moves the trailing where clause into the generic parameter list.
  TrailingWhereClause *TrailingWhere = nullptr;

  /// The generic signature or environment of this declaration.
  ///
  /// When this declaration stores only a signature, the generic
  /// environment will be lazily loaded.
  mutable llvm::PointerUnion<GenericSignature *, GenericEnvironment *>
    GenericSigOrEnv;
};

class GenericContext : private _GenericContext, public DeclContext {
  /// Lazily populate the generic environment.
  GenericEnvironment *getLazyGenericEnvironmentSlow() const;

protected:
  GenericContext(DeclContextKind Kind, DeclContext *Parent)
    : _GenericContext(), DeclContext(Kind, Parent) { }

public:
  /// Retrieve the set of parameters to a generic context, or null if
  /// this context is not generic.
  GenericParamList *getGenericParams() const { return GenericParams; }

  void setGenericParams(GenericParamList *GenericParams);

  /// Determine whether this context has generic parameters
  /// of its own.
  bool isGeneric() const { return GenericParams != nullptr; }

  /// Retrieve the trailing where clause for this extension, if any.
  TrailingWhereClause *getTrailingWhereClause() const {
    return TrailingWhere;
  }

  /// Set the trailing where clause for this extension.
  void setTrailingWhereClause(TrailingWhereClause *trailingWhereClause) {
    TrailingWhere = trailingWhereClause;
  }

  /// Retrieve the generic signature for this context.
  GenericSignature *getGenericSignature() const;

  /// Retrieve the generic context for this context.
  GenericEnvironment *getGenericEnvironment() const;

  /// Retrieve the innermost generic parameter types.
  TypeArrayView<GenericTypeParamType> getInnermostGenericParamTypes() const;

  /// Retrieve the generic requirements.
  ArrayRef<Requirement> getGenericRequirements() const;

  /// Set a lazy generic environment.
  void setLazyGenericEnvironment(LazyMemberLoader *lazyLoader,
                                 GenericSignature *genericSig,
                                 uint64_t genericEnvData);

  /// Whether this generic context has a lazily-created generic environment
  /// that has not yet been constructed.
  bool hasLazyGenericEnvironment() const;

  /// Set the generic context of this context.
  void setGenericEnvironment(GenericEnvironment *genericEnv);

  /// Retrieve the position of any where clause for this context's
  /// generic parameters.
  SourceRange getGenericTrailingWhereClauseSourceRange() const;
};
static_assert(sizeof(_GenericContext) + sizeof(DeclContext) ==
              sizeof(GenericContext), "Please add fields to _GenericContext");

/// Describes what kind of name is being imported.
///
/// If the enumerators here are changed, make sure to update all diagnostics
/// using ImportKind as a select index.
enum class ImportKind : uint8_t {
  Module = 0,
  Type,
  Struct,
  Class,
  Enum,
  Protocol,
  Var,
  Func
};

/// ImportDecl - This represents a single import declaration, e.g.:
///   import Swift
///   import typealias Swift.Int
class ImportDecl final : public Decl,
    private llvm::TrailingObjects<ImportDecl, std::pair<Identifier,SourceLoc>> {
  friend TrailingObjects;

public:
  typedef std::pair<Identifier, SourceLoc> AccessPathElement;

private:
  SourceLoc ImportLoc;
  SourceLoc KindLoc;

  /// The resolved module.
  ModuleDecl *Mod = nullptr;
  /// The resolved decls if this is a decl import.
  ArrayRef<ValueDecl *> Decls;

  ImportDecl(DeclContext *DC, SourceLoc ImportLoc, ImportKind K,
             SourceLoc KindLoc, ArrayRef<AccessPathElement> Path);

public:
  static ImportDecl *create(ASTContext &C, DeclContext *DC,
                            SourceLoc ImportLoc, ImportKind Kind,
                            SourceLoc KindLoc,
                            ArrayRef<AccessPathElement> Path,
                            ClangNode ClangN = ClangNode());

  /// Returns the import kind that is most appropriate for \p VD.
  ///
  /// Note that this will never return \c Type; an imported typealias will use
  /// the more specific kind from its underlying type.
  static ImportKind getBestImportKind(const ValueDecl *VD);

  /// Returns the most appropriate import kind for the given list of decls.
  ///
  /// If the list is non-homogeneous, or if there is more than one decl that
  /// cannot be overloaded, returns None.
  static Optional<ImportKind> findBestImportKind(ArrayRef<ValueDecl *> Decls);

  ArrayRef<AccessPathElement> getFullAccessPath() const {
    return {getTrailingObjects<AccessPathElement>(),
            static_cast<size_t>(Bits.ImportDecl.NumPathElements)};
  }

  ArrayRef<AccessPathElement> getModulePath() const {
    auto result = getFullAccessPath();
    if (getImportKind() != ImportKind::Module)
      result = result.slice(0, result.size()-1);
    return result;
  }

  ArrayRef<AccessPathElement> getDeclPath() const {
    if (getImportKind() == ImportKind::Module)
      return {};
    return getFullAccessPath().back();
  }

  ImportKind getImportKind() const {
    return static_cast<ImportKind>(Bits.ImportDecl.ImportKind);
  }

  bool isExported() const {
    return getAttrs().hasAttribute<ExportedAttr>();
  }

  ModuleDecl *getModule() const { return Mod; }
  void setModule(ModuleDecl *M) { Mod = M; }

  ArrayRef<ValueDecl *> getDecls() const { return Decls; }
  void setDecls(ArrayRef<ValueDecl *> Ds) { Decls = Ds; }

  const clang::Module *getClangModule() const {
    return getClangNode().getClangModule();
  }

  SourceLoc getStartLoc() const { return ImportLoc; }
  SourceLoc getLoc() const { return getFullAccessPath().front().second; }
  SourceRange getSourceRange() const {
    return SourceRange(ImportLoc, getFullAccessPath().back().second);
  }
  SourceLoc getKindLoc() const { return KindLoc; }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Import;
  }
};

/// ExtensionDecl - This represents a type extension containing methods
/// associated with the type.  This is not a ValueDecl and has no Type because
/// there are no runtime values of the Extension's type.  
class ExtensionDecl final : public GenericContext, public Decl,
                            public IterableDeclContext {
  SourceLoc ExtensionLoc;  // Location of 'extension' keyword.
  SourceRange Braces;

  /// The type being extended.
  TypeLoc ExtendedType;

  /// The nominal type being extended.
  NominalTypeDecl *ExtendedNominal = nullptr;

  MutableArrayRef<TypeLoc> Inherited;

  /// The next extension in the linked list of extensions.
  ///
  /// The bit indicates whether this extension has been resolved to refer to
  /// a known nominal type.
  llvm::PointerIntPair<ExtensionDecl *, 1, bool> NextExtension
    = {nullptr, false};

  /// Note that we have added a member into the iterable declaration context.
  void addedMember(Decl *member);

  friend class ExtensionIterator;
  friend class NominalTypeDecl;
  friend class MemberLookupTable;
  friend class ConformanceLookupTable;
  friend class IterableDeclContext;

  ExtensionDecl(SourceLoc extensionLoc, TypeLoc extendedType,
                MutableArrayRef<TypeLoc> inherited,
                DeclContext *parent,
                TrailingWhereClause *trailingWhereClause);

  /// Retrieve the conformance loader (if any), and removing it in the
  /// same operation. The caller is responsible for loading the
  /// conformances.
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoader() {
    if (!Bits.ExtensionDecl.HasLazyConformances)
      return { nullptr, 0 };

    return takeConformanceLoaderSlow();
  }

  /// Slow path for \c takeConformanceLoader().
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoaderSlow();

  friend class ExtendedNominalRequest;
public:
  using Decl::getASTContext;

  /// Create a new extension declaration.
  static ExtensionDecl *create(ASTContext &ctx, SourceLoc extensionLoc,
                               TypeLoc extendedType,
                               MutableArrayRef<TypeLoc> inherited,
                               DeclContext *parent,
                               TrailingWhereClause *trailingWhereClause,
                               ClangNode clangNode = ClangNode());

  SourceLoc getStartLoc() const { return ExtensionLoc; }
  SourceLoc getLoc() const { return ExtensionLoc; }
  SourceRange getSourceRange() const {
    return { ExtensionLoc, Braces.End };
  }

  SourceRange getBraces() const { return Braces; }
  void setBraces(SourceRange braces) { Braces = braces; }

  /// Retrieve the type being extended.
  ///
  /// Only use this entry point when the complete type, as spelled in the source,
  /// is required. For most clients, \c getExtendedNominal(), which provides
  /// only the \c NominalTypeDecl, will suffice.
  Type getExtendedType() const { return ExtendedType.getType(); }

  /// Retrieve the nominal type declaration that is being extended.
  NominalTypeDecl *getExtendedNominal() const;

  /// Determine whether this extension has already been bound to a nominal
  /// type declaration.
  bool alreadyBoundToNominal() const { return NextExtension.getInt(); }

  /// Retrieve the extended type location.
  TypeLoc &getExtendedTypeLoc() { return ExtendedType; }

  /// Retrieve the extended type location.
  const TypeLoc &getExtendedTypeLoc() const { return ExtendedType; }

  /// Retrieve the set of protocols that this type inherits (i.e,
  /// explicitly conforms to).
  MutableArrayRef<TypeLoc> getInherited() { return Inherited; }
  ArrayRef<TypeLoc> getInherited() const { return Inherited; }

  void setInherited(MutableArrayRef<TypeLoc> i) { Inherited = i; }

  /// Whether we have fully checked the extension signature.
  bool hasValidSignature() const {
    return getValidationState() > ValidationState::CheckingWithValidSignature;
  }

  void createGenericParamsIfMissing(NominalTypeDecl *nominal);

  bool hasDefaultAccessLevel() const {
    return Bits.ExtensionDecl.DefaultAndMaxAccessLevel != 0;
  }

  uint8_t getDefaultAndMaxAccessLevelBits() const {
    return Bits.ExtensionDecl.DefaultAndMaxAccessLevel;
  }
  void setDefaultAndMaxAccessLevelBits(AccessLevel defaultAccess,
                                       AccessLevel maxAccess) {
    Bits.ExtensionDecl.DefaultAndMaxAccessLevel =
      (1 << (static_cast<unsigned>(defaultAccess) - 1)) |
      (1 << (static_cast<unsigned>(maxAccess) - 1));
  }

  AccessLevel getDefaultAccessLevel() const;
  AccessLevel getMaxAccessLevel() const;

  void setDefaultAndMaxAccess(AccessLevel defaultAccess,
                              AccessLevel maxAccess) {
    assert(!hasDefaultAccessLevel() && "default access level already set");
    assert(maxAccess >= defaultAccess);
    assert(maxAccess != AccessLevel::Private && "private not valid");
    assert(defaultAccess != AccessLevel::Private && "private not valid");
    setDefaultAndMaxAccessLevelBits(defaultAccess, maxAccess);
    assert(getDefaultAccessLevel() == defaultAccess && "not enough bits");
    assert(getMaxAccessLevel() == maxAccess && "not enough bits");
  }

  void setConformanceLoader(LazyMemberLoader *resolver, uint64_t contextData);

  /// Determine whether this is a constrained extension, which adds additional
  /// requirements beyond those of the nominal type.
  bool isConstrainedExtension() const;
  
  /// Determine whether this extension context is interchangeable with the
  /// original nominal type context.
  ///
  /// False if any of the following properties hold:
  /// - the extension is defined in a different module from the original
  ///   nominal type decl,
  /// - the extension is constrained, or
  /// - the extension is to a protocol.
  /// FIXME: In a world where protocol extensions are dynamically dispatched,
  /// "extension is to a protocol" would no longer be a reason to use the
  /// extension mangling, because an extension method implementation could be
  /// resiliently moved into the original protocol itself.
  bool isEquivalentToExtendedContext() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Extension;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    return C->getIterableContextKind() 
             == IterableDeclContextKind::ExtensionDecl;
  }

  using DeclContext::operator new;
};

/// Iterator that walks the extensions of a particular type.
class ExtensionIterator {
  ExtensionDecl *current;

public:
  ExtensionIterator() : current() { }
  explicit ExtensionIterator(ExtensionDecl *current) : current(current) { }

  ExtensionDecl *operator*() const { return current; }
  ExtensionDecl *operator->() const { return current; }

  ExtensionIterator &operator++() {
    current = current->NextExtension.getPointer();
    return *this;
  }

  ExtensionIterator operator++(int) {
    ExtensionIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  friend bool operator==(ExtensionIterator x, ExtensionIterator y) {
    return x.current == y.current;
  }

  friend bool operator!=(ExtensionIterator x, ExtensionIterator y) {
    return x.current != y.current;
  }
};

/// Range that covers a set of extensions.
class ExtensionRange {
  ExtensionIterator first;
  ExtensionIterator last;

public:

  ExtensionRange(ExtensionIterator first, ExtensionIterator last)
    : first(first), last(last) { }

  typedef ExtensionIterator iterator;
  iterator begin() const { return first; }
  iterator end() const { return last; }
};
  
  
/// This represents one entry in a PatternBindingDecl, which are pairs of
/// Pattern and Initialization expression.  The pattern is always present, but
/// the initializer can be null if there is none.
class PatternBindingEntry {
  enum class Flags {
    Checked = 1 << 0,
    Removed = 1 << 1,
    /// Whether the contents of this initializer were subsumed by
    /// some other initialization, e.g., a lazy property's initializer
    /// gets subsumed by the getter body.
    Subsumed    = 1 << 2
  };
  llvm::PointerIntPair<Pattern *, 3, OptionSet<Flags>> PatternAndFlags;

  struct InitializerAndEqualLoc {
    // When the initializer is removed we don't actually clear the pointers
    // because we might need to get initializer's source range. Since the
    // initializer is ASTContext-allocated it is safe.
    
    /// Exactly the expr the programmer wrote
    Expr *originalInit;
    /// Might be transformed, e.g. for a property wrapper. In the absence of
    /// transformation or synthesis, holds the expr as parsed.
    Expr *initAfterSynthesis;
    /// The location of the equal '=' token.
    SourceLoc EqualLoc;
  };

  union {
    /// The initializer expression and its '=' token loc.
    InitializerAndEqualLoc InitExpr;

    /// The text of the initializer expression if deserialized from a module.
    StringRef InitStringRepresentation;
  };

  /// The initializer context used for this pattern binding entry.
  llvm::PointerIntPair<DeclContext *, 1, bool> InitContextAndIsText;

  /// Values captured by this initializer.
  CaptureInfo Captures;

  friend class PatternBindingInitializer;

public:
  /// \p E is the initializer as parsed.
  PatternBindingEntry(Pattern *P, SourceLoc EqualLoc, Expr *E,
                      DeclContext *InitContext)
    : PatternAndFlags(P, {}), InitExpr({E, E, EqualLoc}),
      InitContextAndIsText({InitContext, false}) {
  }

  Pattern *getPattern() const { return PatternAndFlags.getPointer(); }
  void setPattern(Pattern *P) { PatternAndFlags.setPointer(P); }

  /// Whether the given pattern binding entry is initialized.
  bool isInitialized() const;

  Expr *getInit() const {
    if (PatternAndFlags.getInt().contains(Flags::Removed) ||
        InitContextAndIsText.getInt())
      return nullptr;
    return InitExpr.initAfterSynthesis;
  }
  /// Retrieve the initializer if it should be executed to initialize this
  /// particular pattern binding.
  Expr *getExecutableInit() const {
    return isInitializerSubsumed() ? nullptr : getInit();
  }
  SourceRange getOriginalInitRange() const;
  void setInit(Expr *E);

  /// Gets the text of the initializer expression, stripping out inactive
  /// branches of any #ifs inside the expression.
  StringRef getInitStringRepresentation(SmallVectorImpl<char> &scratch) const;

  /// Sets the initializer string representation to the string that was
  /// deserialized from a partial module.
  void setInitStringRepresentation(StringRef str) {
    InitStringRepresentation = str;
    InitContextAndIsText.setInt(true);
  }

  /// Whether this pattern entry can generate a string representation of its
  /// initializer expression.
  bool hasInitStringRepresentation() const;

  /// Retrieve the location of the equal '=' token.
  SourceLoc getEqualLoc() const {
    return InitContextAndIsText.getInt() ? SourceLoc() : InitExpr.EqualLoc;
  }

  /// Set the location of the equal '=' token.
  void setEqualLoc(SourceLoc equalLoc) {
    assert(!InitContextAndIsText.getInt() &&
           "cannot set equal loc for textual initializer");
    InitExpr.EqualLoc = equalLoc;
  }

  /// Retrieve the initializer after the =, if any, as it was written in the
  /// source.
  Expr *getOriginalInit() const;

  /// Set the initializer after the = as it was written in the source.
  void setOriginalInit(Expr *);

  bool isInitializerChecked() const {
    return PatternAndFlags.getInt().contains(Flags::Checked);
  }
  void setInitializerChecked() {
    PatternAndFlags.setInt(PatternAndFlags.getInt() | Flags::Checked);
  }

  bool isInitializerSubsumed() const {
    return PatternAndFlags.getInt().contains(Flags::Subsumed);
  }
  void setInitializerSubsumed() {
    PatternAndFlags.setInt(PatternAndFlags.getInt() | Flags::Subsumed);
  }

  // Return the first variable initialized by this pattern.
  VarDecl *getAnchoringVarDecl() const;

  // Retrieve the declaration context for the initializer.
  DeclContext *getInitContext() const {
    return InitContextAndIsText.getPointer();
  }

  /// Override the initializer context.
  void setInitContext(DeclContext *dc) {
    InitContextAndIsText.setPointer(dc);
  }

  SourceLoc getStartLoc() const;

  /// Retrieve the end location covered by this pattern binding entry.
  ///
  /// \param omitAccessors Whether the computation should omit the accessors
  /// from the source range.
  SourceLoc getEndLoc(bool omitAccessors = false) const;

  /// Retrieve the source range covered by this pattern binding entry.
  ///
  /// \param omitAccessors Whether the computation should omit the accessors
  /// from the source range.
  SourceRange getSourceRange(bool omitAccessors = false) const;

  const CaptureInfo &getCaptureInfo() const { return Captures; }
  void setCaptureInfo(const CaptureInfo &captures) { Captures = captures; }

private:
  SourceLoc getLastAccessorEndLoc() const;
};

/// This decl contains a pattern and optional initializer for a set
/// of one or more VarDecls declared together.
///
/// For example, in
/// \code
///   var (a, b) = foo(), (c,d) = bar()
/// \endcode
///
/// this includes two entries in the pattern list.  The first contains the
/// pattern "(a, b)" and the initializer "foo()".  The second contains the
/// pattern "(c, d)" and the initializer "bar()".
///
class PatternBindingDecl final : public Decl,
    private llvm::TrailingObjects<PatternBindingDecl, PatternBindingEntry> {
  friend TrailingObjects;

  SourceLoc StaticLoc; ///< Location of the 'static/class' keyword, if present.
  SourceLoc VarLoc;    ///< Location of the 'var' keyword.

  friend class Decl;
  
  PatternBindingDecl(SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
                     SourceLoc VarLoc, unsigned NumPatternEntries,
                     DeclContext *Parent);

public:
  static PatternBindingDecl *create(ASTContext &Ctx, SourceLoc StaticLoc,
                                    StaticSpellingKind StaticSpelling,
                                    SourceLoc VarLoc,
                                    ArrayRef<PatternBindingEntry> PatternList,
                                    DeclContext *Parent);

  static PatternBindingDecl *create(ASTContext &Ctx, SourceLoc StaticLoc,
                                    StaticSpellingKind StaticSpelling,
                                    SourceLoc VarLoc, Pattern *Pat,
                                    SourceLoc EqualLoc, Expr *E,
                                    DeclContext *Parent);

  static PatternBindingDecl *createImplicit(ASTContext &Ctx,
                                            StaticSpellingKind StaticSpelling,
                                            Pattern *Pat, Expr *E,
                                            DeclContext *Parent,
                                            SourceLoc VarLoc = SourceLoc());

  static PatternBindingDecl *createDeserialized(
                               ASTContext &Ctx, SourceLoc StaticLoc,
                               StaticSpellingKind StaticSpelling,
                               SourceLoc VarLoc,
                               unsigned NumPatternEntries,
                               DeclContext *Parent);

  SourceLoc getStartLoc() const {
    return StaticLoc.isValid() ? StaticLoc : VarLoc;
  }
  SourceLoc getLoc() const { return VarLoc; }
  SourceRange getSourceRange() const;

  unsigned getNumPatternEntries() const {
    return Bits.PatternBindingDecl.NumPatternEntries;
  }
  
  ArrayRef<PatternBindingEntry> getPatternList() const {
    return const_cast<PatternBindingDecl*>(this)->getMutablePatternList();
  }

  void setInitStringRepresentation(unsigned i, StringRef str) {
    getMutablePatternList()[i].setInitStringRepresentation(str);
  }

  /// Whether the given pattern entry is initialized.
  bool isInitialized(unsigned i) const {
    return getPatternList()[i].isInitialized();
  }

  Expr *getInit(unsigned i) const {
    return getPatternList()[i].getInit();
  }
  Expr *getExecutableInit(unsigned i) const {
    return getPatternList()[i].getExecutableInit();
  }

  SourceRange getOriginalInitRange(unsigned i) const {
    return getPatternList()[i].getOriginalInitRange();
  }

  void setInit(unsigned i, Expr *E) {
    getMutablePatternList()[i].setInit(E);
  }

  Pattern *getPattern(unsigned i) const {
    return getPatternList()[i].getPattern();
  }
  
  void setPattern(unsigned i, Pattern *Pat, DeclContext *InitContext);

  DeclContext *getInitContext(unsigned i) const {
    return getPatternList()[i].getInitContext();
  }

  const CaptureInfo &getCaptureInfo(unsigned i) const {
    return getPatternList()[i].getCaptureInfo();
  }

  void setCaptureInfo(unsigned i, const CaptureInfo &captures) {
    getMutablePatternList()[i].setCaptureInfo(captures);
  }

  /// Given that this PBD is the parent pattern for the specified VarDecl,
  /// return the entry of the VarDecl in our PatternList.  For example, in:
  ///
  ///   let (a,b) = foo(), (c,d) = bar()
  ///
  /// "a" and "b" will have index 0, since they correspond to the first pattern,
  /// and "c" and "d" will have index 1 since they correspond to the second one.
  unsigned getPatternEntryIndexForVarDecl(const VarDecl *VD) const;
  
  /// Return the PatternEntry (a pattern + initializer pair) for the specified
  /// VarDecl.
  const PatternBindingEntry &getPatternEntryForVarDecl(
    const VarDecl *VD) const {
    return getPatternList()[getPatternEntryIndexForVarDecl(VD)];
  }
  
  bool isInitializerChecked(unsigned i) const {
    return getPatternList()[i].isInitializerChecked();
  }

  void setInitializerChecked(unsigned i) {
    getMutablePatternList()[i].setInitializerChecked();
  }

  bool isInitializerSubsumed(unsigned i) const {
    return getPatternList()[i].isInitializerSubsumed();
  }

  void setInitializerSubsumed(unsigned i) {
    getMutablePatternList()[i].setInitializerSubsumed();
  }
  
  /// Does this binding declare something that requires storage?
  bool hasStorage() const;

  /// Determines whether this binding either has an initializer expression, or is
  /// default initialized, without performing any type checking on it.
  bool isDefaultInitializable() const {
    for (unsigned i = 0, e = getNumPatternEntries(); i < e; ++i)
      if (!isDefaultInitializable(i))
        return false;

    return true;
  }

  /// Can the pattern at index i be default initialized?
  bool isDefaultInitializable(unsigned i) const;
  
  /// When the pattern binding contains only a single variable with no
  /// destructuring, retrieve that variable.
  VarDecl *getSingleVar() const;

  bool isStatic() const { return Bits.PatternBindingDecl.IsStatic; }
  void setStatic(bool s) { Bits.PatternBindingDecl.IsStatic = s; }
  SourceLoc getStaticLoc() const { return StaticLoc; }
  /// \returns the way 'static'/'class' was spelled in the source.
  StaticSpellingKind getStaticSpelling() const {
    return static_cast<StaticSpellingKind>(
        Bits.PatternBindingDecl.StaticSpelling);
  }
  /// \returns the way 'static'/'class' should be spelled for this declaration.
  StaticSpellingKind getCorrectStaticSpelling() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PatternBinding;
  }

private:
  MutableArrayRef<PatternBindingEntry> getMutablePatternList() {
    // Pattern entries are tail allocated.
    return {getTrailingObjects<PatternBindingEntry>(), getNumPatternEntries()};
  }
};
  
/// TopLevelCodeDecl - This decl is used as a container for top-level
/// expressions and statements in the main module.  It is always a direct
/// child of a SourceFile.  The primary reason for building these is to give
/// top-level statements a DeclContext which is distinct from the file itself.
/// This, among other things, makes it easier to distinguish between local
/// top-level variables (which are not live past the end of the statement) and
/// global variables.
class TopLevelCodeDecl : public DeclContext, public Decl {
  BraceStmt *Body;

public:
  TopLevelCodeDecl(DeclContext *Parent, BraceStmt *Body = nullptr)
    : DeclContext(DeclContextKind::TopLevelCodeDecl, Parent),
      Decl(DeclKind::TopLevelCode, Parent),
      Body(Body) {}

  BraceStmt *getBody() const { return Body; }
  void setBody(BraceStmt *b) { Body = b; }

  SourceLoc getStartLoc() const;
  SourceLoc getLoc() const { return getStartLoc(); }
  SourceRange getSourceRange() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::TopLevelCode;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  
  using DeclContext::operator new;
};

/// SerializedTopLevelCodeDeclContext - This represents what was originally a
/// TopLevelCodeDecl during serialization. It is preserved only to maintain the
/// correct AST structure and remangling after deserialization.
class SerializedTopLevelCodeDeclContext : public SerializedLocalDeclContext {
public:
  SerializedTopLevelCodeDeclContext(DeclContext *Parent)
    : SerializedLocalDeclContext(LocalDeclContextKind::TopLevelCodeDecl,
                                 Parent) {}
  static bool classof(const DeclContext *DC) {
    if (auto LDC = dyn_cast<SerializedLocalDeclContext>(DC))
      return LDC->getLocalDeclContextKind() ==
        LocalDeclContextKind::TopLevelCodeDecl;
    return false;
  }
};

/// IfConfigDecl - This class represents #if/#else/#endif blocks.
/// Active and inactive block members are stored separately, with the intention
/// being that active members will be handed back to the enclosing context.
class IfConfigDecl : public Decl {
  /// An array of clauses controlling each of the #if/#elseif/#else conditions.
  /// The array is ASTContext allocated.
  ArrayRef<IfConfigClause> Clauses;
  SourceLoc EndLoc;
public:
  
  IfConfigDecl(DeclContext *Parent, ArrayRef<IfConfigClause> Clauses,
               SourceLoc EndLoc, bool HadMissingEnd)
    : Decl(DeclKind::IfConfig, Parent), Clauses(Clauses), EndLoc(EndLoc)
  {
    Bits.IfConfigDecl.HadMissingEnd = HadMissingEnd;
  }

  ArrayRef<IfConfigClause> getClauses() const { return Clauses; }

  /// Return the active clause, or null if there is no active one.
  const IfConfigClause *getActiveClause() const {
    for (auto &Clause : Clauses)
      if (Clause.isActive) return &Clause;
    return nullptr;
  }

  const ArrayRef<ASTNode> getActiveClauseElements() const {
    if (auto *Clause = getActiveClause())
      return Clause->Elements;
    return {};
  }
  
  SourceLoc getEndLoc() const { return EndLoc; }
  SourceLoc getLoc() const { return Clauses[0].Loc; }

  bool hadMissingEnd() const { return Bits.IfConfigDecl.HadMissingEnd; }
  
  SourceRange getSourceRange() const;
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::IfConfig;
  }
};

class StringLiteralExpr;

class PoundDiagnosticDecl : public Decl {
  SourceLoc StartLoc;
  SourceLoc EndLoc;
  StringLiteralExpr *Message;

public:
  PoundDiagnosticDecl(DeclContext *Parent, bool IsError, SourceLoc StartLoc,
                      SourceLoc EndLoc, StringLiteralExpr *Message)
    : Decl(DeclKind::PoundDiagnostic, Parent), StartLoc(StartLoc),
      EndLoc(EndLoc), Message(Message) {
      Bits.PoundDiagnosticDecl.IsError = IsError;
      Bits.PoundDiagnosticDecl.HasBeenEmitted = false; 
    }

  DiagnosticKind getKind() {
    return isError() ? DiagnosticKind::Error : DiagnosticKind::Warning;
  }

  StringLiteralExpr *getMessage() { return Message; }

  bool isError() {
    return Bits.PoundDiagnosticDecl.IsError;
  }

  bool hasBeenEmitted() {
    return Bits.PoundDiagnosticDecl.HasBeenEmitted;
  }

  void markEmitted() {
    Bits.PoundDiagnosticDecl.HasBeenEmitted = true;
  }
  
  SourceLoc getEndLoc() const { return EndLoc; };
  SourceLoc getLoc() const { return StartLoc; }
  
  SourceRange getSourceRange() const {
    return SourceRange(StartLoc, EndLoc);
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PoundDiagnostic;
  }
};
  
class OpaqueTypeDecl;

/// ValueDecl - All named decls that are values in the language.  These can
/// have a type, etc.
class ValueDecl : public Decl {
  DeclName Name;
  SourceLoc NameLoc;
  llvm::PointerIntPair<Type, 3, OptionalEnum<AccessLevel>> TypeAndAccess;
  unsigned LocalDiscriminator = 0;

  struct {
    /// Whether the "IsObjC" bit has been computed yet.
    unsigned isObjCComputed : 1;

    /// Whether this declaration is exposed to Objective-C.
    unsigned isObjC : 1;

    /// Whether the "overridden" declarations have been computed already.
    unsigned hasOverriddenComputed : 1;

    /// Whether there are any "overridden" declarations. The actual overridden
    /// declarations are kept in a side table in the ASTContext.
    unsigned hasOverridden : 1;

    /// Whether the "isDynamic" bit has been computed yet.
    unsigned isDynamicComputed : 1;

    /// Whether this declaration is 'dynamic', meaning that all uses of
    /// the declaration will go through an extra level of indirection that
    /// allows the entity to be replaced at runtime.
    unsigned isDynamic : 1;

    /// Whether the "isFinal" bit has been computed yet.
    unsigned isFinalComputed : 1;

    /// Whether this declaration is 'final'. A final class can't be subclassed,
    /// a final class member can't be overriden.
    unsigned isFinal : 1;
  } LazySemanticInfo = { };

  friend class OverriddenDeclsRequest;
  friend class IsObjCRequest;
  friend class IsFinalRequest;
  friend class IsDynamicRequest;

protected:
  ValueDecl(DeclKind K,
            llvm::PointerUnion<DeclContext *, ASTContext *> context,
            DeclName name, SourceLoc NameLoc)
    : Decl(K, context), Name(name), NameLoc(NameLoc) {
    Bits.ValueDecl.AlreadyInLookupTable = false;
    Bits.ValueDecl.CheckedRedeclaration = false;
    Bits.ValueDecl.IsUserAccessible = true;
  }

  // MemberLookupTable borrows a bit from this type
  friend class MemberLookupTable;
  bool isAlreadyInLookupTable() {
    return Bits.ValueDecl.AlreadyInLookupTable;
  }
  void setAlreadyInLookupTable(bool value = true) {
    Bits.ValueDecl.AlreadyInLookupTable = value;
  }

public:
  /// Return true if this protocol member is a protocol requirement.
  ///
  /// Asserts if this is not a member of a protocol.
  bool isProtocolRequirement() const;

  /// Determine whether we have already checked whether this
  /// declaration is a redeclaration.
  bool alreadyCheckedRedeclaration() const { 
    return Bits.ValueDecl.CheckedRedeclaration;
  }

  /// Set whether we have already checked this declaration as a
  /// redeclaration.
  void setCheckedRedeclaration(bool checked) {
    Bits.ValueDecl.CheckedRedeclaration = checked;
  }

  void setUserAccessible(bool Accessible) {
    Bits.ValueDecl.IsUserAccessible = Accessible;
  }

  bool isUserAccessible() const {
    return Bits.ValueDecl.IsUserAccessible;
  }

  bool hasName() const { return bool(Name); }
  bool isOperator() const { return Name.isOperator(); }

  /// Retrieve the full name of the declaration.
  /// TODO: Rename to getName?
  DeclName getFullName() const { return Name; }
  void setName(DeclName name) { Name = name; }

  /// Retrieve the base name of the declaration, ignoring any argument
  /// names.
  DeclBaseName getBaseName() const { return Name.getBaseName(); }

  /// Retrieve the name to use for this declaration when interoperating
  /// with the Objective-C runtime.
  ///
  /// \returns A "selector" containing the runtime name. For non-method
  /// entities (classes, protocols, properties), this operation will
  /// return a zero-parameter selector with the appropriate name in its
  /// first slot.
  Optional<ObjCSelector> getObjCRuntimeName(
                                    bool skipIsObjCResolution = false) const;

  /// Determine whether the given declaration can infer @objc, or the
  /// Objective-C name, if used to satisfy the given requirement.
  bool canInferObjCFromRequirement(ValueDecl *requirement);

  SourceLoc getNameLoc() const { return NameLoc; }
  SourceLoc getLoc() const { return NameLoc; }

  bool isUsableFromInline() const;

  /// Returns \c true if this declaration is *not* intended to be used directly
  /// by application developers despite the visibility.
  bool shouldHideFromEditor() const;

  bool hasAccess() const {
    return TypeAndAccess.getInt().hasValue();
  }

  /// Access control is done by Requests.
  friend class AccessLevelRequest;

  /// Returns the access level specified explicitly by the user, or provided by
  /// default according to language rules.
  ///
  /// Most of the time this is not the interesting value to check; access is
  /// limited by enclosing scopes per SE-0025. Use #getFormalAccessScope to
  /// check if access control is being used consistently, and to take features
  /// such as \c \@testable and \c \@usableFromInline into account.
  ///
  /// \sa getFormalAccessScope
  /// \sa hasOpenAccess
  AccessLevel getFormalAccess() const;

  /// Determine whether this Decl has either Private or FilePrivate access,
  /// and its DeclContext does not.
  bool isOutermostPrivateOrFilePrivateScope() const;

  /// Returns the outermost DeclContext from which this declaration can be
  /// accessed, or null if the declaration is public.
  ///
  /// This is used when calculating if access control is being used
  /// consistently. If \p useDC is provided (the location where the value is
  /// being used), features that affect formal access such as \c \@testable are
  /// taken into account.
  ///
  /// \invariant
  /// <code>value.isAccessibleFrom(
  ///     value.getFormalAccessScope().getDeclContext())</code>
  ///
  /// If \p treatUsableFromInlineAsPublic is true, declarations marked with the
  /// \c @usableFromInline attribute are treated as public. This is normally
  /// false for name lookup and other source language concerns, but true when
  /// computing the linkage of generated functions.
  ///
  /// \sa getFormalAccess
  /// \sa isAccessibleFrom
  /// \sa hasOpenAccess
  AccessScope
  getFormalAccessScope(const DeclContext *useDC = nullptr,
                       bool treatUsableFromInlineAsPublic = false) const;


  /// Copy the formal access level and @usableFromInline attribute from
  /// \p source.
  ///
  /// If \p sourceIsParentContext is true, an access level of \c private will
  /// be copied as \c fileprivate, to ensure that this declaration will be
  /// available everywhere \p source is.
  void copyFormalAccessFrom(const ValueDecl *source,
                            bool sourceIsParentContext = false);

  /// Returns the access level that actually controls how a declaration should
  /// be emitted and may be used.
  ///
  /// This is the access used when making optimization and code generation
  /// decisions. It should not be used at the AST or semantic level.
  AccessLevel getEffectiveAccess() const;

  void setAccess(AccessLevel access) {
    assert(!hasAccess() && "access already set");
    overwriteAccess(access);
  }

  /// Overwrite the access of this declaration.
  ///
  /// This is needed in the LLDB REPL.
  void overwriteAccess(AccessLevel access) {
    TypeAndAccess.setInt(access);
  }

  /// Returns true if this declaration is accessible from the given context.
  ///
  /// A private declaration is accessible from any DeclContext within the same
  /// source file.
  ///
  /// An internal declaration is accessible from any DeclContext within the same
  /// module.
  ///
  /// A public declaration is accessible everywhere.
  ///
  /// If \p DC is null, returns true only if this declaration is public.
  ///
  /// If \p forConformance is true, we ignore the visibility of the protocol
  /// when evaluating protocol extension members. This language rule allows a
  /// protocol extension of a private protocol to provide default
  /// implementations for the requirements of a public protocol, even when
  /// the default implementations are not visible to name lookup.
  bool isAccessibleFrom(const DeclContext *DC,
                        bool forConformance = false) const;

  /// Returns whether this declaration should be treated as \c open from
  /// \p useDC. This is very similar to #getFormalAccess, but takes
  /// \c \@testable into account.
  ///
  /// This is mostly only useful when considering requirements on an override:
  /// if the base declaration is \c open, the override might have to be too.
  bool hasOpenAccess(const DeclContext *useDC) const;

  /// Retrieve the "interface" type of this value, which uses
  /// GenericTypeParamType if the declaration is generic. For a generic
  /// function, this will have a GenericFunctionType with a
  /// GenericSignature inside the type.
  Type getInterfaceType() const;
  bool hasInterfaceType() const;

  /// Set the interface type for the given value.
  void setInterfaceType(Type type);

  bool hasValidSignature() const;
  
  /// isInstanceMember - Determine whether this value is an instance member
  /// of an enum or protocol.
  bool isInstanceMember() const;

  /// Retrieve the context discriminator for this local value, which
  /// is the index of this declaration in the sequence of
  /// discriminated declarations with the same name in the current
  /// context.  Only local functions and variables with getters and
  /// setters have discriminators.
  unsigned getLocalDiscriminator() const;
  void setLocalDiscriminator(unsigned index);

  /// Retrieve the declaration that this declaration overrides, if any.
  ValueDecl *getOverriddenDecl() const;

  /// Retrieve the declarations that this declaration overrides, if any.
  llvm::TinyPtrVector<ValueDecl *> getOverriddenDecls() const;

  /// Set the declaration that this declaration overrides.
  void setOverriddenDecl(ValueDecl *overridden) {
    setOverriddenDecls(overridden);
  }

  /// Set the declarations that this declaration overrides.
  void setOverriddenDecls(ArrayRef<ValueDecl *> overridden);

  /// Whether the overridden declarations have already been computed.
  bool overriddenDeclsComputed() const;

  /// Compute the untyped overload signature for this declaration.
  OverloadSignature getOverloadSignature() const;

  /// Retrieve the type used to describe this entity for the purposes of
  /// overload resolution.
  CanType getOverloadSignatureType() const;

  /// Returns true if the decl requires Objective-C interop.
  ///
  /// This can be true even if there is no 'objc' attribute on the declaration.
  /// In that case it was inferred by the type checker and set with a call to
  /// markAsObjC().
  bool isObjC() const;

  /// Note whether this declaration is known to be exposed to Objective-C.
  void setIsObjC(bool Value);

  /// Is this declaration 'final'?
  bool isFinal() const;

  /// Is this declaration marked with 'dynamic'?
  bool isDynamic() const;

  bool isObjCDynamic() const {
    return isObjC() && isDynamic();
  }

  bool isNativeDynamic() const {
    return !isObjC() && isDynamic();
  }

  /// Set whether this type is 'dynamic' or not.
  void setIsDynamic(bool value);

  /// Whether the 'dynamic' bit has been computed already.
  bool isDynamicComputed() const {
    return LazySemanticInfo.isDynamicComputed;
  }

  /// Returns true if this decl can be found by id-style dynamic lookup.
  bool canBeAccessedByDynamicLookup() const;

  /// Returns the protocol requirements that this decl conforms to.
  ArrayRef<ValueDecl *>
  getSatisfiedProtocolRequirements(bool Sorted = false) const;

  /// Determines the kind of access that should be performed by a
  /// DeclRefExpr or MemberRefExpr use of this value in the specified
  /// context.
  ///
  /// \param DC The declaration context.
  ///
  /// \param isAccessOnSelf Whether this is a member access on the implicit
  ///        'self' declaration of the declaration context.
  AccessSemantics getAccessSemanticsFromContext(const DeclContext *DC,
                                                bool isAccessOnSelf) const;

  /// Print a reference to the given declaration.
  std::string printRef() const;

  /// Dump a reference to the given declaration.
  void dumpRef(raw_ostream &os) const;

  /// Dump a reference to the given declaration.
  void dumpRef() const;

  /// Returns true if the declaration is a static member of a type.
  ///
  /// This is not necessarily the opposite of "isInstanceMember()". Both
  /// predicates will be false for declarations that either categorically
  /// can't be "static" or are in a context where "static" doesn't make sense.
  bool isStatic() const;

  /// Retrieve the location at which we should insert a new attribute or
  /// modifier.
  SourceLoc getAttributeInsertionLoc(bool forModifier) const;

  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_ValueDecl &&
           D->getKind() <= DeclKind::Last_ValueDecl;
  }
  
  /// True if this is a C function that was imported as a member of a type in
  /// Swift.
  bool isImportAsMember() const;

  /// Returns true if the declaration's interface type is a function type with a
  /// curried self parameter.
  bool hasCurriedSelf() const;

  /// Returns true if the declaration has a parameter list associated with it.
  ///
  /// Note that not all declarations with function interface types have
  /// parameter lists, for example an enum element without associated values.
  bool hasParameterList() const;

  /// Returns the number of curry levels in the declaration's interface type.
  unsigned getNumCurryLevels() const;

  /// Get the decl for this value's opaque result type, if it has one.
  OpaqueTypeDecl *getOpaqueResultTypeDecl() const;

  /// Get the representative for this value's opaque result type, if it has one.
  OpaqueReturnTypeRepr *getOpaqueResultTypeRepr() const;

  /// Set the opaque return type decl for this decl.
  ///
  /// `this` must be of a decl type that supports opaque return types, and
  /// must not have previously had an opaque result type set.
  void setOpaqueResultTypeDecl(OpaqueTypeDecl *D);

  /// Retrieve the attribute associating this declaration with a
  /// function builder, if there is one.
  CustomAttr *getAttachedFunctionBuilder() const;

  /// Retrieve the @functionBuilder type attached to this declaration,
  /// if there is one.
  Type getFunctionBuilderType() const;
};

/// This is a common base class for declarations which declare a type.
class TypeDecl : public ValueDecl {
  MutableArrayRef<TypeLoc> Inherited;

protected:
  TypeDecl(DeclKind K, llvm::PointerUnion<DeclContext *, ASTContext *> context,
           Identifier name, SourceLoc NameLoc,
           MutableArrayRef<TypeLoc> inherited) :
    ValueDecl(K, context, name, NameLoc), Inherited(inherited) {}

public:
  Identifier getName() const { return getFullName().getBaseIdentifier(); }

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    assert(!getFullName().isSpecial() && "Cannot get string for special names");
    return hasName() ? getBaseName().getIdentifier().str() : "_";
  }

  /// The type of this declaration's values. For the type of the
  /// declaration itself, use getInterfaceType(), which returns a
  /// metatype.
  Type getDeclaredInterfaceType() const;

  /// Retrieve the set of protocols that this type inherits (i.e,
  /// explicitly conforms to).
  MutableArrayRef<TypeLoc> getInherited() { return Inherited; }
  ArrayRef<TypeLoc> getInherited() const { return Inherited; }

  void setInherited(MutableArrayRef<TypeLoc> i) { Inherited = i; }

  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_TypeDecl &&
           D->getKind() <= DeclKind::Last_TypeDecl;
  }

  /// Compute an ordering between two type declarations that is ABI-stable.
  static int compare(const TypeDecl *type1, const TypeDecl *type2);

  /// Compute an ordering between two type declarations that is ABI-stable.
  /// This version takes a pointer-to-a-pointer for use with
  /// llvm::array_pod_sort() and similar.
  template<typename T>
  static int compare(T * const* type1, T * const* type2) {
    return compare(*type1, *type2);
  }
};

/// A type declaration that can have generic parameters attached to it. Because
/// it has these generic parameters, it is always a DeclContext.
class GenericTypeDecl : public GenericContext, public TypeDecl {
public:
  GenericTypeDecl(DeclKind K, DeclContext *DC,
                  Identifier name, SourceLoc nameLoc,
                  MutableArrayRef<TypeLoc> inherited,
                  GenericParamList *GenericParams);

  // Resolve ambiguity due to multiple base classes.
  using TypeDecl::getASTContext;
  using DeclContext::operator new;
  using TypeDecl::getDeclaredInterfaceType;

  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_GenericTypeDecl &&
           D->getKind() <= DeclKind::Last_GenericTypeDecl;
  }
};

/// OpaqueTypeDecl - This is a declaration of an opaque type. The opaque type
/// is formally equivalent to its underlying type, but abstracts it away from
/// clients of the opaque type, only exposing the type as something conforming
/// to a given set of constraints.
///
/// Currently, opaque types do not normally have an explicit spelling in source
/// code. One is formed implicitly when a declaration is written with an opaque
/// result type, as in:
///
/// func foo() -> opaque SignedInteger { return 1 }
///
/// The declared type is a special kind of ArchetypeType representing the
/// abstracted underlying type.
class OpaqueTypeDecl : public GenericTypeDecl {
  /// The original declaration that "names" the opaque type. Although a specific
  /// opaque type cannot be explicitly named, oapque types can propagate
  /// arbitrarily through expressions, so we need to know *which* opaque type is
  /// propagated.
  ValueDecl *NamingDecl;
  
  /// The generic signature of the opaque interface to the type. This is the
  /// outer generic signature with an added generic parameter representing the
  /// underlying type.
  GenericSignature *OpaqueInterfaceGenericSignature;
  
  /// The generic parameter that represents the underlying type.
  GenericTypeParamType *UnderlyingInterfaceType;
  
  /// If known, the underlying type and conformances of the opaque type,
  /// expressed as a SubstitutionMap for the opaque interface generic signature.
  /// This maps types in the interface generic signature to the outer generic
  /// signature of the original declaration.
  Optional<SubstitutionMap> UnderlyingTypeSubstitutions;
  
  mutable Identifier OpaqueReturnTypeIdentifier;
  
public:
  OpaqueTypeDecl(ValueDecl *NamingDecl,
                 GenericParamList *GenericParams,
                 DeclContext *DC,
                 GenericSignature *OpaqueInterfaceGenericSignature,
                 GenericTypeParamType *UnderlyingInterfaceType);
  
  ValueDecl *getNamingDecl() const { return NamingDecl; }
  
  void setNamingDecl(ValueDecl *D) {
    assert(!NamingDecl && "already have naming decl");
    NamingDecl = D;
  }

  /// Is this opaque type the opaque return type of the given function?
  ///
  /// This is more complex than just checking `getNamingDecl` because the
  /// function could also be the getter of a storage declaration.
  bool isOpaqueReturnTypeOfFunction(const AbstractFunctionDecl *func) const;

  GenericSignature *getOpaqueInterfaceGenericSignature() const {
    return OpaqueInterfaceGenericSignature;
  }
  
  GenericTypeParamType *getUnderlyingInterfaceType() const {
    return UnderlyingInterfaceType;
  }
  
  Optional<SubstitutionMap> getUnderlyingTypeSubstitutions() const {
    return UnderlyingTypeSubstitutions;
  }
  
  void setUnderlyingTypeSubstitutions(SubstitutionMap subs) {
    assert(!UnderlyingTypeSubstitutions.hasValue() && "resetting underlying type?!");
    UnderlyingTypeSubstitutions = subs;
  }
  
  // Opaque type decls are currently always implicit
  SourceRange getSourceRange() const { return SourceRange(); }
  
  // Get the identifier string that can be used to cross-reference unnamed
  // opaque return types across files.
  Identifier getOpaqueReturnTypeIdentifier() const;
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::OpaqueType;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::OpaqueType;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
};

/// TypeAliasDecl - This is a declaration of a typealias, for example:
///
///    typealias Foo = Int
///
/// TypeAliasDecl's always have 'MetatypeType' type.
///
class TypeAliasDecl : public GenericTypeDecl {
  /// The location of the 'typealias' keyword
  SourceLoc TypeAliasLoc;

  /// The location of the equal '=' token
  SourceLoc EqualLoc;

  /// The end of the type, valid even when the type cannot be parsed
  SourceLoc TypeEndLoc;

  /// The location of the right-hand side of the typealias binding
  TypeLoc UnderlyingTy;

public:
  TypeAliasDecl(SourceLoc TypeAliasLoc, SourceLoc EqualLoc, Identifier Name,
                SourceLoc NameLoc, GenericParamList *GenericParams,
                DeclContext *DC);

  SourceLoc getStartLoc() const { return TypeAliasLoc; }
  SourceRange getSourceRange() const;

  /// Returns the location of the equal '=' token
  SourceLoc getEqualLoc() const {
    return EqualLoc;
  }

  void setTypeEndLoc(SourceLoc e) { TypeEndLoc = e; }

  TypeLoc &getUnderlyingTypeLoc() {
    return UnderlyingTy;
  }
  const TypeLoc &getUnderlyingTypeLoc() const {
    return UnderlyingTy;
  }

  void setUnderlyingType(Type type);

  /// For generic typealiases, return the unbound generic type.
  UnboundGenericType *getUnboundGenericType() const;

  Type getStructuralType() const;

  bool isCompatibilityAlias() const {
    return Bits.TypeAliasDecl.IsCompatibilityAlias;
  }

  void markAsCompatibilityAlias(bool newValue = true) {
    Bits.TypeAliasDecl.IsCompatibilityAlias = newValue;
  }

  /// Is this a special debugger variable?
  bool isDebuggerAlias() const { return Bits.TypeAliasDecl.IsDebuggerAlias; }
  void markAsDebuggerAlias(bool isDebuggerAlias) {
    Bits.TypeAliasDecl.IsDebuggerAlias = isDebuggerAlias;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::TypeAlias;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::TypeAlias;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
};

/// Abstract class describing generic type parameters and associated types,
/// whose common purpose is to anchor the abstract type parameter and specify
/// requirements for any corresponding type argument.
class AbstractTypeParamDecl : public TypeDecl {
protected:
  AbstractTypeParamDecl(DeclKind kind, DeclContext *dc, Identifier name,
                        SourceLoc NameLoc)
    : TypeDecl(kind, dc, name, NameLoc, { }) { }

public:
  /// Return the superclass of the generic parameter.
  Type getSuperclass() const;

  /// Retrieve the set of protocols to which this abstract type
  /// parameter conforms.
  ArrayRef<ProtocolDecl *> getConformingProtocols() const;

  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_AbstractTypeParamDecl &&
           D->getKind() <= DeclKind::Last_AbstractTypeParamDecl;
  }
};

/// A declaration of a generic type parameter.
///
/// A generic type parameter introduces a new, named type parameter along
/// with some set of requirements on any type argument used to realize this
/// type parameter. The requirements involve conformances to specific
/// protocols or inheritance from a specific class type.
///
/// In the following example, 'T' is a generic type parameter with the
/// requirement that the type argument conform to the 'Comparable' protocol.
///
/// \code
/// func min<T : Comparable>(x : T, y : T) -> T { ... }
/// \endcode
class GenericTypeParamDecl : public AbstractTypeParamDecl {
public:
  static const unsigned InvalidDepth = 0xFFFF;

  /// Construct a new generic type parameter.
  ///
  /// \param dc The DeclContext in which the generic type parameter's owner
  /// occurs. This should later be overwritten with the actual declaration
  /// context that owns the type parameter.
  ///
  /// \param name The name of the generic parameter.
  /// \param nameLoc The location of the name.
  GenericTypeParamDecl(DeclContext *dc, Identifier name, SourceLoc nameLoc,
                       unsigned depth, unsigned index);

  /// The depth of this generic type parameter, i.e., the number of outer
  /// levels of generic parameter lists that enclose this type parameter.
  ///
  /// \code
  /// struct X<T> {
  ///   func f<U>() { }
  /// }
  /// \endcode
  ///
  /// Here 'T' has depth 0 and 'U' has depth 1. Both have index 0.
  unsigned getDepth() const { return Bits.GenericTypeParamDecl.Depth; }

  /// Set the depth of this generic type parameter.
  ///
  /// \sa getDepth
  void setDepth(unsigned depth) {
    Bits.GenericTypeParamDecl.Depth = depth;
    assert(Bits.GenericTypeParamDecl.Depth == depth && "Truncation");
  }

  /// The index of this generic type parameter within its generic parameter
  /// list.
  ///
  /// \code
  /// struct X<T, U> {
  ///   func f<V>() { }
  /// }
  /// \endcode
  ///
  /// Here 'T' and 'U' have indexes 0 and 1, respectively. 'V' has index 0.
  unsigned getIndex() const { return Bits.GenericTypeParamDecl.Index; }

  SourceLoc getStartLoc() const { return getNameLoc(); }
  SourceRange getSourceRange() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::GenericTypeParam;
  }
};

/// A declaration of an associated type.
///
/// An associated type introduces a new, named type in a protocol that
/// can vary from one conforming type to the next. Associated types have a
/// set of requirements to which the type that replaces it much realize,
/// described via conformance to specific protocols, or inheritance from a
/// specific class type.
///
/// In the following example, 'Element' is an associated type with no
/// requirements.
///
/// \code
/// protocol Enumerator {
///   typealias Element
///   func getNext() -> Element?
/// }
/// \endcode
class AssociatedTypeDecl : public AbstractTypeParamDecl {
  /// The location of the initial keyword.
  SourceLoc KeywordLoc;

  /// The default definition.
  TypeRepr *DefaultDefinition;

  /// The where clause attached to the associated type.
  TrailingWhereClause *TrailingWhere;

  LazyMemberLoader *Resolver = nullptr;
  uint64_t ResolverContextData;

  friend class DefaultDefinitionTypeRequest;

public:
  AssociatedTypeDecl(DeclContext *dc, SourceLoc keywordLoc, Identifier name,
                     SourceLoc nameLoc, TypeRepr *defaultDefinition,
                     TrailingWhereClause *trailingWhere);
  AssociatedTypeDecl(DeclContext *dc, SourceLoc keywordLoc, Identifier name,
                     SourceLoc nameLoc, TrailingWhereClause *trailingWhere,
                     LazyMemberLoader *definitionResolver,
                     uint64_t resolverData);

  /// Get the protocol in which this associated type is declared.
  ProtocolDecl *getProtocol() const {
    return cast<ProtocolDecl>(getDeclContext());
  }

  /// Check if we have a default definition type.
  bool hasDefaultDefinitionType() const {
    // If we have a TypeRepr, return true immediately without kicking off
    // a request.
    return DefaultDefinition || getDefaultDefinitionType();
  }

  /// Retrieve the default definition type.
  Type getDefaultDefinitionType() const;

  /// Retrieve the default definition as written in the source.
  TypeRepr *getDefaultDefinitionTypeRepr() const {
    return DefaultDefinition;
  }

  /// Retrieve the trailing where clause for this associated type, if any.
  TrailingWhereClause *getTrailingWhereClause() const { return TrailingWhere; }

  /// Set the trailing where clause for this associated type.
  void setTrailingWhereClause(TrailingWhereClause *trailingWhereClause) {
    TrailingWhere = trailingWhereClause;
  }

  /// Set the interface type of this associated type declaration to a dependent
  /// member type of 'Self'.
  void computeType();

  /// Retrieve the associated type "anchor", which is the associated type
  /// declaration that will be used to describe this associated type in the
  /// ABI.
  ///
  /// The associated type "anchor" is an associated type that does not
  /// override any other associated type. There may be several such associated
  /// types; select one deterministically.
  AssociatedTypeDecl *getAssociatedTypeAnchor() const;

  /// Retrieve the (first) overridden associated type declaration, if any.
  AssociatedTypeDecl *getOverriddenDecl() const {
    return cast_or_null<AssociatedTypeDecl>(
                                   AbstractTypeParamDecl::getOverriddenDecl());
  }

  /// Retrieve the set of associated types overridden by this associated
  /// type.
  llvm::TinyPtrVector<AssociatedTypeDecl *> getOverriddenDecls() const;

  SourceLoc getStartLoc() const { return KeywordLoc; }
  SourceRange getSourceRange() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::AssociatedType;
  }
};

class MemberLookupTable;
class ConformanceLookupTable;

/// Kinds of optional types.
enum OptionalTypeKind : unsigned {
  /// The type is not an optional type.
  OTK_None = 0,

  /// The type is Optional<T>.
  OTK_Optional,

  /// The type is ImplicitlyUnwrappedOptional<T>.
  OTK_ImplicitlyUnwrappedOptional
};
enum { NumOptionalTypeKinds = 2 };
  
// Kinds of pointer types.
enum PointerTypeKind : unsigned {
  PTK_UnsafeMutableRawPointer,
  PTK_UnsafeRawPointer,
  PTK_UnsafeMutablePointer,
  PTK_UnsafePointer,
  PTK_AutoreleasingUnsafeMutablePointer,
};

static inline bool isRawPointerKind(PointerTypeKind PTK) {
  switch (PTK) {
  case PTK_UnsafeMutableRawPointer:
  case PTK_UnsafeRawPointer:
    return true;
  case PTK_UnsafeMutablePointer:
  case PTK_UnsafePointer:
  case PTK_AutoreleasingUnsafeMutablePointer:
    return false;
  }

  llvm_unreachable("Unhandled PointerTypeKind in switch.");
}

enum KeyPathTypeKind : unsigned char {
  KPTK_AnyKeyPath,
  KPTK_PartialKeyPath,
  KPTK_KeyPath,
  KPTK_WritableKeyPath,
  KPTK_ReferenceWritableKeyPath
};

/// NominalTypeDecl - a declaration of a nominal type, like a struct.
class NominalTypeDecl : public GenericTypeDecl, public IterableDeclContext {
  SourceRange Braces;

  /// The first extension of this type.
  ExtensionDecl *FirstExtension = nullptr;

  /// The last extension of this type, used solely for efficient
  /// insertion of new extensions.
  ExtensionDecl *LastExtension = nullptr;

  /// The generation at which we last loaded extensions.
  unsigned ExtensionGeneration;
  
  /// Prepare to traverse the list of extensions.
  void prepareExtensions();

  /// Retrieve the conformance loader (if any), and removing it in the
  /// same operation. The caller is responsible for loading the
  /// conformances.
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoader() {
    if (!Bits.NominalTypeDecl.HasLazyConformances)
      return { nullptr, 0 };

    return takeConformanceLoaderSlow();
  }

  /// Slow path for \c takeConformanceLoader().
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoaderSlow();

  /// A lookup table containing all of the members of this type and
  /// its extensions.
  ///
  /// The table itself is lazily constructed and updated when
  /// lookupDirect() is called. The bit indicates whether the lookup
  /// table has already added members by walking the declarations in
  /// scope; it should be manipulated through \c isLookupTablePopulated()
  /// and \c setLookupTablePopulated().
  llvm::PointerIntPair<MemberLookupTable *, 1, bool> LookupTable;

  /// Prepare the lookup table to make it ready for lookups.
  void prepareLookupTable(bool ignoreNewExtensions);

  /// True if the entries in \c LookupTable are complete--that is, if a
  /// name is present, it contains all members with that name.
  bool isLookupTablePopulated() const;
  void setLookupTablePopulated(bool value);

  /// Note that we have added a member into the iterable declaration context,
  /// so that it can also be added to the lookup table (if needed).
  void addedMember(Decl *member);

  /// Note that we have added an extension into the nominal type,
  /// so that its members can eventually be added to the lookup table.
  void addedExtension(ExtensionDecl *ext);

  /// A lookup table used to find the protocol conformances of
  /// a given nominal type.
  mutable ConformanceLookupTable *ConformanceTable = nullptr;

  /// Prepare the conformance table.
  void prepareConformanceTable() const;

  /// Returns the protocol requirements that \c Member conforms to.
  ArrayRef<ValueDecl *>
    getSatisfiedProtocolRequirementsForMember(const ValueDecl *Member,
                                              bool Sorted) const;

  friend class ASTContext;
  friend class MemberLookupTable;
  friend class ConformanceLookupTable;
  friend class ExtensionDecl;
  friend class DeclContext;
  friend class IterableDeclContext;
  friend ArrayRef<ValueDecl *>
           ValueDecl::getSatisfiedProtocolRequirements(bool Sorted) const;

protected:
  Type DeclaredTy;
  Type DeclaredTyInContext;
  Type DeclaredInterfaceTy;

  NominalTypeDecl(DeclKind K, DeclContext *DC, Identifier name,
                  SourceLoc NameLoc,
                  MutableArrayRef<TypeLoc> inherited,
                  GenericParamList *GenericParams) :
    GenericTypeDecl(K, DC, name, NameLoc, inherited, GenericParams),
    IterableDeclContext(IterableDeclContextKind::NominalTypeDecl)
  {
    setGenericParams(GenericParams);
    Bits.NominalTypeDecl.AddedImplicitInitializers = false;
    ExtensionGeneration = 0;
    Bits.NominalTypeDecl.HasLazyConformances = false;
  }

  friend class ProtocolType;

public:
  using GenericTypeDecl::getASTContext;

  SourceRange getBraces() const { return Braces; }
  
  void setBraces(SourceRange braces) { Braces = braces; }

  /// Should this declaration behave as if it must be accessed
  /// resiliently, even when we're building a non-resilient module?
  ///
  /// This is used for diagnostics, because we do not want a behavior
  /// change between builds with resilience enabled and disabled.
  bool isFormallyResilient() const;

  /// Do we need to use resilient access patterns outside of this type's
  /// resilience domain?
  bool isResilient() const;

  /// Do we need to use resilient access patterns when accessing this
  /// type from the given module?
  bool isResilient(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// Determine whether we have already attempted to add any
  /// implicitly-defined initializers to this declaration.
  bool addedImplicitInitializers() const {
    return Bits.NominalTypeDecl.AddedImplicitInitializers;
  }

  /// Note that we have attempted to add implicit initializers.
  void setAddedImplicitInitializers() {
    Bits.NominalTypeDecl.AddedImplicitInitializers = true;
  }

  /// Set the interface type of this nominal type to the metatype of the
  /// declared interface type.
  void computeType();

  /// getDeclaredType - Retrieve the type declared by this entity, without
  /// any generic parameters bound if this is a generic type.
  Type getDeclaredType() const;

  /// getDeclaredTypeInContext - Retrieve the type declared by this entity, with
  /// context archetypes bound if this is a generic type.
  Type getDeclaredTypeInContext() const;

  /// getDeclaredInterfaceType - Retrieve the type declared by this entity, with
  /// generic parameters bound if this is a generic type.
  Type getDeclaredInterfaceType() const;

  /// Add a new extension to this nominal type.
  void addExtension(ExtensionDecl *extension);

  /// Retrieve the set of extensions of this type.
  ExtensionRange getExtensions();

  /// Make a member of this nominal type, or one of its extensions,
  /// immediately visible in the lookup table.
  ///
  /// A member of a nominal type or extension thereof will become
  /// visible to name lookup as soon as it is added. However, if the
  /// addition of a member is delayed---for example, because it's
  /// being introduced in response to name lookup---this method can be
  /// called to make it immediately visible.
  void makeMemberVisible(ValueDecl *member);

  /// Special-behaviour flags passed to lookupDirect()
  enum class LookupDirectFlags {
    /// Whether to avoid loading any new extension.
    /// Used by the module loader to break recursion.
    IgnoreNewExtensions = 1 << 0,
    /// Whether to include @_implements members.
    /// Used by conformance-checking to find special @_implements members.
    IncludeAttrImplements = 1 << 1,
  };

  /// Find all of the declarations with the given name within this nominal type
  /// and its extensions.
  ///
  /// This routine does not look into superclasses, nor does it consider
  /// protocols to which the nominal type conforms. Furthermore, the resulting
  /// set of declarations has not been filtered for visibility, nor have
  /// overridden declarations been removed.
  TinyPtrVector<ValueDecl *> lookupDirect(DeclName name,
                                          OptionSet<LookupDirectFlags> flags =
                                          OptionSet<LookupDirectFlags>());

  /// Collect the set of protocols to which this type should implicitly
  /// conform, such as AnyObject (for classes).
  void getImplicitProtocols(SmallVectorImpl<ProtocolDecl *> &protocols);

  /// Look for conformances of this nominal type to the given
  /// protocol.
  ///
  /// \param module The module from which we initiate the search.
  /// FIXME: This is currently unused.
  ///
  /// \param protocol The protocol whose conformance is requested.
  /// \param conformances Will be populated with the set of protocol
  /// conformances found for this protocol.
  ///
  /// \returns true if any conformances were found. 
  bool lookupConformance(
         ModuleDecl *module, ProtocolDecl *protocol,
         SmallVectorImpl<ProtocolConformance *> &conformances) const;

  /// Retrieve all of the protocols that this nominal type conforms to.
  SmallVector<ProtocolDecl *, 2> getAllProtocols() const;

  /// Retrieve all of the protocol conformances for this nominal type.
  SmallVector<ProtocolConformance *, 2> getAllConformances(
                                          bool sorted = false) const;

  /// Register an externally-created protocol conformance in the
  /// conformance lookup table.
  ///
  /// This is used by deserialization of module files to report
  /// conformances.
  void registerProtocolConformance(ProtocolConformance *conformance);

  void setConformanceLoader(LazyMemberLoader *resolver, uint64_t contextData);

  /// Is this the decl for Optional<T>?
  bool isOptionalDecl() const;

  /// Is this a key path type?
  Optional<KeyPathTypeKind> getKeyPathTypeKind() const;

  /// Retrieve information about this type as a property wrapper.
  PropertyWrapperTypeInfo getPropertyWrapperTypeInfo() const;

  /// Return a collection of the stored member variables of this type.
  ArrayRef<VarDecl *> getStoredProperties() const;

  /// Return a collection of the stored member variables of this type, along
  /// with placeholders for unimportable stored properties.
  ArrayRef<Decl *> getStoredPropertiesAndMissingMemberPlaceholders() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_NominalTypeDecl &&
           D->getKind() <= DeclKind::Last_NominalTypeDecl;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() >= DeclKind::First_NominalTypeDecl &&
           D->getKind() <= DeclKind::Last_NominalTypeDecl;
  }

  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    return C->getIterableContextKind()
             == IterableDeclContextKind::NominalTypeDecl;
  }
  static bool classof(const NominalTypeDecl *D) { return true; }
  static bool classof(const ExtensionDecl *D) { return false; }
};

/// This is the declaration of an enum.
///
/// For example:
///
/// \code
///    enum Bool {
///      case false
///      case true
///    }
///
///    enum Optional<T> {
///      case none
///      case some(T)
///    }
/// \endcode
///
/// The type of the decl itself is a MetatypeType; use getDeclaredType()
/// to get the declared type ("Bool" or "Optional" in the above example).
class EnumDecl final : public NominalTypeDecl {
  SourceLoc EnumLoc;

  struct {
    /// The raw type and a bit to indicate whether the
    /// raw was computed yet or not.
    llvm::PointerIntPair<Type, 1, bool> RawType;
  } LazySemanticInfo;

  friend class EnumRawTypeRequest;
  friend class TypeChecker;

public:
  EnumDecl(SourceLoc EnumLoc, Identifier Name, SourceLoc NameLoc,
            MutableArrayRef<TypeLoc> Inherited,
            GenericParamList *GenericParams, DeclContext *DC);

  SourceLoc getStartLoc() const { return EnumLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(EnumLoc, getBraces().End);
  }

  EnumElementDecl *getElement(Identifier Name) const;
  
public:
  /// A range for iterating the elements of an enum.
  using ElementRange = DowncastFilterRange<EnumElementDecl, DeclRange>;

  /// A range for iterating the cases of an enum.
  using CaseRange = DowncastFilterRange<EnumCaseDecl, DeclRange>;

  /// Return a range that iterates over all the elements of an enum.
  ElementRange getAllElements() const {
    return ElementRange(getMembers());
  }

  unsigned getNumElements() const {
    auto eltRange = getAllElements();
    return std::distance(eltRange.begin(), eltRange.end());
  }

  /// If this enum has a unique element, return it. A unique element can
  /// either hold a value or not, and the existence of one unique element does
  /// not imply the existence or non-existence of the opposite unique element.
  EnumElementDecl *getUniqueElement(bool hasValue) const;

  /// Return a range that iterates over all the cases of an enum.
  CaseRange getAllCases() const {
    return CaseRange(getMembers());
  }

  /// Insert all of the 'case' element declarations into a DenseSet.
  void getAllElements(llvm::DenseSet<EnumElementDecl*> &elements) const {
    for (auto elt : getAllElements())
      elements.insert(elt);
  }
  
  /// Retrieve the status of circularity checking for class inheritance.
  CircularityCheck getCircularityCheck() const {
    return static_cast<CircularityCheck>(Bits.EnumDecl.Circularity);
  }
  
  /// Record the current stage of circularity checking.
  void setCircularityCheck(CircularityCheck circularity) {
    Bits.EnumDecl.Circularity = static_cast<unsigned>(circularity);
  }
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Enum;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Enum;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Enum;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
  
  /// Determine whether this enum declares a raw type in its inheritance clause.
  bool hasRawType() const { return (bool)getRawType(); }

  /// Retrieve the declared raw type of the enum from its inheritance clause,
  /// or null if it has none.
  Type getRawType() const;

  /// Set the raw type of the enum from its inheritance clause.
  void setRawType(Type rawType) {
    LazySemanticInfo.RawType.setPointerAndInt(rawType, true);
  }
  
  /// True if none of the enum cases have associated values.
  ///
  /// Note that this is true for enums with absolutely no cases.
  bool hasOnlyCasesWithoutAssociatedValues() const;

  /// True if any of the enum cases have availability annotations.
  ///
  /// Note that this is false for enums with absolutely no cases.
  bool hasPotentiallyUnavailableCaseValue() const;

  /// True if the enum has cases.
  bool hasCases() const {
    return !getAllElements().empty();
  }

  /// True if the enum is marked 'indirect'.
  bool isIndirect() const {
    return getAttrs().hasAttribute<IndirectAttr>();
  }

  /// True if the enum can be exhaustively switched within \p useDC.
  ///
  /// Note that this property is \e not necessarily true for all children of
  /// \p useDC. In particular, an inlinable function does not get to switch
  /// exhaustively over a non-exhaustive enum declared in the same module.
  ///
  /// This is the predicate used when deciding if a switch statement needs a
  /// default case. It should not be used for optimization or code generation.
  ///
  /// \sa isEffectivelyExhaustive
  bool isFormallyExhaustive(const DeclContext *useDC) const;

  /// True if the enum can be exhaustively switched within a function defined
  /// within \p M, with \p expansion specifying whether the function is
  /// inlinable.
  ///
  /// This is the predicate used when making optimization and code generation
  /// decisions. It should not be used at the AST or semantic level.
  ///
  /// \sa isFormallyExhaustive
  bool isEffectivelyExhaustive(ModuleDecl *M,
                               ResilienceExpansion expansion) const;
};

/// StructDecl - This is the declaration of a struct, for example:
///
///    struct Complex { var R : Double, I : Double }
///
/// The type of the decl itself is a MetatypeType; use getDeclaredType()
/// to get the declared type ("Complex" in the above example).
class StructDecl final : public NominalTypeDecl {
  SourceLoc StructLoc;

public:
  StructDecl(SourceLoc StructLoc, Identifier Name, SourceLoc NameLoc,
             MutableArrayRef<TypeLoc> Inherited,
             GenericParamList *GenericParams, DeclContext *DC);

  SourceLoc getStartLoc() const { return StructLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(StructLoc, getBraces().End);
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Struct;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Struct;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Struct;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }

  /// Does this struct contain unreferenceable storage, such as C fields that
  /// cannot be represented in Swift?
  bool hasUnreferenceableStorage() const {
    return Bits.StructDecl.HasUnreferenceableStorage;
  }

  void setHasUnreferenceableStorage(bool v) {
    Bits.StructDecl.HasUnreferenceableStorage = v;
  }
};

/// The kind of artificial main to generate for a class.
enum class ArtificialMainKind : uint8_t {
  NSApplicationMain,
  UIApplicationMain,
};

/// This is the base type for AncestryOptions. Each flag describes possible
/// interesting kinds of superclasses that a class may have.
enum class AncestryFlags : uint8_t {
  /// The class or one of its superclasses is @objc.
  ObjC = (1<<0),

  /// The class or one of its superclasses is @objcMembers.
  ObjCMembers = (1<<1),

  /// The class or one of its superclasses is generic.
  Generic = (1<<2),

  /// The class or one of its superclasses is resilient.
  Resilient = (1<<3),

  /// The class or one of its superclasses has resilient metadata and is in a
  /// different resilience domain.
  ResilientOther = (1<<4),

  /// The class or one of its superclasses is imported from Clang.
  ClangImported = (1<<5),

  /// The class or one of its superclasses requires stored property initializers.
  RequiresStoredPropertyInits = (1<<6),
};

/// Return type of ClassDecl::checkAncestry(). Describes a set of interesting
/// kinds of superclasses that a class may have.
using AncestryOptions = OptionSet<AncestryFlags>;

/// ClassDecl - This is the declaration of a class, for example:
///
///    class Complex { var R : Double, I : Double }
///
/// The type of the decl itself is a MetatypeType; use getDeclaredType()
/// to get the declared type ("Complex" in the above example).
class ClassDecl final : public NominalTypeDecl {
  class ObjCMethodLookupTable;

  SourceLoc ClassLoc;
  ObjCMethodLookupTable *ObjCMethodLookup = nullptr;

  /// Create the Objective-C member lookup table.
  void createObjCMethodLookup();

  struct {
    /// The superclass decl and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<ClassDecl *, 1, bool> SuperclassDecl;

    /// The superclass type and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<Type, 1, bool> SuperclassType;
  } LazySemanticInfo;

  friend class SuperclassDeclRequest;
  friend class SuperclassTypeRequest;
  friend class TypeChecker;

public:
  ClassDecl(SourceLoc ClassLoc, Identifier Name, SourceLoc NameLoc,
            MutableArrayRef<TypeLoc> Inherited,
            GenericParamList *GenericParams, DeclContext *DC);

  SourceLoc getStartLoc() const { return ClassLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(ClassLoc, getBraces().End);
  }

  /// Determine whether the member area of this class's metadata (which consists
  /// of field offsets and vtable entries) is to be considered opaque by clients.
  ///
  /// Note that even @_fixed_layout classes have resilient metadata if they are
  /// in a resilient module.
  bool hasResilientMetadata() const;

  /// Determine whether this class has resilient metadata when accessed from the
  /// given module and resilience expansion.
  bool hasResilientMetadata(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// Determine whether this class has a superclass.
  bool hasSuperclass() const { return (bool)getSuperclassDecl(); }

  /// Retrieve the superclass of this class, or null if there is no superclass.
  Type getSuperclass() const;

  /// Retrieve the ClassDecl for the superclass of this class, or null if there
  /// is no superclass.
  ClassDecl *getSuperclassDecl() const;

  /// Check if this class is a superclass or equal to the given class.
  bool isSuperclassOf(ClassDecl *other) const;

  /// Set the superclass of this class.
  void setSuperclass(Type superclass);

  /// Retrieve the status of circularity checking for class inheritance.
  CircularityCheck getCircularityCheck() const {
    return static_cast<CircularityCheck>(Bits.ClassDecl.Circularity);
  }

  /// Record the current stage of circularity checking.
  void setCircularityCheck(CircularityCheck circularity) {
    Bits.ClassDecl.Circularity = static_cast<unsigned>(circularity);
  }

  /// Walk this class and all of the superclasses of this class, transitively,
  /// invoking the callback function for each class.
  ///
  /// \param fn The callback function that will be invoked for each superclass.
  /// It can return \c Continue to continue the traversal. Returning
  /// \c SkipChildren halts the search and returns \c false, while returning
  /// \c Stop halts the search and returns \c true.
  ///
  /// \returns \c true if \c fn returned \c Stop for any class, \c false
  /// otherwise.
  bool walkSuperclasses(
      llvm::function_ref<TypeWalker::Action(ClassDecl *)> fn) const;

  //// Whether this class requires all of its stored properties to
  //// have initializers in the class definition.
  bool requiresStoredPropertyInits() const {
    return checkAncestry(AncestryFlags::RequiresStoredPropertyInits);
  }

  /// \see getForeignClassKind
  enum class ForeignKind : uint8_t {
    /// A normal Swift or Objective-C class.
    Normal = 0,
    /// An imported Core Foundation type. These are AnyObject-compatible but
    /// do not have runtime metadata.
    CFType,
    /// An imported Objective-C type whose class and metaclass symbols are not
    /// both available at link-time but can be accessed through the Objective-C
    /// runtime.
    RuntimeOnly
  };

  /// Whether this class is "foreign", meaning that it is implemented
  /// by a runtime that Swift does not have first-class integration
  /// with.  This generally means that:
  ///   - class data is either abstracted or cannot be made to
  ///     fit with Swift's metatype schema, and/or
  ///   - there is no facility for subclassing or adding polymorphic
  ///     methods to the class.
  ///
  /// We may find ourselves wanting to break this bit into more
  /// precise chunks later.
  ForeignKind getForeignClassKind() const {
    return static_cast<ForeignKind>(Bits.ClassDecl.RawForeignKind);
  }
  void setForeignClassKind(ForeignKind kind) {
    Bits.ClassDecl.RawForeignKind = static_cast<unsigned>(kind);
  }

  /// Returns true if this class is any kind of "foreign class".
  ///
  /// \see getForeignClassKind
  bool isForeign() const {
    return getForeignClassKind() != ForeignKind::Normal;
  }

  /// Returns true if the class has designated initializers that are not listed
  /// in its members.
  ///
  /// This can occur, for example, if the class is an Objective-C class with
  /// initializers that cannot be represented in Swift.
  bool hasMissingDesignatedInitializers() const;

  void setHasMissingDesignatedInitializers(bool newValue = true) {
    Bits.ClassDecl.HasMissingDesignatedInitializers = newValue;
  }

  /// Returns true if the class has missing members that require vtable entries.
  ///
  /// In this case, the class cannot be subclassed, because we cannot construct
  /// the vtable for the subclass.
  bool hasMissingVTableEntries() const;

  void setHasMissingVTableEntries(bool newValue = true) {
    Bits.ClassDecl.HasMissingVTableEntries = newValue;
  }

  /// Returns true if this class cannot be used with weak or unowned
  /// references.
  /// 
  /// Note that this is true if this class or any of its ancestor classes
  /// are marked incompatible.
  bool isIncompatibleWithWeakReferences() const;

  void setIsIncompatibleWithWeakReferences(bool newValue = true) {
    Bits.ClassDecl.IsIncompatibleWithWeakReferences = newValue;
  }

  /// Find a method of a class that overrides a given method.
  /// Return nullptr, if no such method exists.
  AbstractFunctionDecl *findOverridingDecl(
      const AbstractFunctionDecl *method) const;

  /// Find a method implementation which will be used when a given method
  /// is invoked on an instance of this class. This implementation may stem
  /// either from a class itself or its direct or indirect superclasses.
  AbstractFunctionDecl *findImplementingMethod(
      const AbstractFunctionDecl *method) const;

  /// True if the class has a destructor.
  ///
  /// Fully type-checked classes always contain destructors, but during parsing
  /// or type-checking, the implicit destructor may not have been synthesized
  /// yet if one was not explicitly declared.
  bool hasDestructor() const { return Bits.ClassDecl.HasDestructorDecl; }

  /// Set the 'has destructor' flag.
  void setHasDestructor() { Bits.ClassDecl.HasDestructorDecl = 1; }
  
  /// Retrieve the destructor for this class.
  DestructorDecl *getDestructor();

  /// Synthesize implicit, trivial destructor, add it to this ClassDecl
  /// and return it.
  void addImplicitDestructor();

  /// Determine whether this class inherits the convenience initializers
  /// from its superclass.
  bool inheritsSuperclassInitializers();

  /// Marks that this class inherits convenience initializers from its
  /// superclass.
  ///
  /// This is computed as part of adding implicit initializers.
  void setInheritsSuperclassInitializers() {
    assert(addedImplicitInitializers());
    Bits.ClassDecl.InheritsSuperclassInits = true;
  }

  /// Walks the class hierarchy starting from this class, checking various
  /// conditions.
  AncestryOptions checkAncestry() const;

  /// Check if the class has ancestry of the given kind.
  bool checkAncestry(AncestryFlags flag) const {
    return checkAncestry().contains(flag);
  }

  /// The type of metaclass to use for a class.
  enum class MetaclassKind : uint8_t {
    ObjC,
    SwiftStub,
  };

  /// Determine which sort of metaclass to use for this class
  MetaclassKind getMetaclassKind() const;

  /// Retrieve the name to use for this class when interoperating with
  /// the Objective-C runtime.
  StringRef getObjCRuntimeName(llvm::SmallVectorImpl<char> &buffer) const;

  /// Returns the appropriate kind of entry point to generate for this class,
  /// based on its attributes.
  ///
  /// It is an error to call this on a class that does not have a
  /// *ApplicationMain attribute.
  ArtificialMainKind getArtificialMainKind() const;

  using NominalTypeDecl::lookupDirect;

  /// Look in this class and its extensions (but not any of its protocols or
  /// superclasses) for declarations with a given Objective-C selector.
  ///
  /// Note that this can find methods, initializers, deinitializers,
  /// getters, and setters.
  ///
  /// \param selector The Objective-C selector of the method we're
  /// looking for.
  ///
  /// \param isInstance Whether we are looking for an instance method
  /// (vs. a class method).
  MutableArrayRef<AbstractFunctionDecl *> lookupDirect(ObjCSelector selector,
                                                       bool isInstance);

  /// Record the presence of an @objc method with the given selector.
  void recordObjCMethod(AbstractFunctionDecl *method, ObjCSelector selector);

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Class;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Class;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Class;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
  
  /// Returns true if the decl uses the Objective-C generics model.
  ///
  /// This is true of imported Objective-C classes.
  bool usesObjCGenericsModel() const {
    return hasClangNode() && isGenericContext() && isObjC();
  }
  
  /// True if the class is known to be implemented in Swift.
  bool hasKnownSwiftImplementation() const {
    return !hasClangNode();
  }
};


/// Describes whether a requirement refers to 'Self', for use in the
/// is-inheritable and is-available-existential checks.
struct SelfReferenceKind {
  bool result;
  bool parameter;
  bool requirement;
  bool other;

  /// The type does not refer to 'Self' at all.
  static SelfReferenceKind None() {
    return SelfReferenceKind(false, false, false, false);
  }

  /// The type refers to 'Self', but only as the result type of a method.
  static SelfReferenceKind Result() {
    return SelfReferenceKind(true, false, false, false);
  }

  /// The type refers to 'Self', but only as the parameter type of a method.
  static SelfReferenceKind Parameter() {
    return SelfReferenceKind(false, true, false, false);
  }

  /// The type refers to 'Self' within a same-type requiement.
  static SelfReferenceKind Requirement() {
    return SelfReferenceKind(false, false, true, false);
  }

  /// The type refers to 'Self' in a position that is invariant.
  static SelfReferenceKind Other() {
    return SelfReferenceKind(false, false, false, true);
  }

  SelfReferenceKind flip() const {
    return SelfReferenceKind(parameter, result, requirement, other);
  }

  SelfReferenceKind operator|=(SelfReferenceKind kind) {
    result |= kind.result;
    requirement |= kind.requirement;
    parameter |= kind.parameter;
    other |= kind.other;
    return *this;
  }

  operator bool() const {
    return result || parameter || requirement || other;
  }

private:
  SelfReferenceKind(bool result, bool parameter, bool requirement, bool other)
    : result(result), parameter(parameter), requirement(requirement),
      other(other) { }
};

/// ProtocolDecl - A declaration of a protocol, for example:
///
///   protocol Drawable {
///     func draw()
///   }
///
/// Every protocol has an implicitly-created 'Self' generic parameter that
/// stands for a type that conforms to the protocol. For example,
///
///   protocol Cloneable {
///     func clone() -> Self
///   }
///
class ProtocolDecl final : public NominalTypeDecl {
  SourceLoc ProtocolLoc;

  ArrayRef<ProtocolDecl *> InheritedProtocols;

  struct {
    /// The superclass decl and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<ClassDecl *, 1, bool> SuperclassDecl;

    /// The superclass type and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<Type, 1, bool> SuperclassType;
  } LazySemanticInfo;

  /// The generic signature representing exactly the new requirements introduced
  /// by this protocol.
  const Requirement *RequirementSignature = nullptr;

  /// Returns the cached result of \c requiresClass or \c None if it hasn't yet
  /// been computed.
  Optional<bool> getCachedRequiresClass() const {
    if (Bits.ProtocolDecl.RequiresClassValid)
      return Bits.ProtocolDecl.RequiresClass;

    return None;
  }

  /// Caches the result of \c requiresClass
  void setCachedRequiresClass(bool requiresClass) {
    Bits.ProtocolDecl.RequiresClassValid = true;
    Bits.ProtocolDecl.RequiresClass = requiresClass;
  }

  /// Returns the cached result of \c existentialConformsToSelf or \c None if it
  /// hasn't yet been computed.
  Optional<bool> getCachedExistentialConformsToSelf() const {
    if (Bits.ProtocolDecl.ExistentialConformsToSelfValid)
      return Bits.ProtocolDecl.ExistentialConformsToSelf;

    return None;
  }

  /// Caches the result of \c existentialConformsToSelf
  void setCachedExistentialConformsToSelf(bool result) {
    Bits.ProtocolDecl.ExistentialConformsToSelfValid = true;
    Bits.ProtocolDecl.ExistentialConformsToSelf = result;
  }

  /// Returns the cached result of \c existentialTypeSupported or \c None if it
  /// hasn't yet been computed.
  Optional<bool> getCachedExistentialTypeSupported() {
    if (Bits.ProtocolDecl.ExistentialTypeSupportedValid)
      return Bits.ProtocolDecl.ExistentialTypeSupported;

    return None;
  }

  /// Caches the result of \c existentialTypeSupported
  void setCachedExistentialTypeSupported(bool supported) {
    Bits.ProtocolDecl.ExistentialTypeSupportedValid = true;
    Bits.ProtocolDecl.ExistentialTypeSupported = supported;
  }

  ArrayRef<ProtocolDecl *> getInheritedProtocolsSlow();

  bool hasLazyRequirementSignature() const {
    return Bits.ProtocolDecl.HasLazyRequirementSignature;
  }

  friend class SuperclassDeclRequest;
  friend class SuperclassTypeRequest;
  friend class RequirementSignatureRequest;
  friend class ProtocolRequiresClassRequest;
  friend class ExistentialConformsToSelfRequest;
  friend class ExistentialTypeSupportedRequest;
  friend class TypeChecker;

public:
  ProtocolDecl(DeclContext *DC, SourceLoc ProtocolLoc, SourceLoc NameLoc,
               Identifier Name, MutableArrayRef<TypeLoc> Inherited,
               TrailingWhereClause *TrailingWhere);

  using Decl::getASTContext;

  /// Retrieve the set of protocols inherited from this protocol.
  ArrayRef<ProtocolDecl *> getInheritedProtocols() const {
    if (Bits.ProtocolDecl.InheritedProtocolsValid)
      return InheritedProtocols;

    return const_cast<ProtocolDecl *>(this)->getInheritedProtocolsSlow();
  }

  /// Determine whether this protocol has a superclass.
  bool hasSuperclass() const { return (bool)getSuperclassDecl(); }

  /// Retrieve the superclass of this protocol, or null if there is no superclass.
  Type getSuperclass() const;

  /// Retrieve the ClassDecl for the superclass of this protocol, or null if there
  /// is no superclass.
  ClassDecl *getSuperclassDecl() const;

  /// Set the superclass of this protocol.
  void setSuperclass(Type superclass);

  /// Retrieve the set of AssociatedTypeDecl members of this protocol; this
  /// saves loading the set of members in cases where there's no possibility of
  /// a protocol having nested types (ObjC protocols).
  llvm::TinyPtrVector<AssociatedTypeDecl *> getAssociatedTypeMembers() const;

  /// Walk this protocol and all of the protocols inherited by this protocol,
  /// transitively, invoking the callback function for each protocol.
  ///
  /// \param fn The callback function that will be invoked for each inherited
  /// protocol. It can return \c Continue to continue the traversal,
  /// \c SkipChildren to avoid visiting the children of the given protocol
  /// but continue the search, and \c Stop to halt the search.
  ///
  /// \returns \c true if \c fn returned \c Stop for any protocol, \c false
  /// otherwise.
  bool walkInheritedProtocols(
               llvm::function_ref<TypeWalker::Action(ProtocolDecl *)> fn) const;

  /// Determine whether this protocol inherits from the given ("super")
  /// protocol.
  bool inheritsFrom(const ProtocolDecl *Super) const;
  
  ProtocolType *getDeclaredType() const {
    return reinterpret_cast<ProtocolType *>(
      NominalTypeDecl::getDeclaredType().getPointer());
  }
  
  SourceLoc getStartLoc() const { return ProtocolLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(ProtocolLoc, getBraces().End);
  }

  /// True if this protocol can only be conformed to by class types.
  bool requiresClass() const;

  /// Determine whether an existential conforming to this protocol can be
  /// matched with a generic type parameter constrained to this protocol.
  /// This is only permitted if there is nothing "non-trivial" that we
  /// can do with the metatype, which means the protocol must not have
  /// any static methods and must be declared @objc.
  bool existentialConformsToSelf() const;

  /// Does this protocol require a self-conformance witness table?
  bool requiresSelfConformanceWitnessTable() const;

  /// Find direct Self references within the given requirement.
  ///
  /// \param allowCovariantParameters If true, 'Self' is assumed to be
  /// covariant anywhere; otherwise, only in the return type of the top-level
  /// function type.
  ///
  /// \param skipAssocTypes If true, associated types of 'Self' are ignored;
  /// otherwise, they count as an 'other' usage of 'Self'.
  SelfReferenceKind findProtocolSelfReferences(const ValueDecl *decl,
                                               bool allowCovariantParameters,
                                               bool skipAssocTypes) const;

  /// Determine whether we are allowed to refer to an existential type
  /// conforming to this protocol. This is only permitted if the type of
  /// the member does not contain any associated types, and does not
  /// contain 'Self' in 'parameter' or 'other' position.
  bool isAvailableInExistential(const ValueDecl *decl) const;

  /// Determine whether we are allowed to refer to an existential type
  /// conforming to this protocol. This is only permitted if the types of
  /// all the members do not contain any associated types, and do not
  /// contain 'Self' in 'parameter' or 'other' position.
  bool existentialTypeSupported() const;

private:
  void computeKnownProtocolKind() const;

public:
  /// If this is known to be a compiler-known protocol, returns the kind.
  /// Otherwise returns None.
  Optional<KnownProtocolKind> getKnownProtocolKind() const {
    if (Bits.ProtocolDecl.KnownProtocol == 0)
      computeKnownProtocolKind();

    if (Bits.ProtocolDecl.KnownProtocol == 1)
      return None;
    
    return static_cast<KnownProtocolKind>(Bits.ProtocolDecl.KnownProtocol - 2);
  }

  /// Check whether this protocol is of a specific, known protocol kind.
  bool isSpecificProtocol(KnownProtocolKind kind) const {
    if (auto knownKind = getKnownProtocolKind())
      return *knownKind == kind;

    return false;
  }

  /// Retrieve the status of circularity checking for protocol inheritance.
  CircularityCheck getCircularityCheck() const {
    return static_cast<CircularityCheck>(Bits.ProtocolDecl.Circularity);
  }

  /// Record the current stage of circularity checking.
  void setCircularityCheck(CircularityCheck circularity) {
    Bits.ProtocolDecl.Circularity = static_cast<unsigned>(circularity);
  }

  /// Returns true if the protocol has requirements that are not listed in its
  /// members.
  ///
  /// This can occur, for example, if the protocol is an Objective-C protocol
  /// with requirements that cannot be represented in Swift.
  bool hasMissingRequirements() const {
    (void)getMembers();
    return Bits.ProtocolDecl.HasMissingRequirements;
  }

  void setHasMissingRequirements(bool newValue) {
    Bits.ProtocolDecl.HasMissingRequirements = newValue;
  }

  /// Returns the default type witness for an associated type, or a null
  /// type if there is no default.
  Type getDefaultTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Set the default type witness for an associated type.
  void setDefaultTypeWitness(AssociatedTypeDecl *assocType, Type witness);

  /// Returns the default witness for a requirement, or nullptr if there is
  /// no default.
  Witness getDefaultWitness(ValueDecl *requirement) const;

  /// Record the default witness for a requirement.
  void setDefaultWitness(ValueDecl *requirement, Witness witness);

  /// Returns the default associated conformance witness for an associated
  /// type, or \c None if there is no default.
  Optional<ProtocolConformanceRef> getDefaultAssociatedConformanceWitness(
                                              CanType association,
                                              ProtocolDecl *requirement) const;

  /// Set the default associated conformance witness for the given
  /// associated conformance.
  void setDefaultAssociatedConformanceWitness(
                                            CanType association,
                                            ProtocolDecl *requirement,
                                            ProtocolConformanceRef conformance);

  /// Retrieve the name to use for this protocol when interoperating
  /// with the Objective-C runtime.
  StringRef getObjCRuntimeName(llvm::SmallVectorImpl<char> &buffer) const;

  /// Create the generic parameters of this protocol if the haven't been
  /// created yet.
  void createGenericParamsIfMissing();

  /// Retrieve the requirements that describe this protocol.
  ///
  /// These are the requirements including any inherited protocols
  /// and conformances for associated types that are introduced in this
  /// protocol. Requirements implied via any other protocol (e.g., inherited
  /// protocols of the inherited protocols) are not mentioned. The conformance
  /// requirements listed here become entries in the witness table.
  ArrayRef<Requirement> getRequirementSignature() const;

  /// Is the requirement signature currently being computed?
  bool isComputingRequirementSignature() const;

  /// Has the requirement signature been computed yet?
  bool isRequirementSignatureComputed() const {
    return RequirementSignature != nullptr;
  }

  void setRequirementSignature(ArrayRef<Requirement> requirements);

  void setLazyRequirementSignature(LazyMemberLoader *lazyLoader,
                                   uint64_t requirementSignatureData);

private:
  ArrayRef<Requirement> getCachedRequirementSignature() const;

public:
  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Protocol;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Protocol;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Protocol;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
};

/// AbstractStorageDecl - This is the common superclass for VarDecl and
/// SubscriptDecl, representing potentially settable memory locations.
class AbstractStorageDecl : public ValueDecl {
  friend class SetterAccessLevelRequest;
  friend class IsGetterMutatingRequest;
  friend class IsSetterMutatingRequest;
  friend class OpaqueReadOwnershipRequest;
  friend class StorageImplInfoRequest;
  friend class RequiresOpaqueAccessorsRequest;
  friend class RequiresOpaqueModifyCoroutineRequest;
  friend class SynthesizeAccessorRequest;

public:
  static const size_t MaxNumAccessors = 255;
private:
  /// A record of the accessors for the declaration.
  class alignas(1 << 3) AccessorRecord final :
      private llvm::TrailingObjects<AccessorRecord, AccessorDecl*> {
    friend TrailingObjects;

    using AccessorIndex = uint8_t;
    static const AccessorIndex InvalidIndex = 0;

    /// The range of the braces around the accessor clause.
    SourceRange Braces;

    /// The number of accessors currently stored in this record.
    AccessorIndex NumAccessors;

    /// The storage capacity of this record for accessors.  Always includes
    /// enough space for adding opaque accessors to the record, which are the
    /// only accessors that should ever be added retroactively; hence this
    /// field is only here for the purposes of safety checks.
    AccessorIndex AccessorsCapacity;

    /// Either 0, meaning there is no registered accessor of the given kind,
    /// or the index+1 of the accessor in the accessors array.
    AccessorIndex AccessorIndices[NumAccessorKinds];

    AccessorRecord(SourceRange braces,
                   ArrayRef<AccessorDecl*> accessors,
                   AccessorIndex accessorsCapacity);
  public:
    static AccessorRecord *create(ASTContext &ctx, SourceRange braces,
                                  ArrayRef<AccessorDecl*> accessors);

    SourceRange getBracesRange() const { return Braces; }

    inline AccessorDecl *getAccessor(AccessorKind kind) const;

    ArrayRef<AccessorDecl *> getAllAccessors() const {
      return { getTrailingObjects<AccessorDecl*>(), NumAccessors };
    }

    void addOpaqueAccessor(AccessorDecl *accessor);

  private:
    MutableArrayRef<AccessorDecl *> getAccessorsBuffer() {
      return { getTrailingObjects<AccessorDecl*>(), NumAccessors };
    }

    bool registerAccessor(AccessorDecl *accessor, AccessorIndex index);
  };

  llvm::PointerIntPair<AccessorRecord*, 3, OptionalEnum<AccessLevel>> Accessors;

  struct {
    unsigned IsGetterMutatingComputed : 1;
    unsigned IsGetterMutating : 1;
    unsigned IsSetterMutatingComputed : 1;
    unsigned IsSetterMutating : 1;
    unsigned OpaqueReadOwnershipComputed : 1;
    unsigned OpaqueReadOwnership : 2;
    unsigned ImplInfoComputed : 1;
    unsigned RequiresOpaqueAccessorsComputed : 1;
    unsigned RequiresOpaqueAccessors : 1;
    unsigned RequiresOpaqueModifyCoroutineComputed : 1;
    unsigned RequiresOpaqueModifyCoroutine : 1;
  } LazySemanticInfo = { };

  /// The implementation info for the accessors.
  StorageImplInfo ImplInfo;

  /// Add a synthesized accessor.
  void setSynthesizedAccessor(AccessorKind kind, AccessorDecl *getter);

protected:
  AbstractStorageDecl(DeclKind Kind, bool IsStatic, DeclContext *DC,
                      DeclName Name, SourceLoc NameLoc,
                      StorageIsMutable_t supportsMutation)
    : ValueDecl(Kind, DC, Name, NameLoc),
      ImplInfo(StorageImplInfo::getSimpleStored(supportsMutation)) {
    Bits.AbstractStorageDecl.IsStatic = IsStatic;
  }

  void computeIsValidKeyPathComponent();
  
  OpaqueTypeDecl *OpaqueReturn = nullptr;

public:

  /// Should this declaration be treated as if annotated with transparent
  /// attribute.
  bool isTransparent() const;

  /// Is this a type ('static') variable?
  bool isStatic() const {
    return Bits.AbstractStorageDecl.IsStatic;
  }
  void setStatic(bool IsStatic) {
    Bits.AbstractStorageDecl.IsStatic = IsStatic;
  }

  /// \returns the way 'static'/'class' should be spelled for this declaration.
  StaticSpellingKind getCorrectStaticSpelling() const;

  /// Return the interface type of the stored value.
  Type getValueInterfaceType() const;

  /// Determine how this storage is implemented.
  StorageImplInfo getImplInfo() const;

  /// Overwrite the registered implementation-info.  This should be
  /// used carefully.
  void setImplInfo(StorageImplInfo implInfo) {
    LazySemanticInfo.ImplInfoComputed = 1;
    ImplInfo = implInfo;
  }

  ReadImplKind getReadImpl() const {
    return getImplInfo().getReadImpl();
  }
  WriteImplKind getWriteImpl() const {
    return getImplInfo().getWriteImpl();
  }
  ReadWriteImplKind getReadWriteImpl() const {
    return getImplInfo().getReadWriteImpl();
  }


  /// Return true if this is a VarDecl that has storage associated with
  /// it.
  bool hasStorage() const {
    return getImplInfo().hasStorage();
  }

  /// Return true if this storage has the basic accessors/capability
  /// to be mutated.  This is generally constant after the accessors are
  /// installed by the parser/importer/whatever.
  ///
  /// Note that this is different from the mutability of the declaration
  /// in the user language: sometimes we can assign to declarations that
  /// don't support mutation (e.g. to initialize them), and sometimes we
  /// can't mutate things that do support mutation (e.g. because their
  /// setter is private).
  StorageIsMutable_t supportsMutation() const {
    return getImplInfo().supportsMutation();
  }

  /// isSettable - Determine whether references to this decl may appear
  /// on the left-hand side of an assignment or as the operand of a
  /// `&` or 'inout' operator.
  bool isSettable(const DeclContext *UseDC,
                  const DeclRefExpr *base = nullptr) const;

  /// Are there any accessors for this declaration, including implicit ones?
  bool hasAnyAccessors() const {
    return !getAllAccessors().empty();
  }

  /// Return the ownership of values opaquely read from this storage.
  OpaqueReadOwnership getOpaqueReadOwnership() const;
  void setOpaqueReadOwnership(OpaqueReadOwnership ownership) {
    LazySemanticInfo.OpaqueReadOwnership = unsigned(ownership);
    LazySemanticInfo.OpaqueReadOwnershipComputed = true;
  }

  /// Return true if reading this storage requires the ability to
  /// modify the base value.
  bool isGetterMutating() const;
  void setIsGetterMutating(bool isMutating) {
    LazySemanticInfo.IsGetterMutating = isMutating;
    LazySemanticInfo.IsGetterMutatingComputed = true;
  }
  
  /// Return true if modifying this storage requires the ability to
  /// modify the base value.
  bool isSetterMutating() const;
  void setIsSetterMutating(bool isMutating) {
    LazySemanticInfo.IsSetterMutating = isMutating;
    LazySemanticInfo.IsSetterMutatingComputed = true;
  }

  AccessorDecl *getAccessor(AccessorKind kind) const {
    if (auto info = Accessors.getPointer())
      return info->getAccessor(kind);
    return nullptr;
  }

  ArrayRef<AccessorDecl*> getAllAccessors() const {
    if (const auto *info = Accessors.getPointer())
      return info->getAllAccessors();
    return {};
  }

  /// Return an accessor that this storage is expected to have, synthesizing
  /// one if necessary. Note that will always synthesize one, even if the
  /// accessor is not part of the expected opaque set for the storage, so use
  /// with caution.
  AccessorDecl *getSynthesizedAccessor(AccessorKind kind) const;

  /// Return an accessor part of the set of opaque accessors dictated by the
  /// requirements of the ABI.
  ///
  /// This will synthesize the accessor if one is required but not specified
  /// in source; for example, most of the time a mutable property is required
  /// to have a 'modify' accessor, but if the property was only written with
  /// 'get' and 'set' accessors, 'modify' will be synthesized to call 'get'
  /// followed by 'set'.
  ///
  /// If the accessor is not needed for ABI reasons, this returns nullptr.
  /// To ensure an accessor is always returned, use getSynthesizedAccessor().
  AccessorDecl *getOpaqueAccessor(AccessorKind kind) const;

  /// Return an accessor that was written in source. Returns null if the
  /// accessor was not explicitly defined by the user.
  AccessorDecl *getParsedAccessor(AccessorKind kind) const;

  /// Visit all the opaque accessors that this storage is expected to have.
  void visitExpectedOpaqueAccessors(
                            llvm::function_ref<void (AccessorKind)>) const;

  /// Visit all the opaque accessors of this storage declaration.
  void visitOpaqueAccessors(llvm::function_ref<void (AccessorDecl*)>) const;

  void setAccessors(SourceLoc lbraceLoc, ArrayRef<AccessorDecl*> accessors,
                    SourceLoc rbraceLoc);

  /// Add a setter to an existing Computed var.
  ///
  /// This should only be used by the ClangImporter.
  void setComputedSetter(AccessorDecl *Set);

  /// Does this storage require opaque accessors of any kind?
  bool requiresOpaqueAccessors() const;

  /// Does this storage require an opaque accessor of the given kind?
  bool requiresOpaqueAccessor(AccessorKind kind) const;

  /// Does this storage require a 'get' accessor in its opaque-accessors set?
  bool requiresOpaqueGetter() const {
    return getOpaqueReadOwnership() != OpaqueReadOwnership::Borrowed;
  }

  /// Does this storage require a 'read' accessor in its opaque-accessors set?
  bool requiresOpaqueReadCoroutine() const {
    return getOpaqueReadOwnership() != OpaqueReadOwnership::Owned;
  }

  /// Does this storage require a 'set' accessor in its opaque-accessors set?
  bool requiresOpaqueSetter() const { return supportsMutation(); }

  /// Does this storage require a 'modify' accessor in its opaque-accessors set?
  bool requiresOpaqueModifyCoroutine() const;

  SourceRange getBracesRange() const {
    if (auto info = Accessors.getPointer())
      return info->getBracesRange();
    return SourceRange();
  }

  AccessLevel getSetterFormalAccess() const;

  AccessScope
  getSetterFormalAccessScope(const DeclContext *useDC = nullptr,
                             bool treatUsableFromInlineAsPublic = false) const;

  void setSetterAccess(AccessLevel accessLevel) {
    assert(!Accessors.getInt().hasValue());
    overwriteSetterAccess(accessLevel);
  }

  void overwriteSetterAccess(AccessLevel accessLevel);

  /// Given that this is an Objective-C property or subscript declaration,
  /// produce its getter selector.
  ObjCSelector
  getObjCGetterSelector(Identifier preferredName = Identifier()) const;

  /// Given that this is an Objective-C property or subscript declaration,
  /// produce its setter selector.
  ObjCSelector
  getObjCSetterSelector(Identifier preferredName = Identifier()) const;

  AbstractStorageDecl *getOverriddenDecl() const {
    return cast_or_null<AbstractStorageDecl>(ValueDecl::getOverriddenDecl());
  }

  /// Returns the location of 'override' keyword, if any.
  SourceLoc getOverrideLoc() const;

  /// Returns true if this declaration has a setter accessible from the given
  /// context.
  ///
  /// If \p DC is null, returns true only if the setter is public.
  ///
  /// See \c isAccessibleFrom for a discussion of the \p forConformance
  /// parameter.
  bool isSetterAccessibleFrom(const DeclContext *DC,
                              bool forConformance=false) const;

  /// Determine how this storage declaration should actually be accessed.
  AccessStrategy getAccessStrategy(AccessSemantics semantics,
                                   AccessKind accessKind,
                                   ModuleDecl *module,
                                   ResilienceExpansion expansion) const;

  /// Should this declaration behave as if it must be accessed
  /// resiliently, even when we're building a non-resilient module?
  ///
  /// This is used for diagnostics, because we do not want a behavior
  /// change between builds with resilience enabled and disabled.
  bool isFormallyResilient() const;

  /// Do we need to use resilient access patterns outside of this
  /// property's resilience domain?
  bool isResilient() const;

  /// Do we need to use resilient access patterns when accessing this
  /// property from the given module?
  bool isResilient(ModuleDecl *M, ResilienceExpansion expansion) const;

  void setIsValidKeyPathComponent(bool value) {
    Bits.AbstractStorageDecl.HasComputedValidKeyPathComponent = true;
    Bits.AbstractStorageDecl.ValidKeyPathComponent = value;
  }

  /// True if the storage can be referenced by a keypath directly.
  /// Otherwise, its override must be referenced.
  bool isValidKeyPathComponent() const {
    if (!Bits.AbstractStorageDecl.HasComputedValidKeyPathComponent)
      const_cast<AbstractStorageDecl *>(this)->computeIsValidKeyPathComponent();
    return Bits.AbstractStorageDecl.ValidKeyPathComponent;
  }

  /// True if the storage exports a property descriptor for key paths in
  /// other modules.
  bool exportsPropertyDescriptor() const;

  /// True if any of the accessors to the storage is private or fileprivate.
  bool hasPrivateAccessor() const;

  bool hasDidSetOrWillSetDynamicReplacement() const;

  bool hasAnyNativeDynamicAccessors() const;

  bool hasAnyDynamicReplacementAccessors() const;

  OpaqueTypeDecl *getOpaqueResultTypeDecl() const {
    return OpaqueReturn;
  }
  void setOpaqueResultTypeDecl(OpaqueTypeDecl *decl) {
    assert(!OpaqueReturn && "already has opaque type decl");
    OpaqueReturn = decl;
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_AbstractStorageDecl &&
           D->getKind() <= DeclKind::Last_AbstractStorageDecl;
  }
};

/// Describes which synthesized property for a property with an attached
/// wrapper is being referenced.
enum class PropertyWrapperSynthesizedPropertyKind {
  /// The backing storage property, which is a stored property of the
  /// wrapper type.
  Backing,
  /// A storage wrapper (e.g., `$foo`), which is a wrapper over the
  /// wrapper instance's `projectedValue` property.
  StorageWrapper,
};

/// VarDecl - 'var' and 'let' declarations.
class VarDecl : public AbstractStorageDecl {
public:
  enum class Introducer : uint8_t {
    Let = 0,
    Var = 1
  };

protected:
  PointerUnion3<PatternBindingDecl *, Stmt *, VarDecl *> Parent;

  VarDecl(DeclKind kind, bool isStatic, Introducer introducer,
          bool issCaptureList, SourceLoc nameLoc, Identifier name,
          DeclContext *dc, StorageIsMutable_t supportsMutation);

  /// This is the type specified, including location information.
  TypeLoc typeLoc;

  Type typeInContext;

public:
  VarDecl(bool isStatic, Introducer introducer, bool isCaptureList,
          SourceLoc nameLoc, Identifier name, DeclContext *dc)
    : VarDecl(DeclKind::Var, isStatic, introducer, isCaptureList, nameLoc,
              name, dc, StorageIsMutable_t(introducer == Introducer::Var)) {}

  SourceRange getSourceRange() const;

  Identifier getName() const { return getFullName().getBaseIdentifier(); }

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    assert(!getFullName().isSpecial() && "Cannot get string for special names");
    return hasName() ? getBaseName().getIdentifier().str() : "_";
  }

  TypeLoc &getTypeLoc() { return typeLoc; }
  TypeLoc getTypeLoc() const { return typeLoc; }

  bool hasType() const {
    // We have a type if either the type has been computed already or if
    // this is a deserialized declaration with an interface type.
    return !typeInContext.isNull();
  }

  /// Get the type of the variable within its context. If the context is generic,
  /// this will use archetypes.
  Type getType() const;

  /// Set the type of the variable within its context.
  void setType(Type t);

  void markInvalid();

  /// Retrieve the source range of the variable type, or an invalid range if the
  /// variable's type is not explicitly written in the source.
  ///
  /// Only for use in diagnostics.  It is not always possible to always
  /// precisely point to the variable type because of type aliases.
  SourceRange getTypeSourceRangeForDiagnostics() const;

  /// Returns whether the var is settable in the specified context: this
  /// is either because it is a stored var, because it has a custom setter, or
  /// is a let member in an initializer.
  ///
  /// Pass a null context and null base to check if it's always settable.
  bool isSettable(const DeclContext *UseDC,
                  const DeclRefExpr *base = nullptr) const;

  /// Return the parent pattern binding that may provide an initializer for this
  /// VarDecl.  This returns null if there is none associated with the VarDecl.
  PatternBindingDecl *getParentPatternBinding() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<PatternBindingDecl *>();
  }
  void setParentPatternBinding(PatternBindingDecl *PBD) {
    assert(PBD);
    Parent = PBD;
  }

  /// Return the Pattern involved in initializing this VarDecl.  However, recall
  /// that the Pattern may be involved in initializing more than just this one
  /// vardecl.  For example, if this is a VarDecl for "x", the pattern may be
  /// "(x, y)" and the initializer on the PatternBindingDecl may be "(1,2)" or
  /// "foo()".
  ///
  /// If this has no parent pattern binding decl or statement associated, it
  /// returns null.
  ///
  Pattern *getParentPattern() const;

  /// Return the statement that owns the pattern associated with this VarDecl,
  /// if one exists.
  ///
  /// NOTE: After parsing and before type checking, all VarDecls from
  /// CaseLabelItem's Patterns return their CaseStmt. After type checking, we
  /// will have constructed the CaseLabelItem VarDecl linked list implying this
  /// will return nullptr. After type checking, if one wishes to find a parent
  /// pattern of a VarDecl of a CaseStmt, \see getRecursiveParentPatternStmt
  /// instead.
  Stmt *getParentPatternStmt() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<Stmt *>();
  }

  void setParentPatternStmt(Stmt *s) {
    assert(s);
    Parent = s;
  }

  /// Look for the parent pattern stmt of this var decl, recursively
  /// looking through var decl pointers and then through any
  /// fallthroughts.
  Stmt *getRecursiveParentPatternStmt() const;

  /// Returns the var decl that this var decl is an implicit reference to if
  /// such a var decl exists.
  VarDecl *getParentVarDecl() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<VarDecl *>();
  }

  /// Set \p v to be the pattern produced VarDecl that is the parent of this
  /// var decl.
  void setParentVarDecl(VarDecl *v) {
    assert(v && v != this);
    Parent = v;
  }

  /// If this is a VarDecl that does not belong to a CaseLabelItem's pattern,
  /// return this. Otherwise, this VarDecl must belong to a CaseStmt's
  /// CaseLabelItem. In that case, return the first case label item of the first
  /// case stmt in a sequence of case stmts that fallthrough into each other.
  ///
  /// NOTE: During type checking, we emit an error if we have a single case
  /// label item with a pattern that has multiple var decls of the same
  /// name. This means that during type checking and before type checking, we
  /// may have a _malformed_ switch stmt var decl linked list since var decls in
  /// the same case label item that have the same name will point at the same
  /// canonical var decl, namely the first var decl with the name in the
  /// canonical case label item's var decl list. This is ok, since we are going
  /// to emit the error, but it requires us to be more careful/cautious before
  /// type checking has been complete when relying on canonical var decls
  /// matching up.
  VarDecl *getCanonicalVarDecl() const;

  /// If this is a case stmt var decl, return the var decl that corresponds to
  /// this var decl in the first case label item of the case stmt. Returns
  /// nullptr if this isn't a VarDecl that is part of a case stmt.
  NullablePtr<VarDecl> getCorrespondingFirstCaseLabelItemVarDecl() const;

  /// If this is a case stmt var decl, return the case body var decl that this
  /// var decl maps to.
  NullablePtr<VarDecl> getCorrespondingCaseBodyVariable() const;

  /// Return true if this var decl is an implicit var decl belonging to a case
  /// stmt's body.
  bool isCaseBodyVariable() const;

  /// True if the global stored property requires lazy initialization.
  bool isLazilyInitializedGlobal() const;

  /// Return the initializer involved in this VarDecl.  Recall that the
  /// initializer may be involved in initializing more than just this one
  /// vardecl though.  For example, if this is a VarDecl for "x", the pattern
  /// may be "(x, y)" and the initializer on the PatternBindingDecl may be
  /// "(1,2)" or "foo()".
  ///
  /// If this has no parent pattern binding decl associated, or if that pattern
  /// binding has no initial value, this returns null.
  ///
  Expr *getParentInitializer() const {
    if (auto *PBD = getParentPatternBinding())
      return PBD->getPatternEntryForVarDecl(this).getInit();
    return nullptr;
  }

  /// Whether there exists an initializer for this \c VarDecl.
  bool isParentInitialized() const {
    if (auto *PBD = getParentPatternBinding())
      return PBD->getPatternEntryForVarDecl(this).isInitialized();
    return false;
  }

  // Return whether this VarDecl has an initial value, either by checking
  // if it has an initializer in its parent pattern binding or if it has
  // the @_hasInitialValue attribute.
  bool hasInitialValue() const {
    return getAttrs().hasAttribute<HasInitialValueAttr>() ||
           isParentInitialized();
  }

  VarDecl *getOverriddenDecl() const {
    return cast_or_null<VarDecl>(AbstractStorageDecl::getOverriddenDecl());
  }
  
  /// Is this an immutable 'let' property?
  ///
  /// If this is a ParamDecl, isLet() is true iff
  /// getSpecifier() == Specifier::Default.
  bool isLet() const { return getIntroducer() == Introducer::Let; }

  Introducer getIntroducer() const {
    return Introducer(Bits.VarDecl.Introducer);
  }

  void setIntroducer(Introducer value) {
    Bits.VarDecl.Introducer = uint8_t(value);
  }

  /// Is this an element in a capture list?
  bool isCaptureList() const { return Bits.VarDecl.IsCaptureList; }

  /// Return true if this vardecl has an initial value bound to it in a way
  /// that isn't represented in the AST with an initializer in the pattern
  /// binding.  This happens in cases like "for i in ...", switch cases, etc.
  bool hasNonPatternBindingInit() const {
    return Bits.VarDecl.HasNonPatternBindingInit;
  }
  void setHasNonPatternBindingInit(bool V = true) {
    Bits.VarDecl.HasNonPatternBindingInit = V;
  }

  /// Determines if this var has an initializer expression that should be
  /// exposed to clients.
  /// There's a very narrow case when we would: if the decl is an instance
  /// member with an initializer expression and the parent type is
  /// @frozen and resides in a resilient module.
  bool isInitExposedToClients() const;
  
  /// Is this a special debugger variable?
  bool isDebuggerVar() const { return Bits.VarDecl.IsDebuggerVar; }
  void setDebuggerVar(bool IsDebuggerVar) {
    Bits.VarDecl.IsDebuggerVar = IsDebuggerVar;
  }

  /// Is this the synthesized storage for a 'lazy' property?
  bool isLazyStorageProperty() const {
    return Bits.VarDecl.IsLazyStorageProperty;
  }
  void setLazyStorageProperty(bool IsLazyStorage) {
    Bits.VarDecl.IsLazyStorageProperty = IsLazyStorage;
  }

  /// Retrieve the custom attributes that attach property wrappers to this
  /// property. The returned list contains all of the attached property wrapper attributes in source order,
  /// which means the outermost wrapper attribute is provided first.
  llvm::TinyPtrVector<CustomAttr *> getAttachedPropertyWrappers() const;

  /// Whether this property has any attached property wrappers.
  bool hasAttachedPropertyWrapper() const;
  
  /// Whether all of the attached property wrappers have an init(initialValue:) initializer.
  bool allAttachedPropertyWrappersHaveInitialValueInit() const;
  
  /// Retrieve the type of the attached property wrapper as a contextual
  /// type.
  ///
  /// \param index Which property wrapper type is being computed, where 0
  /// indicates the first (outermost) attached property wrapper.
  ///
  /// \returns a NULL type for properties without attached wrappers,
  /// an error type when the property wrapper type itself is erroneous,
  /// or the wrapper type itself, which may involve unbound generic
  /// types.
  Type getAttachedPropertyWrapperType(unsigned index) const;

  /// Retrieve information about the attached property wrapper type.
  ///
  /// \param i Which attached property wrapper type is being queried, where 0 is the outermost (first)
  /// attached property wrapper type.
  PropertyWrapperTypeInfo getAttachedPropertyWrapperTypeInfo(unsigned i) const;

  /// Retrieve the fully resolved attached property wrapper type.
  ///
  /// This type will be the fully-resolved form of
  /// \c getAttachedPropertyWrapperType(0), which will not contain any
  /// unbound generic types. It will be the type of the backing property.
  Type getPropertyWrapperBackingPropertyType() const;

  /// Retrieve information about the backing properties of the attached
  /// property wrapper.
  PropertyWrapperBackingPropertyInfo
      getPropertyWrapperBackingPropertyInfo() const;

  /// Retrieve information about the mutability of the composed
  /// property wrappers.
  Optional<PropertyWrapperMutability>
      getPropertyWrapperMutability() const;

  /// Retrieve the backing storage property for a property that has an
  /// attached property wrapper.
  ///
  /// The backing storage property will be a stored property of the
  /// wrapper's type. This will be equivalent to
  /// \c getAttachedPropertyWrapperType(0) when it is fully-specified;
  /// if \c getAttachedPropertyWrapperType(0) involves an unbound
  /// generic type, the backing storage property will be the appropriate
  /// bound generic version.
  VarDecl *getPropertyWrapperBackingProperty() const;

  /// Retreive the storage wrapper for a property that has an attached
  /// property wrapper.
  VarDecl *getPropertyWrapperStorageWrapper() const;

  /// Retrieve the backing storage property for a lazy property.
  VarDecl *getLazyStorageProperty() const;

  /// Whether this is a property with a property wrapper that was initialized
  /// via a value of the original type, e.g.,
  ///
  /// \code
  /// @Lazy var i = 17
  /// \end
  bool isPropertyWrapperInitializedWithInitialValue() const;

  /// Whether the memberwise initializer parameter for a property with a property wrapper type
  /// uses the wrapped type.
  bool isPropertyMemberwiseInitializedWithWrappedType() const;

  /// If this property is the backing storage for a property with an attached
  /// property wrapper, return the original property.
  ///
  /// \param kind If not \c None, only returns the original property when
  /// \c this property is the specified synthesized property.
  VarDecl *getOriginalWrappedProperty(
      Optional<PropertyWrapperSynthesizedPropertyKind> kind = None) const;

  /// Set the property that wraps to this property as it's backing
  /// property.
  void setOriginalWrappedProperty(VarDecl *originalProperty);

  /// Return the Objective-C runtime name for this property.
  Identifier getObjCPropertyName() const;

  /// Retrieve the default Objective-C selector for the getter of a
  /// property of the given name.
  static ObjCSelector getDefaultObjCGetterSelector(ASTContext &ctx,
                                                   Identifier propertyName);

  /// Retrieve the default Objective-C selector for the setter of a
  /// property of the given name.
  static ObjCSelector getDefaultObjCSetterSelector(ASTContext &ctx,
                                                   Identifier propertyName);

  /// If this is a simple 'let' constant, emit a note with a fixit indicating
  /// that it can be rewritten to a 'var'.  This is used in situations where the
  /// compiler detects obvious attempts to mutate a constant.
  void emitLetToVarNoteIfSimple(DeclContext *UseDC) const;

  /// Returns true if the name is the self identifier and is implicit.
  bool isSelfParameter() const;

  /// Determine whether this property will be part of the implicit memberwise
  /// initializer.
  ///
  /// \param preferDeclaredProperties When encountering a `lazy` property
  /// or a property that has an attached property wrapper, prefer the
  /// actual declared property (which may or may not be considered "stored"
  /// as the moment) to the backing storage property. Otherwise, the stored
  /// backing property will be treated as the member-initialized property.
  bool isMemberwiseInitialized(bool preferDeclaredProperties) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { 
    return D->getKind() == DeclKind::Var || D->getKind() == DeclKind::Param; 
  }
};

/// A function parameter declaration.
class ParamDecl : public VarDecl {
  Identifier ArgumentName;
  SourceLoc ArgumentNameLoc;
  SourceLoc SpecifierLoc;

  struct StoredDefaultArgument {
    PointerUnion<Expr *, VarDecl *> DefaultArg;
    Initializer *InitContext = nullptr;
    StringRef StringRepresentation;
  };

  enum class Flags : uint8_t {
    /// Whether or not this parameter is vargs.
    IsVariadic = 1 << 0,

    /// Whether or not this parameter is `@autoclosure`.
    IsAutoClosure = 1 << 1,
  };

  /// The default value, if any, along with flags.
  llvm::PointerIntPair<StoredDefaultArgument *, 2, OptionSet<Flags>>
      DefaultValueAndFlags;

public:
  enum class Specifier : uint8_t {
    Default = 0,
    InOut = 1,
    Shared = 2,
    Owned = 3,
  };

  ParamDecl(Specifier specifier,
            SourceLoc specifierLoc, SourceLoc argumentNameLoc,
            Identifier argumentName, SourceLoc parameterNameLoc,
            Identifier parameterName, DeclContext *dc);

  /// Clone constructor, allocates a new ParamDecl identical to the first.
  /// Intentionally not defined as a typical copy constructor to avoid
  /// accidental copies.
  ParamDecl(ParamDecl *PD, bool withTypes);
  
  /// Retrieve the argument (API) name for this function parameter.
  Identifier getArgumentName() const { return ArgumentName; }

  /// Retrieve the parameter (local) name for this function parameter.
  Identifier getParameterName() const { return getName(); }

  /// Retrieve the source location of the argument (API) name.
  ///
  /// The resulting source location will be valid if the argument name
  /// was specified separately from the parameter name.
  SourceLoc getArgumentNameLoc() const { return ArgumentNameLoc; }
  
  SourceLoc getSpecifierLoc() const { return SpecifierLoc; }
    
  bool isTypeLocImplicit() const { return Bits.ParamDecl.IsTypeLocImplicit; }
  void setIsTypeLocImplicit(bool val) { Bits.ParamDecl.IsTypeLocImplicit = val; }
  
  DefaultArgumentKind getDefaultArgumentKind() const {
    return static_cast<DefaultArgumentKind>(Bits.ParamDecl.defaultArgumentKind);
  }
  bool isDefaultArgument() const {
    return getDefaultArgumentKind() != DefaultArgumentKind::None;
  }
  void setDefaultArgumentKind(DefaultArgumentKind K) {
    Bits.ParamDecl.defaultArgumentKind = static_cast<unsigned>(K);
  }
  
  Expr *getDefaultValue() const {
    if (auto stored = DefaultValueAndFlags.getPointer())
      return stored->DefaultArg.dyn_cast<Expr *>();
    return nullptr;
  }

  VarDecl *getStoredProperty() const {
    if (auto stored = DefaultValueAndFlags.getPointer())
      return stored->DefaultArg.dyn_cast<VarDecl *>();
    return nullptr;
  }

  void setDefaultValue(Expr *E);

  void setStoredProperty(VarDecl *var);

  Initializer *getDefaultArgumentInitContext() const {
    if (auto stored = DefaultValueAndFlags.getPointer())
      return stored->InitContext;
    return nullptr;
  }

  void setDefaultArgumentInitContext(Initializer *initContext);

  /// Extracts the text of the default argument attached to the provided
  /// ParamDecl, removing all inactive #if clauses and providing only the
  /// text of active #if clauses.
  ///
  /// For example, the default argument:
  /// ```
  /// {
  ///   #if false
  ///   print("false")
  ///   #else
  ///   print("true")
  ///   #endif
  /// }
  /// ```
  /// will return
  /// ```
  /// {
  ///   print("true")
  /// }
  /// ```
  /// \sa getDefaultValue
  StringRef getDefaultValueStringRepresentation(
    SmallVectorImpl<char> &scratch) const;

  void setDefaultValueStringRepresentation(StringRef stringRepresentation);

  /// Whether or not this parameter is varargs.
  bool isVariadic() const {
    return DefaultValueAndFlags.getInt().contains(Flags::IsVariadic);
  }
  void setVariadic(bool value = true) {
    auto flags = DefaultValueAndFlags.getInt();
    DefaultValueAndFlags.setInt(value ? flags | Flags::IsVariadic
                                      : flags - Flags::IsVariadic);
  }

  /// Whether or not this parameter is marked with `@autoclosure`.
  bool isAutoClosure() const {
    return DefaultValueAndFlags.getInt().contains(Flags::IsAutoClosure);
  }
  void setAutoClosure(bool value = true) {
    auto flags = DefaultValueAndFlags.getInt();
    DefaultValueAndFlags.setInt(value ? flags | Flags::IsAutoClosure
                                      : flags - Flags::IsAutoClosure);
  }

  /// Remove the type of this varargs element designator, without the array
  /// type wrapping it.  A parameter like "Int..." will have formal parameter
  /// type of "[Int]" and this returns "Int".
  static Type getVarargBaseTy(Type VarArgT);
  
  /// Remove the type of this varargs element designator, without the array
  /// type wrapping it.
  Type getVarargBaseTy() const {
    assert(isVariadic());
    return getVarargBaseTy(getInterfaceType());
  }

  /// Determine whether this declaration is an anonymous closure parameter.
  bool isAnonClosureParam() const;

  /// Return the raw specifier value for this parameter.
  Specifier getSpecifier() const {
    return static_cast<Specifier>(Bits.ParamDecl.Specifier);
  }
  void setSpecifier(Specifier Spec);

  /// Is the type of this parameter 'inout'?
  bool isInOut() const { return getSpecifier() == Specifier::InOut; }
  /// Is this an immutable 'shared' property?
  bool isShared() const { return getSpecifier() == Specifier::Shared; }
  /// Is this an immutable 'owned' property?
  bool isOwned() const { return getSpecifier() == Specifier::Owned; }

  bool isImmutable() const {
    return isImmutableSpecifier(getSpecifier());
  }
  static bool isImmutableSpecifier(Specifier sp) {
    switch (sp) {
    case Specifier::Default:
    case Specifier::Shared:
    case Specifier::Owned:
      return true;
    case Specifier::InOut:
      return false;
    }
    llvm_unreachable("unhandled specifier");
  }

  ValueOwnership getValueOwnership() const {
    return getValueOwnershipForSpecifier(getSpecifier());
  }

  static ValueOwnership getValueOwnershipForSpecifier(Specifier specifier) {
    switch (specifier) {
    case Specifier::Default:
      return ValueOwnership::Default;
    case Specifier::InOut:
      return ValueOwnership::InOut;
    case Specifier::Shared:
      return ValueOwnership::Shared;
    case Specifier::Owned:
      return ValueOwnership::Owned;
    }
    llvm_unreachable("unhandled specifier");
  }

  static Specifier
  getParameterSpecifierForValueOwnership(ValueOwnership ownership) {
    switch (ownership) {
    case ValueOwnership::Default:
      return Specifier::Default;
    case ValueOwnership::Shared:
      return Specifier::Shared;
    case ValueOwnership::InOut:
      return Specifier::InOut;
    case ValueOwnership::Owned:
      return Specifier::Owned;
    }
    llvm_unreachable("unhandled ownership");
  }

  SourceRange getSourceRange() const;

  AnyFunctionType::Param toFunctionParam(Type type = Type()) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { 
    return D->getKind() == DeclKind::Param;
  }
};
  
/// Describes the kind of subscripting used in Objective-C.
enum class ObjCSubscriptKind {
  /// Objective-C indexed subscripting, which is based on an integral
  /// index.
  Indexed,
  /// Objective-C keyed subscripting, which is based on an object
  /// argument or metatype thereof.
  Keyed
};

/// Declares a subscripting operator for a type.
///
/// A subscript declaration is defined as a get/set pair that produces a
/// specific type. For example:
///
/// \code
/// subscript (i : Int) -> String {
///   get { /* return ith String */ }
///   set { /* set ith string to value */ }
/// }
/// \endcode
///
/// A type with a subscript declaration can be used as the base of a subscript
/// expression a[i], where a is of the subscriptable type and i is the type
/// of the index. A subscript can have multiple indices:
///
/// \code
/// struct Matrix {
///   subscript (i : Int, j : Int) -> Double {
///     get { /* return element at position (i, j) */ }
///     set { /* set element at position (i, j) */ }
///   }
/// }
/// \endcode
///
/// A given type can have multiple subscript declarations, so long as the
/// signatures (indices and element type) are distinct.
///
class SubscriptDecl : public GenericContext, public AbstractStorageDecl {
  SourceLoc StaticLoc;
  SourceLoc ArrowLoc;
  SourceLoc EndLoc;
  ParameterList *Indices;
  TypeLoc ElementTy;

public:
  SubscriptDecl(DeclName Name,
                SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
                SourceLoc SubscriptLoc, ParameterList *Indices,
                SourceLoc ArrowLoc, TypeLoc ElementTy, DeclContext *Parent,
                GenericParamList *GenericParams)
    : GenericContext(DeclContextKind::SubscriptDecl, Parent),
      AbstractStorageDecl(DeclKind::Subscript,
                          StaticSpelling != StaticSpellingKind::None,
                          Parent, Name, SubscriptLoc,
                          /*will be overwritten*/ StorageIsNotMutable),
      StaticLoc(StaticLoc), ArrowLoc(ArrowLoc),
      Indices(nullptr), ElementTy(ElementTy) {
    Bits.SubscriptDecl.StaticSpelling = static_cast<unsigned>(StaticSpelling);
    setIndices(Indices);
    setGenericParams(GenericParams);
  }
  
  /// \returns the way 'static'/'class' was spelled in the source.
  StaticSpellingKind getStaticSpelling() const {
    return static_cast<StaticSpellingKind>(Bits.SubscriptDecl.StaticSpelling);
  }
  
  SourceLoc getStaticLoc() const { return StaticLoc; }
  SourceLoc getSubscriptLoc() const { return getNameLoc(); }
  
  SourceLoc getStartLoc() const {
    return getStaticLoc().isValid() ? getStaticLoc() : getSubscriptLoc();
  }
  SourceLoc getEndLoc() const { return EndLoc; }

  void setEndLoc(SourceLoc sl) { EndLoc = sl; }
  SourceRange getSourceRange() const;
  SourceRange getSignatureSourceRange() const;

  /// Retrieve the indices for this subscript operation.
  ParameterList *getIndices() { return Indices; }
  const ParameterList *getIndices() const { return Indices; }
  void setIndices(ParameterList *p);

  /// Retrieve the type of the element referenced by a subscript
  /// operation.
  Type getElementInterfaceType() const;
  TypeLoc &getElementTypeLoc() { return ElementTy; }
  const TypeLoc &getElementTypeLoc() const { return ElementTy; }

  /// Compute the interface type of this subscript from the parameter and
  /// element types.
  void computeType();

  /// Determine the kind of Objective-C subscripting this declaration
  /// implies.
  ObjCSubscriptKind getObjCSubscriptKind() const;

  SubscriptDecl *getOverriddenDecl() const {
    return cast_or_null<SubscriptDecl>(
                                  AbstractStorageDecl::getOverriddenDecl());
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Subscript;
  }
  
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  using DeclContext::operator new;
  using Decl::getASTContext;
};

/// Encodes imported-as-member status for C functions that get imported
/// as methods.
class ImportAsMemberStatus {
  friend class AbstractFunctionDecl;

  // non-0 denotes import-as-member. 1 denotes no self index. n+2 denotes self
  // index of n
  uint8_t rawValue;

public:
  ImportAsMemberStatus(uint8_t rawValue = 0) : rawValue(rawValue) {}

  uint8_t getRawValue() const { return rawValue; }

  bool isImportAsMember() const { return rawValue != 0; }
  bool isInstance() const { return rawValue >= 2; }
  bool isStatic() const { return rawValue == 1; }
  uint8_t getSelfIndex() const {
    assert(isInstance() && "not set");
    return rawValue - 2;
  }
  void setStatic() {
    assert(!isStatic() && "already set");
    rawValue = 1;
  }
  void setSelfIndex(uint8_t idx) {
    assert(!isImportAsMember() && "already set");
    assert(idx <= UINT8_MAX-2 && "out of bounds");
    rawValue = idx + 2;
  }
};

/// Base class for function-like declarations.
class AbstractFunctionDecl : public GenericContext, public ValueDecl {
public:
  enum class BodyKind {
    /// The function did not have a body in the source code file.
    None,

    /// Function body is delayed, to be parsed later.
    Unparsed,

    /// Function body is parsed and available as an AST subtree.
    Parsed,

    /// Function body is not available, although it was written in the source.
    Skipped,

    /// Function body will be synthesized on demand.
    Synthesize,

    /// Function body is present and type-checked.
    TypeChecked,

    /// This is a memberwise initializer that will be synthesized by SILGen.
    MemberwiseInitializer,

    /// Function body text was deserialized from a .swiftmodule.
    Deserialized

    // This enum currently needs to fit in a 3-bit bitfield.
  };

  BodyKind getBodyKind() const {
    return BodyKind(Bits.AbstractFunctionDecl.BodyKind);
  }

  struct BodySynthesizer {
    std::pair<BraceStmt *, bool> (* Fn)(AbstractFunctionDecl *, void *);
    void *Context;
  };

private:
  ParameterList *Params;

protected:
  // If a function has a body at all, we have either a parsed body AST node or
  // we have saved the end location of the unparsed body.
  union {
    /// This enum member is active if getBodyKind() is BodyKind::Parsed or
    /// BodyKind::TypeChecked.
    BraceStmt *Body;

    /// This enum member is active if getBodyKind() is BodyKind::Deserialized.
    StringRef BodyStringRepresentation;

    /// This enum member is active if getBodyKind() == BodyKind::Synthesize.
    BodySynthesizer Synthesizer;

    /// The location of the function body when the body is delayed or skipped.
    ///
    /// This enum member is active if getBodyKind() is BodyKind::Unparsed or
    /// BodyKind::Skipped.
    SourceRange BodyRange;
  };

  CaptureInfo Captures;

  /// Location of the 'throws' token.
  SourceLoc ThrowsLoc;

  AbstractFunctionDecl(DeclKind Kind, DeclContext *Parent, DeclName Name,
                       SourceLoc NameLoc, bool Throws, SourceLoc ThrowsLoc,
                       bool HasImplicitSelfDecl,
                       GenericParamList *GenericParams)
      : GenericContext(DeclContextKind::AbstractFunctionDecl, Parent),
        ValueDecl(Kind, Parent, Name, NameLoc),
        Body(nullptr), ThrowsLoc(ThrowsLoc) {
    setBodyKind(BodyKind::None);
    setGenericParams(GenericParams);
    Bits.AbstractFunctionDecl.HasImplicitSelfDecl = HasImplicitSelfDecl;
    Bits.AbstractFunctionDecl.Overridden = false;
    Bits.AbstractFunctionDecl.Throws = Throws;
    Bits.AbstractFunctionDecl.NeedsNewVTableEntry = false;
    Bits.AbstractFunctionDecl.HasComputedNeedsNewVTableEntry = false;
    Bits.AbstractFunctionDecl.Synthesized = false;
    Bits.AbstractFunctionDecl.HasSingleExpressionBody = false;
  }

  void setBodyKind(BodyKind K) {
    Bits.AbstractFunctionDecl.BodyKind = unsigned(K);
  }

public:
  void setHasSingleExpressionBody(bool Has = true) { 
    Bits.AbstractFunctionDecl.HasSingleExpressionBody = Has;
  }

  bool hasSingleExpressionBody() const {
    return Bits.AbstractFunctionDecl.HasSingleExpressionBody;
  }

  Expr *getSingleExpressionBody() const;
  void setSingleExpressionBody(Expr *NewBody);

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    assert(!getFullName().isSpecial() && "Cannot get string for special names");
    return hasName() ? getBaseName().getIdentifier().str() : "_";
  }

  /// Should this declaration be treated as if annotated with transparent
  /// attribute.
  bool isTransparent() const;

  // Expose our import as member status
  ImportAsMemberStatus getImportAsMemberStatus() const {
    return ImportAsMemberStatus(Bits.AbstractFunctionDecl.IAMStatus);
  }
  bool isImportAsMember() const {
    return getImportAsMemberStatus().isImportAsMember();
  }
  bool isImportAsInstanceMember() const {
    return getImportAsMemberStatus().isInstance();
  }
  bool isImportAsStaticMember() const {
    return getImportAsMemberStatus().isStatic();
  }
  uint8_t getSelfIndex() const {
    return getImportAsMemberStatus().getSelfIndex();
  }

  void setImportAsStaticMember() {
    auto newValue = getImportAsMemberStatus();
    newValue.setStatic();
    Bits.AbstractFunctionDecl.IAMStatus = newValue.getRawValue();
  }
  void setSelfIndex(uint8_t idx) {
    auto newValue = getImportAsMemberStatus();
    newValue.setSelfIndex(idx);
    Bits.AbstractFunctionDecl.IAMStatus = newValue.getRawValue();
  }

  /// Retrieve the location of the 'throws' keyword, if present.
  SourceLoc getThrowsLoc() const { return ThrowsLoc; }

  /// Returns true if the function body throws.
  bool hasThrows() const { return Bits.AbstractFunctionDecl.Throws; }

  // FIXME: Hack that provides names with keyword arguments for accessors.
  DeclName getEffectiveFullName() const;

  /// Returns true if the function has a body written in the source file.
  ///
  /// Note that a true return value does not imply that the body was actually
  /// parsed.
  bool hasBody() const {
    return getBodyKind() != BodyKind::None;
  }

  /// Returns true if the text of this function's body can be retrieved either
  /// by extracting the text from the source buffer or reading the inlinable
  /// body from a deserialized swiftmodule.
  bool hasInlinableBodyText() const;

  /// Returns the function body, if it was parsed, or nullptr otherwise.
  ///
  /// Note that a null return value does not imply that the source code did not
  /// have a body for this function.
  ///
  /// \sa hasBody()
  BraceStmt *getBody(bool canSynthesize = true) const;

  void setBody(BraceStmt *S, BodyKind NewBodyKind = BodyKind::Parsed) {
    assert(getBodyKind() != BodyKind::Skipped &&
           "cannot set a body if it was skipped");

    Body = S;
    setBodyKind(NewBodyKind);
  }

  /// Note that the body was skipped for this function.  Function body
  /// cannot be attached after this call.
  void setBodySkipped(SourceRange bodyRange) {
    assert(getBodyKind() == BodyKind::None);
    BodyRange = bodyRange;
    setBodyKind(BodyKind::Skipped);
  }

  /// Note that parsing for the body was delayed.
  void setBodyDelayed(SourceRange bodyRange) {
    assert(getBodyKind() == BodyKind::None);
    BodyRange = bodyRange;
    setBodyKind(BodyKind::Unparsed);
  }

  /// Provide the parsed body for the function.
  void setBodyParsed(BraceStmt *S) {
    setBody(S, BodyKind::Parsed);
  }

  /// Note that parsing for the body was delayed.
  ///
  /// The function should return the body statement and a flag indicating
  /// whether that body is already type-checked.
  void setBodySynthesizer(
      std::pair<BraceStmt *, bool> (* fn)(AbstractFunctionDecl *, void *),
      void *context = nullptr) {
    assert(getBodyKind() == BodyKind::None);
    Synthesizer = {fn, context};
    setBodyKind(BodyKind::Synthesize);
  }

  /// Note that this is a memberwise initializer and thus the body will be
  /// generated by SILGen.
  void setIsMemberwiseInitializer() {
    assert(getBodyKind() == BodyKind::None);
    assert(isa<ConstructorDecl>(this));
    setBodyKind(BodyKind::MemberwiseInitializer);
  }

  /// Gets the body of this function, stripping the unused portions of #if
  /// configs inside the body. If this function was not deserialized from a
  /// .swiftmodule, this body is reconstructed from the original
  /// source buffer.
  StringRef getInlinableBodyText(SmallVectorImpl<char> &scratch) const;

  void setBodyStringRepresentation(StringRef body) {
    assert(getBodyKind() == BodyKind::None);
    setBodyKind(BodyKind::Deserialized);
    BodyStringRepresentation = body;
  }

  bool isBodyTypeChecked() const {
    return getBodyKind() == BodyKind::TypeChecked;
  }

  bool isMemberwiseInitializer() const {
    return getBodyKind() == BodyKind::MemberwiseInitializer;
  }

  void setNeedsNewVTableEntry(bool value) {
    Bits.AbstractFunctionDecl.HasComputedNeedsNewVTableEntry = true;
    Bits.AbstractFunctionDecl.NeedsNewVTableEntry = value;
  }

  bool needsNewVTableEntry() const {
    if (!Bits.AbstractFunctionDecl.HasComputedNeedsNewVTableEntry)
      const_cast<AbstractFunctionDecl *>(this)->computeNeedsNewVTableEntry();
    return Bits.AbstractFunctionDecl.NeedsNewVTableEntry;
  }

  bool isEffectiveLinkageMoreVisibleThan(ValueDecl *other) const {
    return (std::min(getEffectiveAccess(), AccessLevel::Public) >
            std::min(other->getEffectiveAccess(), AccessLevel::Public));
  }

  bool isSynthesized() const {
    return Bits.AbstractFunctionDecl.Synthesized;
  }

  void setSynthesized(bool value = true) {
    Bits.AbstractFunctionDecl.Synthesized = value;
  }

private:
  void computeNeedsNewVTableEntry();

  void computeSelfDeclType();

public:
  /// Compute the interface type of this function declaration from the
  /// parameter types.
  void computeType(AnyFunctionType::ExtInfo Info = FunctionType::ExtInfo());

  /// Retrieve the source range of the function body.
  SourceRange getBodySourceRange() const;

  /// Retrieve the source range of the function declaration name + patterns.
  SourceRange getSignatureSourceRange() const;

  const CaptureInfo &getCaptureInfo() const { return Captures; }
  void setCaptureInfo(const CaptureInfo &captures) { Captures = captures; }

  /// Retrieve the Objective-C selector that names this method.
  ObjCSelector getObjCSelector(DeclName preferredName = DeclName(),
                               bool skipIsObjCResolution = false) const;

  /// Determine whether the given method would produce an Objective-C
  /// instance method.
  bool isObjCInstanceMethod() const;

  /// Determine whether the name of an argument is an API name by default
  /// depending on the function context.
  bool argumentNameIsAPIByDefault() const;

  /// Retrieve the function's parameter list, not including 'self' if present.
  ParameterList *getParameters() {
    return Params;
  }
  const ParameterList *getParameters() const {
    return Params;
  }

  void setParameters(ParameterList *Params);

  bool hasImplicitSelfDecl() const {
    return Bits.AbstractFunctionDecl.HasImplicitSelfDecl;
  }

  ParamDecl **getImplicitSelfDeclStorage();

  /// Retrieve the implicit 'self' parameter for methods. Returns nullptr for
  /// free functions.
  const ParamDecl *getImplicitSelfDecl(bool createIfNeeded=true) const {
    return const_cast<AbstractFunctionDecl*>(this)
        ->getImplicitSelfDecl(createIfNeeded);
  }
  ParamDecl *getImplicitSelfDecl(bool createIfNeeded=true);

  /// Retrieve the declaration that this method overrides, if any.
  AbstractFunctionDecl *getOverriddenDecl() const {
    return cast_or_null<AbstractFunctionDecl>(ValueDecl::getOverriddenDecl());
  }

  /// Whether the declaration is later overridden in the module
  ///
  /// Overrides are resolved during type checking; only query this field after
  /// the whole module has been checked
  bool isOverridden() const { return Bits.AbstractFunctionDecl.Overridden; }

  /// The declaration has been overridden in the module
  ///
  /// Resolved during type checking
  void setIsOverridden() { Bits.AbstractFunctionDecl.Overridden = true; }

  /// Set information about the foreign error convention used by this
  /// declaration.
  void setForeignErrorConvention(const ForeignErrorConvention &convention);

  /// Get information about the foreign error convention used by this
  /// declaration, given that it is @objc and 'throws'.
  Optional<ForeignErrorConvention> getForeignErrorConvention() const;

  /// If this is a foreign C function imported as a method, get the index of
  /// the foreign parameter imported as `self`. If the function is imported
  /// as a static method, `-1` is returned to represent the `self` parameter
  /// being dropped altogether. `None` is returned for a normal function
  /// or method.
  Optional<int> getForeignFunctionAsMethodSelfParameterIndex() const;
  
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_AbstractFunctionDecl &&
           D->getKind() <= DeclKind::Last_AbstractFunctionDecl;
  }

  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
  
  /// True if the declaration is forced to be statically dispatched.
  bool hasForcedStaticDispatch() const;

  /// Get the type of this declaration without the Self clause.
  /// Asserts if not in type context.
  Type getMethodInterfaceType() const;

  /// Tests if this is a function returning a DynamicSelfType, or a
  /// constructor.
  bool hasDynamicSelfResult() const;

  using DeclContext::operator new;
  using Decl::getASTContext;
};

class OperatorDecl;

enum class SelfAccessKind : uint8_t {
  NonMutating,
  Mutating,
  __Consuming,
};

/// Diagnostic printing of \c SelfAccessKind.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SelfAccessKind SAK);
  
/// FuncDecl - 'func' declaration.
class FuncDecl : public AbstractFunctionDecl {
  friend class AbstractFunctionDecl;
  friend class SelfAccessKindRequest;

  SourceLoc StaticLoc;  // Location of the 'static' token or invalid.
  SourceLoc FuncLoc;    // Location of the 'func' token.

  TypeLoc FnRetType;

  OperatorDecl *Operator = nullptr;
  OpaqueTypeDecl *OpaqueReturn = nullptr;

protected:
  FuncDecl(DeclKind Kind,
           SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
           SourceLoc FuncLoc,
           DeclName Name, SourceLoc NameLoc,
           bool Throws, SourceLoc ThrowsLoc,
           bool HasImplicitSelfDecl,
           GenericParamList *GenericParams, DeclContext *Parent)
    : AbstractFunctionDecl(Kind, Parent,
                           Name, NameLoc,
                           Throws, ThrowsLoc,
                           HasImplicitSelfDecl, GenericParams),
      StaticLoc(StaticLoc), FuncLoc(FuncLoc) {
    assert(!Name.getBaseName().isSpecial());

    Bits.FuncDecl.IsStatic =
      StaticLoc.isValid() || StaticSpelling != StaticSpellingKind::None;
    Bits.FuncDecl.StaticSpelling = static_cast<unsigned>(StaticSpelling);

    Bits.FuncDecl.ForcedStaticDispatch = false;
    Bits.FuncDecl.SelfAccess =
      static_cast<unsigned>(SelfAccessKind::NonMutating);
    Bits.FuncDecl.SelfAccessComputed = false;
  }

private:
  static FuncDecl *createImpl(ASTContext &Context, SourceLoc StaticLoc,
                              StaticSpellingKind StaticSpelling,
                              SourceLoc FuncLoc,
                              DeclName Name, SourceLoc NameLoc,
                              bool Throws, SourceLoc ThrowsLoc,
                              GenericParamList *GenericParams,
                              DeclContext *Parent,
                              ClangNode ClangN);

  Optional<SelfAccessKind> getCachedSelfAccessKind() const {
    if (Bits.FuncDecl.SelfAccessComputed)
      return static_cast<SelfAccessKind>(Bits.FuncDecl.SelfAccess);

    return None;
  }

public:
  /// Factory function only for use by deserialization.
  static FuncDecl *createDeserialized(ASTContext &Context, SourceLoc StaticLoc,
                                      StaticSpellingKind StaticSpelling,
                                      SourceLoc FuncLoc,
                                      DeclName Name, SourceLoc NameLoc,
                                      bool Throws, SourceLoc ThrowsLoc,
                                      GenericParamList *GenericParams,
                                      DeclContext *Parent);

  static FuncDecl *create(ASTContext &Context, SourceLoc StaticLoc,
                          StaticSpellingKind StaticSpelling,
                          SourceLoc FuncLoc,
                          DeclName Name, SourceLoc NameLoc,
                          bool Throws, SourceLoc ThrowsLoc,
                          GenericParamList *GenericParams,
                          ParameterList *ParameterList,
                          TypeLoc FnRetType, DeclContext *Parent,
                          ClangNode ClangN = ClangNode());

  Identifier getName() const { return getFullName().getBaseIdentifier(); }

  bool isStatic() const {
    return Bits.FuncDecl.IsStatic;
  }
  /// \returns the way 'static'/'class' was spelled in the source.
  StaticSpellingKind getStaticSpelling() const {
    return static_cast<StaticSpellingKind>(Bits.FuncDecl.StaticSpelling);
  }
  /// \returns the way 'static'/'class' should be spelled for this declaration.
  StaticSpellingKind getCorrectStaticSpelling() const;
  void setStatic(bool IsStatic = true) {
    Bits.FuncDecl.IsStatic = IsStatic;
  }

  bool isMutating() const {
    return getSelfAccessKind() == SelfAccessKind::Mutating;
  }
  bool isNonMutating() const {
    return getSelfAccessKind() == SelfAccessKind::NonMutating;
  }
  bool isConsuming() const {
    return getSelfAccessKind() == SelfAccessKind::__Consuming;
  }

  SelfAccessKind getSelfAccessKind() const;

  void setSelfAccessKind(SelfAccessKind mod) {
    Bits.FuncDecl.SelfAccess = static_cast<unsigned>(mod);
    Bits.FuncDecl.SelfAccessComputed = true;
  }

  SourceLoc getStaticLoc() const { return StaticLoc; }
  SourceLoc getFuncLoc() const { return FuncLoc; }

  SourceLoc getStartLoc() const {
    return StaticLoc.isValid() && !isa<AccessorDecl>(this)
              ? StaticLoc : FuncLoc;
  }
  SourceRange getSourceRange() const;

  TypeLoc &getBodyResultTypeLoc() { return FnRetType; }
  const TypeLoc &getBodyResultTypeLoc() const { return FnRetType; }

  /// Retrieve the result interface type of this function.
  Type getResultInterfaceType() const;

  /// isUnaryOperator - Determine whether this is a unary operator
  /// implementation.  This check is a syntactic rather than type-based check,
  /// which looks at the number of parameters specified, in order to allow
  /// for the definition of unary operators on tuples, as in:
  ///
  ///   prefix func + (param : (a:Int, b:Int))
  ///
  /// This also allows the unary-operator-ness of a func decl to be determined
  /// prior to type checking.
  bool isUnaryOperator() const;
  
  /// isBinaryOperator - Determine whether this is a binary operator
  /// implementation.  This check is a syntactic rather than type-based check,
  /// which looks at the number of parameters specified, in order to allow
  /// distinguishing a binary operator from a unary operator on tuples, as in:
  ///
  ///   prefix func + (_:(a:Int, b:Int)) // unary operator +(1,2)
  ///   infix func  + (a:Int, b:Int)     // binary operator 1 + 2
  ///
  /// This also allows the binary-operator-ness of a func decl to be determined
  /// prior to type checking.
  bool isBinaryOperator() const;

  void getLocalCaptures(SmallVectorImpl<CapturedValue> &Result) const {
    return getCaptureInfo().getLocalCaptures(Result);
  }

  ParamDecl **getImplicitSelfDeclStorage();

  /// Get the supertype method this method overrides, if any.
  FuncDecl *getOverriddenDecl() const {
    return cast_or_null<FuncDecl>(AbstractFunctionDecl::getOverriddenDecl());
  }

  OperatorDecl *getOperatorDecl() const {
    return Operator;
  }
  void setOperatorDecl(OperatorDecl *o) {
    assert(isOperator() && "can't set an OperatorDecl for a non-operator");
    Operator = o;
  }
  
  OpaqueTypeDecl *getOpaqueResultTypeDecl() const {
    return OpaqueReturn;
  }
  void setOpaqueResultTypeDecl(OpaqueTypeDecl *decl) {
    assert(!OpaqueReturn && "already has opaque type decl");
    OpaqueReturn = decl;
  }
  
  /// Returns true if the function is forced to be statically dispatched.
  bool hasForcedStaticDispatch() const {
    return Bits.FuncDecl.ForcedStaticDispatch;
  }
  void setForcedStaticDispatch(bool flag) {
    Bits.FuncDecl.ForcedStaticDispatch = flag;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Func ||
           D->getKind() == DeclKind::Accessor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  /// True if the function is a defer body.
  bool isDeferBody() const;

  /// Perform basic checking to determine whether the @IBAction or
  /// @IBSegueAction attribute can be applied to this function.
  bool isPotentialIBActionTarget() const;
};

/// This represents an accessor function, such as a getter or setter.
class AccessorDecl final : public FuncDecl {
  /// Location of the accessor keyword, e.g. 'set'.
  SourceLoc AccessorKeywordLoc;

  AbstractStorageDecl *Storage;

  AccessorDecl(SourceLoc declLoc, SourceLoc accessorKeywordLoc,
               AccessorKind accessorKind, AbstractStorageDecl *storage,
               SourceLoc staticLoc, StaticSpellingKind staticSpelling,
               bool throws, SourceLoc throwsLoc,
               bool hasImplicitSelfDecl, GenericParamList *genericParams,
               DeclContext *parent)
    : FuncDecl(DeclKind::Accessor,
               staticLoc, staticSpelling, /*func loc*/ declLoc,
               /*name*/ Identifier(), /*name loc*/ declLoc,
               throws, throwsLoc, hasImplicitSelfDecl, genericParams, parent),
      AccessorKeywordLoc(accessorKeywordLoc),
      Storage(storage) {
    Bits.AccessorDecl.AccessorKind = unsigned(accessorKind);
  }

  static AccessorDecl *createImpl(ASTContext &ctx,
                                  SourceLoc declLoc,
                                  SourceLoc accessorKeywordLoc,
                                  AccessorKind accessorKind,
                                  AbstractStorageDecl *storage,
                                  SourceLoc staticLoc,
                                  StaticSpellingKind staticSpelling,
                                  bool throws, SourceLoc throwsLoc,
                                  GenericParamList *genericParams,
                                  DeclContext *parent,
                                  ClangNode clangNode);

  Optional<bool> getCachedIsTransparent() const {
    if (Bits.AccessorDecl.IsTransparentComputed)
      return Bits.AccessorDecl.IsTransparent;
    return None;
  }

  friend class IsAccessorTransparentRequest;

public:
  static AccessorDecl *createDeserialized(ASTContext &ctx,
                              SourceLoc declLoc,
                              SourceLoc accessorKeywordLoc,
                              AccessorKind accessorKind,
                              AbstractStorageDecl *storage,
                              SourceLoc staticLoc,
                              StaticSpellingKind staticSpelling,
                              bool throws, SourceLoc throwsLoc,
                              GenericParamList *genericParams,
                              DeclContext *parent);

  static AccessorDecl *create(ASTContext &ctx, SourceLoc declLoc,
                              SourceLoc accessorKeywordLoc,
                              AccessorKind accessorKind,
                              AbstractStorageDecl *storage,
                              SourceLoc staticLoc,
                              StaticSpellingKind staticSpelling,
                              bool throws, SourceLoc throwsLoc,
                              GenericParamList *genericParams,
                              ParameterList *parameterList,
                              TypeLoc fnRetType, DeclContext *parent,
                              ClangNode clangNode = ClangNode());

  SourceLoc getAccessorKeywordLoc() const { return AccessorKeywordLoc; }

  AbstractStorageDecl *getStorage() const {
    return Storage;
  }

  AccessorKind getAccessorKind() const {
    return AccessorKind(Bits.AccessorDecl.AccessorKind);
  }

  bool isGetter() const { return getAccessorKind() == AccessorKind::Get; }
  bool isSetter() const { return getAccessorKind() == AccessorKind::Set; }
  bool isAnyAddressor() const {
    auto kind = getAccessorKind();
    return kind == AccessorKind::Address
        || kind == AccessorKind::MutableAddress;
  }

  /// isGetterOrSetter - Determine whether this is specifically a getter or
  /// a setter, as opposed to some other kind of accessor.
  ///
  /// For example, only getters and setters can be exposed to Objective-C.
  bool isGetterOrSetter() const { return isGetter() || isSetter(); }

  bool isObservingAccessor() const {
    switch (getAccessorKind()) {
#define OBSERVING_ACCESSOR(ID, KEYWORD) \
    case AccessorKind::ID: return true;
#define ACCESSOR(ID) \
    case AccessorKind::ID: return false;
#include "swift/AST/AccessorKinds.def"
    }
    llvm_unreachable("bad accessor kind");
  }

  /// \returns true if this is non-mutating due to applying a 'mutating'
  /// attribute. For example a "mutating set" accessor.
  bool isExplicitNonMutating() const;

  /// Is the accesor one of the kinds that's assumed nonmutating by default?
  bool isAssumedNonMutating() const;

  /// Is this accessor one of the kinds that's implicitly a coroutine?
  bool isCoroutine() const {
    switch (getAccessorKind()) {
#define COROUTINE_ACCESSOR(ID, KEYWORD) \
    case AccessorKind::ID: return true;
#define ACCESSOR(ID) \
    case AccessorKind::ID: return false;
#include "swift/AST/AccessorKinds.def"
    }
    llvm_unreachable("bad accessor kind");
  }

  void setIsTransparent(bool transparent) {
    Bits.AccessorDecl.IsTransparent = transparent;
    Bits.AccessorDecl.IsTransparentComputed = 1;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Accessor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
};

inline AccessorDecl *
AbstractStorageDecl::AccessorRecord::getAccessor(AccessorKind kind) const {
  if (auto optIndex = AccessorIndices[unsigned(kind)]) {
    auto accessor = getAllAccessors()[optIndex - 1];
    assert(accessor && accessor->getAccessorKind() == kind);
    return accessor;
  }
  return nullptr;
}
  
/// This represents a 'case' declaration in an 'enum', which may declare
/// one or more individual comma-separated EnumElementDecls.
class EnumCaseDecl final : public Decl,
    private llvm::TrailingObjects<EnumCaseDecl, EnumElementDecl *> {
  friend TrailingObjects;
  SourceLoc CaseLoc;
  
  EnumCaseDecl(SourceLoc CaseLoc,
               ArrayRef<EnumElementDecl *> Elements,
               DeclContext *DC)
    : Decl(DeclKind::EnumCase, DC),
      CaseLoc(CaseLoc)
  {
    Bits.EnumCaseDecl.NumElements = Elements.size();
    std::uninitialized_copy(Elements.begin(), Elements.end(),
                            getTrailingObjects<EnumElementDecl *>());
  }

public:
  static EnumCaseDecl *create(SourceLoc CaseLoc,
                              ArrayRef<EnumElementDecl*> Elements,
                              DeclContext *DC);
  
  /// Get the list of elements declared in this case.
  ArrayRef<EnumElementDecl *> getElements() const {
    return {getTrailingObjects<EnumElementDecl *>(),
            Bits.EnumCaseDecl.NumElements};
  }
  
  SourceLoc getLoc() const {
    return CaseLoc;
  }
  
  SourceRange getSourceRange() const;
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::EnumCase;
  }
};

/// This represents a single case of an 'enum' declaration.
///
/// For example, the X, Y, and Z in this enum:
///
/// \code
///   enum V {
///     case X(Int), Y(Int)
///     case Z
///   }
/// \endcode
///
/// The type of an EnumElementDecl is always the EnumType for the containing
/// enum. EnumElementDecls are represented in the AST as members of their
/// parent EnumDecl, although syntactically they are subordinate to the
/// EnumCaseDecl.
class EnumElementDecl : public DeclContext, public ValueDecl {
  /// This is the type specified with the enum element, for
  /// example 'Int' in 'case Y(Int)'.  This is null if there is no type
  /// associated with this element, as in 'case Z' or in all elements of enum
  /// definitions.
  ParameterList *Params;
  
  SourceLoc EqualsLoc;
  
  /// The raw value literal for the enum element, or null.
  LiteralExpr *RawValueExpr;
  /// The type-checked raw value expression.
  Expr *TypeCheckedRawValueExpr = nullptr;
  
public:
  EnumElementDecl(SourceLoc IdentifierLoc, DeclName Name,
                  ParameterList *Params,
                  SourceLoc EqualsLoc,
                  LiteralExpr *RawValueExpr,
                  DeclContext *DC)
  : DeclContext(DeclContextKind::EnumElementDecl, DC),
    ValueDecl(DeclKind::EnumElement, DC, Name, IdentifierLoc),
    Params(Params),
    EqualsLoc(EqualsLoc),
    RawValueExpr(RawValueExpr)
  {}

  Identifier getName() const { return getFullName().getBaseIdentifier(); }

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    assert(!getFullName().isSpecial() && "Cannot get string for special names");
    return hasName() ? getBaseName().getIdentifier().str() : "_";
  }

  /// Set the interface type of this enum element to the constructor function
  /// type; (Self.Type) -> Self or (Self.Type) -> (Args...) -> Self.
  void computeType();

  Type getArgumentInterfaceType() const;

  ParameterList *getParameterList() const { return Params; }

  bool hasRawValueExpr() const { return RawValueExpr; }
  LiteralExpr *getRawValueExpr() const { return RawValueExpr; }
  void setRawValueExpr(LiteralExpr *e) { RawValueExpr = e; }
  
  Expr *getTypeCheckedRawValueExpr() const {
    return TypeCheckedRawValueExpr;
  }
  void setTypeCheckedRawValueExpr(Expr *e) {
    TypeCheckedRawValueExpr = e;
  }

  /// Return the containing EnumDecl.
  EnumDecl *getParentEnum() const {
    return cast<EnumDecl>(getDeclContext());
  }
  
  /// Return the containing EnumCaseDecl.
  EnumCaseDecl *getParentCase() const;

  SourceLoc getStartLoc() const {
    return getNameLoc();
  }
  SourceRange getSourceRange() const;
  
  bool hasAssociatedValues() const {
    return getParameterList() != nullptr;
  }

  /// True if the case is marked 'indirect'.
  bool isIndirect() const {
    return getAttrs().hasAttribute<IndirectAttr>();
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::EnumElement;
  }

  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  using DeclContext::operator new;
  using Decl::getASTContext;
};
  
inline SourceRange EnumCaseDecl::getSourceRange() const {
  auto subRange = getElements().back()->getSourceRange();
  if (subRange.isValid())
    return {CaseLoc, subRange.End};
  return {};
}

/// Describes the kind of initializer.
enum class CtorInitializerKind {
  /// A designated initializer is an initializer responsible for initializing
  /// the stored properties of the current class and chaining to a superclass's
  /// designated initializer (for non-root classes).
  ///
  /// Designated initializers are never inherited.
  Designated,

  /// A convenience initializer is an initializer that initializes a complete
  /// object by delegating to another initializer (eventually reaching a
  /// designated initializer).
  ///
  /// Convenience initializers are inherited into subclasses that override
  /// all of their superclass's designated initializers.
  Convenience,

  /// A convenience factory initializer is a convenience initializer introduced
  /// by an imported Objective-C factory method.
  ///
  /// Convenience factory initializers cannot be expressed directly in
  /// Swift; rather, they are produced by the Clang importer when importing
  /// an instancetype factory method from Objective-C.
  ConvenienceFactory,

  /// A factory initializer is an initializer that is neither designated nor
  /// convenience: it can be used to create an object of the given type, but
  /// cannot be chained to via "super.init" nor is it inherited.
  ///
  /// A factory initializer is written with a return type of the class name
  /// itself. FIXME: However, this is only a presentation form, and at present
  /// the only factory initializers are produced by importing an Objective-C
  /// factory method that does not return instancetype.
  ///
  /// FIXME: Arguably, structs and enums only have factory initializers, and
  /// using designated initializers for them is a misnomer.
  Factory
};

/// ConstructorDecl - Declares a constructor for a type.  For example:
///
/// \code
/// struct X {
///   var x : Int
///   init(i : Int) {
///      x = i
///   }
/// }
/// \endcode
class ConstructorDecl : public AbstractFunctionDecl {
  /// The location of the '!' or '?' for a failable initializer.
  SourceLoc FailabilityLoc;

  ParamDecl *SelfDecl;

  /// The interface type of the initializing constructor.
  Type InitializerInterfaceType;

  /// The typechecked call to super.init expression, which needs to be
  /// inserted at the end of the initializer by SILGen.
  Expr *CallToSuperInit = nullptr;

public:
  ConstructorDecl(DeclName Name, SourceLoc ConstructorLoc, 
                  OptionalTypeKind Failability, SourceLoc FailabilityLoc,
                  bool Throws, SourceLoc ThrowsLoc,
                  ParameterList *BodyParams,
                  GenericParamList *GenericParams, 
                  DeclContext *Parent);

  SourceLoc getConstructorLoc() const { return getNameLoc(); }
  SourceLoc getStartLoc() const { return getConstructorLoc(); }
  SourceRange getSourceRange() const;

  /// Get the interface type of the constructed object.
  Type getResultInterfaceType() const;

  /// Get the interface type of the initializing constructor.
  Type getInitializerInterfaceType();

  /// Get the typechecked call to super.init expression, which needs to be
  /// inserted at the end of the initializer by SILGen.
  Expr *getSuperInitCall() { return CallToSuperInit; }
  void setSuperInitCall(Expr *CallExpr) { CallToSuperInit = CallExpr; }

  ParamDecl **getImplicitSelfDeclStorage() { return &SelfDecl; }

  /// Specifies the kind of initialization call performed within the body
  /// of the constructor, e.g., self.init or super.init.
  enum class BodyInitKind {
    /// There are no calls to self.init or super.init.
    None,
    /// There is a call to self.init, which delegates to another (peer)
    /// initializer.
    Delegating,
    /// There is a call to super.init, which chains to a superclass initializer.
    Chained,
    /// There are no calls to self.init or super.init explicitly in the body of
    /// the constructor, but a 'super.init' call will be implicitly added
    /// by semantic analysis.
    ImplicitChained
  };

  /// Determine whether the body of this constructor contains any delegating
  /// or superclass initializations (\c self.init or \c super.init,
  /// respectively) within its body.
  ///
  /// \param diags If non-null, this check will ensure that the constructor
  /// body is consistent in its use of delegation vs. chaining and emit any
  /// diagnostics through the given diagnostic engine.
  ///
  /// \param init If non-null and there is an explicit \c self.init or
  /// \c super.init within the body, will be set to point at that
  /// initializer.
  BodyInitKind getDelegatingOrChainedInitKind(DiagnosticEngine *diags,
                                              ApplyExpr **init = nullptr) const;

  /// Whether this constructor is required.
  bool isRequired() const {
    return getAttrs().hasAttribute<RequiredAttr>();
  }

  /// Determine the kind of initializer this is.
  CtorInitializerKind getInitKind() const;

  /// Whether this is a designated initializer.
  bool isDesignatedInit() const {
    return getInitKind() == CtorInitializerKind::Designated;
  }

  /// Whether this is a convenience initializer.
  bool isConvenienceInit() const {
    return getInitKind() == CtorInitializerKind::Convenience ||
           getInitKind() == CtorInitializerKind::ConvenienceFactory;
  }

  /// Whether this is a factory initializer.
  bool isFactoryInit() const {
    switch (getInitKind()) {
    case CtorInitializerKind::Designated:
    case CtorInitializerKind::Convenience:
      return false;
        
    case CtorInitializerKind::Factory:
    case CtorInitializerKind::ConvenienceFactory:
      return true;
    }
    llvm_unreachable("bad CtorInitializerKind");
  }

  /// Determine whether this initializer is inheritable.
  bool isInheritable() const {
    switch (getInitKind()) {
    case CtorInitializerKind::Designated:
    case CtorInitializerKind::Factory:
      return false;

    case CtorInitializerKind::Convenience:
    case CtorInitializerKind::ConvenienceFactory:
      return true;
    }
    llvm_unreachable("bad CtorInitializerKind");
  }

  /// Determine the failability of the initializer.
  OptionalTypeKind getFailability() const {
    return static_cast<OptionalTypeKind>(Bits.ConstructorDecl.Failability);
  }

  /// Retrieve the location of the '!' or '?' in a failable initializer.
  SourceLoc getFailabilityLoc() const { return FailabilityLoc; }

  /// Whether the implementation of this method is a stub that traps at runtime.
  bool hasStubImplementation() const {
    return Bits.ConstructorDecl.HasStubImplementation;
  }

  /// Set whether the implementation of this method is a stub that
  /// traps at runtime.
  void setStubImplementation(bool stub) {
    Bits.ConstructorDecl.HasStubImplementation = stub;
  }

  ConstructorDecl *getOverriddenDecl() const {
    return cast_or_null<ConstructorDecl>(
        AbstractFunctionDecl::getOverriddenDecl());
  }

  /// Determine whether this initializer falls into the special case for
  /// Objective-C initializers with selectors longer than "init", e.g.,
  /// \c initForMemory.
  ///
  /// In such cases, one can write the Swift initializer
  /// with a single parameter of type '()', e.g,
  ///
  /// \code
  /// @objc init(forMemory: ())
  /// \endcode
  bool isObjCZeroParameterWithLongSelector() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Constructor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
};

/// DestructorDecl - Declares a destructor for a type.  For example:
///
/// \code
/// struct X {
///   var fd : Int
///   deinit {
///      close(fd)
///   }
/// }
/// \endcode
class DestructorDecl : public AbstractFunctionDecl {
  ParamDecl *SelfDecl;

public:
  DestructorDecl(SourceLoc DestructorLoc, DeclContext *Parent);

  ParamDecl **getImplicitSelfDeclStorage() { return &SelfDecl; }

  SourceLoc getDestructorLoc() const { return getNameLoc(); }
  SourceLoc getStartLoc() const { return getDestructorLoc(); }
  SourceRange getSourceRange() const;

  /// Retrieve the Objective-C selector for destructors.
  ObjCSelector getObjCSelector() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Destructor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
};

/// Declares a precedence group.  For example:
///
/// \code
/// precedencegroup MultiplicativePrecedence {
///   associativity: right
///   higherThan: AdditivePrecedence
/// }
/// \endcode
class PrecedenceGroupDecl : public Decl {
public:
  struct Relation {
    SourceLoc NameLoc;
    Identifier Name;
    PrecedenceGroupDecl *Group;
  };

private:
  SourceLoc PrecedenceGroupLoc, NameLoc, LBraceLoc, RBraceLoc;
  SourceLoc AssociativityKeywordLoc, AssociativityValueLoc;
  SourceLoc AssignmentKeywordLoc, AssignmentValueLoc;
  SourceLoc HigherThanLoc, LowerThanLoc;
  Identifier Name;
  unsigned NumHigherThan, NumLowerThan;
  // Tail-allocated array of Relations

  Relation *getHigherThanBuffer() {
    return reinterpret_cast<Relation*>(this + 1);
  }
  const Relation *getHigherThanBuffer() const {
    return reinterpret_cast<const Relation*>(this + 1);
  }
  Relation *getLowerThanBuffer() {
    return getHigherThanBuffer() + NumHigherThan;
  }
  const Relation *getLowerThanBuffer() const {
    return getHigherThanBuffer() + NumHigherThan;
  }

  PrecedenceGroupDecl(DeclContext *DC,
                      SourceLoc precedenceGroupLoc,
                      SourceLoc nameLoc, Identifier name,
                      SourceLoc lbraceLoc,
                      SourceLoc associativityKeywordLoc,
                      SourceLoc associativityValueLoc,
                      Associativity associativity,
                      SourceLoc assignmentKeywordLoc,
                      SourceLoc assignmentValueLoc,
                      bool isAssignment,
                      SourceLoc higherThanLoc, ArrayRef<Relation> higherThan,
                      SourceLoc lowerThanLoc, ArrayRef<Relation> lowerThan,
                      SourceLoc rbraceLoc);

public:
  static PrecedenceGroupDecl *create(DeclContext *dc,
                                     SourceLoc precedenceGroupLoc,
                                     SourceLoc nameLoc,
                                     Identifier name,
                                     SourceLoc lbraceLoc,
                                     SourceLoc associativityKeywordLoc,
                                     SourceLoc associativityValueLoc,
                                     Associativity associativity,
                                     SourceLoc assignmentKeywordLoc,
                                     SourceLoc assignmentValueLoc,
                                     bool isAssignment,
                                     SourceLoc higherThanLoc,
                                     ArrayRef<Relation> higherThan,
                                     SourceLoc lowerThanLoc,
                                     ArrayRef<Relation> lowerThan,
                                     SourceLoc rbraceLoc);


  SourceLoc getLoc() const { return NameLoc; }
  SourceRange getSourceRange() const {
    return { PrecedenceGroupLoc, RBraceLoc };
  }

  /// Return the location of 'precedencegroup' in:
  ///   precedencegroup MultiplicativePrecedence { ... }
  SourceLoc getPrecedenceGroupLoc() const { return PrecedenceGroupLoc; }

  /// Return the location of 'MultiplicativePrecedence' in:
  ///   precedencegroup MultiplicativePrecedence { ... }
  SourceLoc getNameLoc() const {
    return NameLoc;
  }

  Identifier getName() const {
    return Name;
  }

  SourceLoc getLBraceLoc() const { return LBraceLoc; }
  SourceLoc getRBraceLoc() const { return RBraceLoc; }

  bool isAssociativityImplicit() const {
    return AssociativityKeywordLoc.isInvalid();
  }

  /// Return the location of 'associativity' in:
  ///   associativity: left
  SourceLoc getAssociativityKeywordLoc() const {
    return AssociativityKeywordLoc;
  }

  /// Return the location of 'right' in:
  ///   associativity: right
  SourceLoc getAssociativityValueLoc() const {
    return AssociativityValueLoc;
  }

  Associativity getAssociativity() const {
    return Associativity(Bits.PrecedenceGroupDecl.Associativity);
  }
  bool isLeftAssociative() const {
    return getAssociativity() == Associativity::Left;
  }
  bool isRightAssociative() const {
    return getAssociativity() == Associativity::Right;
  }
  bool isNonAssociative() const {
    return getAssociativity() == Associativity::None;
  }

  bool isAssignmentImplicit() const {
    return AssignmentKeywordLoc.isInvalid();
  }

  /// Return the location of 'assignment' in:
  ///   assignment: true
  SourceLoc getAssignmentKeywordLoc() const {
    return AssignmentKeywordLoc;
  }

  /// Return the location of 'assignment' in:
  ///   assignment: true
  SourceLoc getAssignmentValueLoc() const {
    return AssignmentValueLoc;
  }

  bool isAssignment() const {
    return Bits.PrecedenceGroupDecl.IsAssignment;
  }

  bool isHigherThanImplicit() const {
    return HigherThanLoc.isInvalid();
  }

  /// Return the location of 'higherThan' in:
  ///   higherThan: AdditivePrecedence
  SourceLoc getHigherThanLoc() const {
    return HigherThanLoc;
  }

  ArrayRef<Relation> getHigherThan() const {
    return { getHigherThanBuffer(), NumHigherThan };
  }
  MutableArrayRef<Relation> getMutableHigherThan() {
    return { getHigherThanBuffer(), NumHigherThan };
  }

  bool isLowerThanImplicit() const {
    return LowerThanLoc.isInvalid();
  }

  /// Return the location of 'lowerThan' in:
  ///   lowerThan: MultiplicativePrecedence
  SourceLoc getLowerThanLoc() const {
    return LowerThanLoc;
  }

  ArrayRef<Relation> getLowerThan() const {
    return { getLowerThanBuffer(), NumLowerThan };
  }
  MutableArrayRef<Relation> getMutableLowerThan() {
    return { getLowerThanBuffer(), NumLowerThan };
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PrecedenceGroup;
  }
};

/// Abstract base class of operator declarations.
class OperatorDecl : public Decl {
  SourceLoc OperatorLoc, NameLoc;
  
  Identifier name;

  ArrayRef<Identifier> Identifiers;
  ArrayRef<SourceLoc> IdentifierLocs;
  ArrayRef<NominalTypeDecl *> DesignatedNominalTypes;

public:
  OperatorDecl(DeclKind kind, DeclContext *DC, SourceLoc OperatorLoc,
               Identifier Name, SourceLoc NameLoc,
               ArrayRef<Identifier> Identifiers,
               ArrayRef<SourceLoc> IdentifierLocs)
      : Decl(kind, DC), OperatorLoc(OperatorLoc), NameLoc(NameLoc), name(Name),
        Identifiers(Identifiers), IdentifierLocs(IdentifierLocs) {}

  OperatorDecl(DeclKind kind, DeclContext *DC, SourceLoc OperatorLoc,
               Identifier Name, SourceLoc NameLoc,
               ArrayRef<NominalTypeDecl *> DesignatedNominalTypes)
      : Decl(kind, DC), OperatorLoc(OperatorLoc), NameLoc(NameLoc), name(Name),
        DesignatedNominalTypes(DesignatedNominalTypes) {}

  SourceLoc getLoc() const { return NameLoc; }

  SourceLoc getOperatorLoc() const { return OperatorLoc; }
  SourceLoc getNameLoc() const { return NameLoc; }
  Identifier getName() const { return name; }

  ArrayRef<Identifier> getIdentifiers() const {
    return Identifiers;
  }

  ArrayRef<SourceLoc> getIdentifierLocs() const {
    return IdentifierLocs;
  }

  ArrayRef<NominalTypeDecl *> getDesignatedNominalTypes() const {
    return DesignatedNominalTypes;
  }

  void setDesignatedNominalTypes(ArrayRef<NominalTypeDecl *> nominalTypes) {
    DesignatedNominalTypes = nominalTypes;
  }

  static bool classof(const Decl *D) {
    // Workaround: http://llvm.org/PR35906
    if (DeclKind::Last_Decl == DeclKind::Last_OperatorDecl)
      return D->getKind() >= DeclKind::First_OperatorDecl;
    return D->getKind() >= DeclKind::First_OperatorDecl
        && D->getKind() <= DeclKind::Last_OperatorDecl;
  }
};

/// Declares the behavior of an infix operator. For example:
///
/// \code
/// infix operator /+/ : AdditionPrecedence, Numeric
/// \endcode
class InfixOperatorDecl : public OperatorDecl {
  SourceLoc ColonLoc;
  PrecedenceGroupDecl *PrecedenceGroup = nullptr;

public:
  InfixOperatorDecl(DeclContext *DC, SourceLoc operatorLoc, Identifier name,
                    SourceLoc nameLoc, SourceLoc colonLoc,
                    ArrayRef<Identifier> identifiers,
                    ArrayRef<SourceLoc> identifierLocs)
      : OperatorDecl(DeclKind::InfixOperator, DC, operatorLoc, name, nameLoc,
                     identifiers, identifierLocs),
        ColonLoc(colonLoc) {}

  InfixOperatorDecl(DeclContext *DC, SourceLoc operatorLoc, Identifier name,
                    SourceLoc nameLoc, SourceLoc colonLoc,
                    PrecedenceGroupDecl *precedenceGroup,
                    ArrayRef<NominalTypeDecl *> designatedNominalTypes)
      : OperatorDecl(DeclKind::InfixOperator, DC, operatorLoc, name, nameLoc,
                     designatedNominalTypes),
        ColonLoc(colonLoc), PrecedenceGroup(precedenceGroup) {}

  SourceLoc getEndLoc() const {
    auto identifierLocs = getIdentifierLocs();
    if (identifierLocs.empty())
      return getNameLoc();

    return identifierLocs.back();
  }

  SourceRange getSourceRange() const {
    return { getOperatorLoc(), getEndLoc() };
  }

  SourceLoc getColonLoc() const { return ColonLoc; }

  PrecedenceGroupDecl *getPrecedenceGroup() const { return PrecedenceGroup; }
  void setPrecedenceGroup(PrecedenceGroupDecl *PGD) {
    PrecedenceGroup = PGD;
  }

  /// True if this decl's attributes conflict with those declared by another
  /// operator.
  bool conflictsWith(InfixOperatorDecl *other) {
    return getPrecedenceGroup() != other->getPrecedenceGroup();
  }
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::InfixOperator;
  }
};
  
/// Declares the behavior of a prefix operator. For example:
///
/// \code
/// prefix operator /+/ {}
/// \endcode
class PrefixOperatorDecl : public OperatorDecl {
public:
  PrefixOperatorDecl(DeclContext *DC, SourceLoc OperatorLoc, Identifier Name,
                     SourceLoc NameLoc,
                     ArrayRef<Identifier> Identifiers,
                     ArrayRef<SourceLoc> IdentifierLocs)
      : OperatorDecl(DeclKind::PrefixOperator, DC, OperatorLoc, Name, NameLoc,
                     Identifiers, IdentifierLocs) {}

  PrefixOperatorDecl(DeclContext *DC, SourceLoc OperatorLoc, Identifier Name,
                     SourceLoc NameLoc,
                     ArrayRef<NominalTypeDecl *> designatedNominalTypes)
      : OperatorDecl(DeclKind::PrefixOperator, DC, OperatorLoc, Name, NameLoc,
                     designatedNominalTypes) {}

  SourceRange getSourceRange() const {
    return { getOperatorLoc(), getNameLoc() };
  }

  /// True if this decl's attributes conflict with those declared by another
  /// PrefixOperatorDecl.
  bool conflictsWith(PrefixOperatorDecl *other) {
    return false;
  }
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PrefixOperator;
  }
};
  
/// Declares the behavior of a postfix operator. For example:
///
/// \code
/// postfix operator /+/ {}
/// \endcode
class PostfixOperatorDecl : public OperatorDecl {
public:
  PostfixOperatorDecl(DeclContext *DC, SourceLoc OperatorLoc, Identifier Name,
                      SourceLoc NameLoc,
                      ArrayRef<Identifier> Identifiers,
                      ArrayRef<SourceLoc> IdentifierLocs)
      : OperatorDecl(DeclKind::PostfixOperator, DC, OperatorLoc, Name, NameLoc,
                     Identifiers, IdentifierLocs) {}

  PostfixOperatorDecl(DeclContext *DC, SourceLoc OperatorLoc, Identifier Name,
                      SourceLoc NameLoc,
                      ArrayRef<NominalTypeDecl *> designatedNominalTypes)
      : OperatorDecl(DeclKind::PostfixOperator, DC, OperatorLoc, Name, NameLoc,
                     designatedNominalTypes) {}

  SourceRange getSourceRange() const {
    return { getOperatorLoc(), getNameLoc() };
  }

  /// True if this decl's attributes conflict with those declared by another
  /// PostfixOperatorDecl.
  bool conflictsWith(PostfixOperatorDecl *other) {
    return false;
  }
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PostfixOperator;
  }
};

/// Represents a hole where a declaration should have been.
///
/// Among other things, these are used to keep vtable layout consistent.
class MissingMemberDecl : public Decl {
  DeclName Name;

  MissingMemberDecl(DeclContext *DC, DeclName name,
                    unsigned vtableEntries,
                    unsigned fieldOffsetVectorEntries)
      : Decl(DeclKind::MissingMember, DC), Name(name) {
    Bits.MissingMemberDecl.NumberOfVTableEntries = vtableEntries;
    assert(getNumberOfVTableEntries() == vtableEntries && "not enough bits");
    Bits.MissingMemberDecl.NumberOfFieldOffsetVectorEntries =
      fieldOffsetVectorEntries;
    assert(getNumberOfFieldOffsetVectorEntries() == fieldOffsetVectorEntries
           && "not enough bits");
    setImplicit();
  }
public:
  static MissingMemberDecl *
  create(ASTContext &ctx, DeclContext *DC, DeclName name,
         unsigned numVTableEntries, bool hasStorage) {
    assert(!numVTableEntries || isa<ProtocolDecl>(DC) || isa<ClassDecl>(DC) &&
           "Only classes and protocols have vtable/witness table entries");
    assert(!hasStorage || !isa<ProtocolDecl>(DC) &&
           "Protocols cannot have missing stored properties");

    return new (ctx) MissingMemberDecl(DC, name, numVTableEntries, hasStorage);
  }

  DeclName getFullName() const {
    return Name;
  }

  unsigned getNumberOfVTableEntries() const {
    return Bits.MissingMemberDecl.NumberOfVTableEntries;
  }

  unsigned getNumberOfFieldOffsetVectorEntries() const {
    return Bits.MissingMemberDecl.NumberOfFieldOffsetVectorEntries;
  }

  SourceLoc getLoc() const {
    return SourceLoc();
  }

  SourceRange getSourceRange() const {
    return SourceRange();
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::MissingMember;
  }
};

inline bool AbstractStorageDecl::isSettable(const DeclContext *UseDC,
                                            const DeclRefExpr *base) const {
  if (auto vd = dyn_cast<VarDecl>(this))
    return vd->isSettable(UseDC, base);

  auto sd = cast<SubscriptDecl>(this);
  return sd->supportsMutation();
}

inline void
AbstractStorageDecl::overwriteSetterAccess(AccessLevel accessLevel) {
  Accessors.setInt(accessLevel);
  if (auto setter = getAccessor(AccessorKind::Set))
    setter->overwriteAccess(accessLevel);
  if (auto modify = getAccessor(AccessorKind::Modify))
    modify->overwriteAccess(accessLevel);
  if (auto mutableAddressor = getAccessor(AccessorKind::MutableAddress))
    mutableAddressor->overwriteAccess(accessLevel);
}

/// Constructors and destructors always have a 'self' parameter,
/// which is stored in an instance member. Functions only have a
/// 'self' if they are declared inside of a nominal type or extension,
/// in which case we tail-allocate storage for it.
inline ParamDecl **AbstractFunctionDecl::getImplicitSelfDeclStorage() {
  switch (getKind()) {
  default: llvm_unreachable("Unknown AbstractFunctionDecl!");
  case DeclKind::Constructor:
    return cast<ConstructorDecl>(this)->getImplicitSelfDeclStorage();
  case DeclKind::Destructor:
    return cast<DestructorDecl>(this)->getImplicitSelfDeclStorage();
  case DeclKind::Func:
  case DeclKind::Accessor:
    return cast<FuncDecl>(this)->getImplicitSelfDeclStorage();
  }
}

inline ParamDecl **FuncDecl::getImplicitSelfDeclStorage() {
  if (!hasImplicitSelfDecl())
    return nullptr;

  if (!isa<AccessorDecl>(this)) {
    assert(getKind() == DeclKind::Func && "no new kinds of functions");
    return reinterpret_cast<ParamDecl **>(this+1);
  }
  return reinterpret_cast<ParamDecl **>(static_cast<AccessorDecl*>(this)+1);
}

inline DeclIterator &DeclIterator::operator++() {
  Current = Current->NextDecl;
  return *this;
}

inline bool AbstractFunctionDecl::hasForcedStaticDispatch() const {
  if (auto func = dyn_cast<FuncDecl>(this))
    return func->hasForcedStaticDispatch();
  return false;
}

inline bool ValueDecl::isStatic() const {
  // Currently, only storage and function decls can be static/class.
  if (auto storage = dyn_cast<AbstractStorageDecl>(this))
    return storage->isStatic();
  if (auto func = dyn_cast<FuncDecl>(this))
    return func->isStatic();
  return false;
}

inline bool ValueDecl::isImportAsMember() const {
  if (auto func = dyn_cast<AbstractFunctionDecl>(this))
    return func->isImportAsMember();
  return false;
}

inline bool ValueDecl::hasCurriedSelf() const {
  if (auto *afd = dyn_cast<AbstractFunctionDecl>(this))
    return afd->hasImplicitSelfDecl();
  if (isa<EnumElementDecl>(this))
    return true;
  return false;
}

inline bool ValueDecl::hasParameterList() const {
  if (auto *eed = dyn_cast<EnumElementDecl>(this))
    return eed->hasAssociatedValues();
  return isa<AbstractFunctionDecl>(this) || isa<SubscriptDecl>(this);
}

inline unsigned ValueDecl::getNumCurryLevels() const {
  unsigned curryLevels = 0;
  if (hasParameterList())
    curryLevels++;
  if (hasCurriedSelf())
    curryLevels++;
  return curryLevels;
}

inline bool Decl::isPotentiallyOverridable() const {
  if (isa<VarDecl>(this) ||
      isa<SubscriptDecl>(this) ||
      isa<FuncDecl>(this) ||
      isa<DestructorDecl>(this)) {
    return getDeclContext()->getSelfClassDecl();
  } else {
    return false;
  }
}

inline GenericParamKey::GenericParamKey(const GenericTypeParamDecl *d)
  : Depth(d->getDepth()), Index(d->getIndex()) { }

inline const GenericContext *Decl::getAsGenericContext() const {
  switch (getKind()) {
  default: return nullptr;
#define DECL(Id, Parent) // See previous line
#define GENERIC_DECL(Id, Parent) \
  case DeclKind::Id: \
    return static_cast<const Id##Decl*>(this);
#include "swift/AST/DeclNodes.def"
  }
}

inline bool DeclContext::classof(const Decl *D) {
  switch (D->getKind()) { //
  default: return false;
#define DECL(ID, PARENT) // See previous line
#define CONTEXT_DECL(ID, PARENT) \
  case DeclKind::ID: return true;
#include "swift/AST/DeclNodes.def"
  }
}

inline DeclContext *DeclContext::castDeclToDeclContext(const Decl *D) {
  // XXX -- ModuleDecl is not defined in Decl.h, but because DeclContexts
  // preface decls in memory, any DeclContext type will due.
  const DeclContext *DC = static_cast<const ExtensionDecl*>(D);
  switch (D->getKind()) {
  default: llvm_unreachable("Not a DeclContext");
#define DECL(ID, PARENT) // See previous line
#define CONTEXT_DECL(ID, PARENT) \
  case DeclKind::ID:
#include "swift/AST/DeclNodes.def"
    return const_cast<DeclContext *>(DC);
  }
}

inline EnumElementDecl *EnumDecl::getUniqueElement(bool hasValue) const {
  EnumElementDecl *result = nullptr;
  bool found = false;
  for (auto elt : getAllElements()) {
    if (elt->hasAssociatedValues() == hasValue) {
      if (found)
        return nullptr;
      found = true;
      result = elt;
    }
  }
  return result;
}

/// Retrieve parameter declaration from the given source at given index.
const ParamDecl *getParameterAt(const ValueDecl *source, unsigned index);

/// Display Decl subclasses.
void simple_display(llvm::raw_ostream &out, const Decl *decl);

/// Display ValueDecl subclasses.
void simple_display(llvm::raw_ostream &out, const ValueDecl *decl);

/// Extract the source location from the given declaration.
SourceLoc extractNearestSourceLoc(const Decl *decl);

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const ExtensionDecl *ext) {
  return extractNearestSourceLoc(static_cast<const Decl *>(ext));
}

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const GenericTypeDecl *type) {
  return extractNearestSourceLoc(static_cast<const Decl *>(type));
}

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const AbstractFunctionDecl *func) {
  return extractNearestSourceLoc(static_cast<const Decl *>(func));
}

} // end namespace swift

#endif
