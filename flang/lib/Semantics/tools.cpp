//===-- lib/Semantics/tools.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Parser/tools.h"
#include "flang/Common/indirection.h"
#include "flang/Parser/dump-parse-tree.h"
#include "flang/Parser/message.h"
#include "flang/Parser/parse-tree.h"
#include "flang/Semantics/scope.h"
#include "flang/Semantics/semantics.h"
#include "flang/Semantics/symbol.h"
#include "flang/Semantics/tools.h"
#include "flang/Semantics/type.h"
#include "flang/Support/Fortran.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <set>
#include <variant>

namespace Fortran::semantics {

// Find this or containing scope that matches predicate
static const Scope *FindScopeContaining(
    const Scope &start, std::function<bool(const Scope &)> predicate) {
  for (const Scope *scope{&start};; scope = &scope->parent()) {
    if (predicate(*scope)) {
      return scope;
    }
    if (scope->IsTopLevel()) {
      return nullptr;
    }
  }
}

const Scope &GetTopLevelUnitContaining(const Scope &start) {
  CHECK(!start.IsTopLevel());
  return DEREF(FindScopeContaining(
      start, [](const Scope &scope) { return scope.parent().IsTopLevel(); }));
}

const Scope &GetTopLevelUnitContaining(const Symbol &symbol) {
  return GetTopLevelUnitContaining(symbol.owner());
}

const Scope *FindModuleContaining(const Scope &start) {
  return FindScopeContaining(
      start, [](const Scope &scope) { return scope.IsModule(); });
}

const Scope *FindModuleOrSubmoduleContaining(const Scope &start) {
  return FindScopeContaining(start, [](const Scope &scope) {
    return scope.IsModule() || scope.IsSubmodule();
  });
}

const Scope *FindModuleFileContaining(const Scope &start) {
  return FindScopeContaining(
      start, [](const Scope &scope) { return scope.IsModuleFile(); });
}

const Scope &GetProgramUnitContaining(const Scope &start) {
  CHECK(!start.IsTopLevel());
  return DEREF(FindScopeContaining(start, [](const Scope &scope) {
    switch (scope.kind()) {
    case Scope::Kind::Module:
    case Scope::Kind::MainProgram:
    case Scope::Kind::Subprogram:
    case Scope::Kind::BlockData:
      return true;
    default:
      return false;
    }
  }));
}

const Scope &GetProgramUnitContaining(const Symbol &symbol) {
  return GetProgramUnitContaining(symbol.owner());
}

const Scope &GetProgramUnitOrBlockConstructContaining(const Scope &start) {
  CHECK(!start.IsTopLevel());
  return DEREF(FindScopeContaining(start, [](const Scope &scope) {
    switch (scope.kind()) {
    case Scope::Kind::Module:
    case Scope::Kind::MainProgram:
    case Scope::Kind::Subprogram:
    case Scope::Kind::BlockData:
    case Scope::Kind::BlockConstruct:
      return true;
    default:
      return false;
    }
  }));
}

const Scope &GetProgramUnitOrBlockConstructContaining(const Symbol &symbol) {
  return GetProgramUnitOrBlockConstructContaining(symbol.owner());
}

const Scope *FindPureProcedureContaining(const Scope &start) {
  // N.B. We only need to examine the innermost containing program unit
  // because an internal subprogram of a pure subprogram must also
  // be pure (C1592).
  if (start.IsTopLevel()) {
    return nullptr;
  } else {
    const Scope &scope{GetProgramUnitContaining(start)};
    return IsPureProcedure(scope) ? &scope : nullptr;
  }
}

const Scope *FindOpenACCConstructContaining(const Scope *scope) {
  return scope ? FindScopeContaining(*scope,
                     [](const Scope &s) {
                       return s.kind() == Scope::Kind::OpenACCConstruct;
                     })
               : nullptr;
}

// 7.5.2.4 "same derived type" test -- rely on IsTkCompatibleWith() and its
// infrastructure to detect and handle comparisons on distinct (but "same")
// sequence/bind(C) derived types
static bool MightBeSameDerivedType(
    const std::optional<evaluate::DynamicType> &lhsType,
    const std::optional<evaluate::DynamicType> &rhsType) {
  return lhsType && rhsType && lhsType->IsTkCompatibleWith(*rhsType);
}

Tristate IsDefinedAssignment(
    const std::optional<evaluate::DynamicType> &lhsType, int lhsRank,
    const std::optional<evaluate::DynamicType> &rhsType, int rhsRank) {
  if (!lhsType || !rhsType) {
    return Tristate::No; // error or rhs is untyped
  }
  TypeCategory lhsCat{lhsType->category()};
  TypeCategory rhsCat{rhsType->category()};
  if (rhsRank > 0 && lhsRank != rhsRank) {
    return Tristate::Yes;
  } else if (lhsCat != TypeCategory::Derived) {
    return ToTristate(lhsCat != rhsCat &&
        (!IsNumericTypeCategory(lhsCat) || !IsNumericTypeCategory(rhsCat) ||
            lhsCat == TypeCategory::Unsigned ||
            rhsCat == TypeCategory::Unsigned));
  } else if (MightBeSameDerivedType(lhsType, rhsType)) {
    return Tristate::Maybe; // TYPE(t) = TYPE(t) can be defined or intrinsic
  } else {
    return Tristate::Yes;
  }
}

bool IsIntrinsicRelational(common::RelationalOperator opr,
    const evaluate::DynamicType &type0, int rank0,
    const evaluate::DynamicType &type1, int rank1) {
  if (!evaluate::AreConformable(rank0, rank1)) {
    return false;
  } else {
    auto cat0{type0.category()};
    auto cat1{type1.category()};
    if (cat0 == TypeCategory::Unsigned || cat1 == TypeCategory::Unsigned) {
      return cat0 == cat1;
    } else if (IsNumericTypeCategory(cat0) && IsNumericTypeCategory(cat1)) {
      // numeric types: EQ/NE always ok, others ok for non-complex
      return opr == common::RelationalOperator::EQ ||
          opr == common::RelationalOperator::NE ||
          (cat0 != TypeCategory::Complex && cat1 != TypeCategory::Complex);
    } else {
      // not both numeric: only Character is ok
      return cat0 == TypeCategory::Character && cat1 == TypeCategory::Character;
    }
  }
}

bool IsIntrinsicNumeric(const evaluate::DynamicType &type0) {
  return IsNumericTypeCategory(type0.category());
}
bool IsIntrinsicNumeric(const evaluate::DynamicType &type0, int rank0,
    const evaluate::DynamicType &type1, int rank1) {
  return evaluate::AreConformable(rank0, rank1) &&
      IsNumericTypeCategory(type0.category()) &&
      IsNumericTypeCategory(type1.category());
}

bool IsIntrinsicLogical(const evaluate::DynamicType &type0) {
  return type0.category() == TypeCategory::Logical;
}
bool IsIntrinsicLogical(const evaluate::DynamicType &type0, int rank0,
    const evaluate::DynamicType &type1, int rank1) {
  return evaluate::AreConformable(rank0, rank1) &&
      type0.category() == TypeCategory::Logical &&
      type1.category() == TypeCategory::Logical;
}

bool IsIntrinsicConcat(const evaluate::DynamicType &type0, int rank0,
    const evaluate::DynamicType &type1, int rank1) {
  return evaluate::AreConformable(rank0, rank1) &&
      type0.category() == TypeCategory::Character &&
      type1.category() == TypeCategory::Character &&
      type0.kind() == type1.kind();
}

bool IsGenericDefinedOp(const Symbol &symbol) {
  const Symbol &ultimate{symbol.GetUltimate()};
  if (const auto *generic{ultimate.detailsIf<GenericDetails>()}) {
    return generic->kind().IsDefinedOperator();
  } else if (const auto *misc{ultimate.detailsIf<MiscDetails>()}) {
    return misc->kind() == MiscDetails::Kind::TypeBoundDefinedOp;
  } else {
    return false;
  }
}

bool IsDefinedOperator(SourceName name) {
  const char *begin{name.begin()};
  const char *end{name.end()};
  return begin != end && begin[0] == '.' && end[-1] == '.';
}

std::string MakeOpName(SourceName name) {
  std::string result{name.ToString()};
  return IsDefinedOperator(name)         ? "OPERATOR(" + result + ")"
      : result.find("operator(", 0) == 0 ? parser::ToUpperCaseLetters(result)
                                         : result;
}

bool IsCommonBlockContaining(const Symbol &block, const Symbol &object) {
  const auto &objects{block.get<CommonBlockDetails>().objects()};
  return llvm::is_contained(objects, object);
}

bool IsUseAssociated(const Symbol &symbol, const Scope &scope) {
  const Scope &owner{GetTopLevelUnitContaining(symbol.GetUltimate().owner())};
  return owner.kind() == Scope::Kind::Module &&
      owner != GetTopLevelUnitContaining(scope);
}

bool DoesScopeContain(
    const Scope *maybeAncestor, const Scope &maybeDescendent) {
  return maybeAncestor && !maybeDescendent.IsTopLevel() &&
      FindScopeContaining(maybeDescendent.parent(),
          [&](const Scope &scope) { return &scope == maybeAncestor; });
}

bool DoesScopeContain(const Scope *maybeAncestor, const Symbol &symbol) {
  return DoesScopeContain(maybeAncestor, symbol.owner());
}

static const Symbol &FollowHostAssoc(const Symbol &symbol) {
  for (const Symbol *s{&symbol};;) {
    const auto *details{s->detailsIf<HostAssocDetails>()};
    if (!details) {
      return *s;
    }
    s = &details->symbol();
  }
}

bool IsHostAssociated(const Symbol &symbol, const Scope &scope) {
  const Symbol &base{FollowHostAssoc(symbol)};
  return base.owner().IsTopLevel() ||
      DoesScopeContain(&GetProgramUnitOrBlockConstructContaining(base),
          GetProgramUnitOrBlockConstructContaining(scope));
}

bool IsHostAssociatedIntoSubprogram(const Symbol &symbol, const Scope &scope) {
  const Symbol &base{FollowHostAssoc(symbol)};
  return base.owner().IsTopLevel() ||
      DoesScopeContain(&GetProgramUnitOrBlockConstructContaining(base),
          GetProgramUnitContaining(scope));
}

bool IsInStmtFunction(const Symbol &symbol) {
  if (const Symbol * function{symbol.owner().symbol()}) {
    return IsStmtFunction(*function);
  }
  return false;
}

bool IsStmtFunctionDummy(const Symbol &symbol) {
  return IsDummy(symbol) && IsInStmtFunction(symbol);
}

bool IsStmtFunctionResult(const Symbol &symbol) {
  return IsFunctionResult(symbol) && IsInStmtFunction(symbol);
}

bool IsPointerDummy(const Symbol &symbol) {
  return IsPointer(symbol) && IsDummy(symbol);
}

bool IsBindCProcedure(const Symbol &original) {
  const Symbol &symbol{original.GetUltimate()};
  if (const auto *procDetails{symbol.detailsIf<ProcEntityDetails>()}) {
    if (procDetails->procInterface()) {
      // procedure component with a BIND(C) interface
      return IsBindCProcedure(*procDetails->procInterface());
    }
  }
  return symbol.attrs().test(Attr::BIND_C) && IsProcedure(symbol);
}

bool IsBindCProcedure(const Scope &scope) {
  if (const Symbol * symbol{scope.GetSymbol()}) {
    return IsBindCProcedure(*symbol);
  } else {
    return false;
  }
}

// C1594 specifies several ways by which an object might be globally visible.
const Symbol *FindExternallyVisibleObject(
    const Symbol &object, const Scope &scope, bool isPointerDefinition) {
  // TODO: Storage association with any object for which this predicate holds,
  // once EQUIVALENCE is supported.
  const Symbol &ultimate{GetAssociationRoot(object)};
  if (IsDummy(ultimate)) {
    if (IsIntentIn(ultimate)) {
      return &ultimate;
    }
    if (!isPointerDefinition && IsPointer(ultimate) &&
        IsPureProcedure(ultimate.owner()) && IsFunction(ultimate.owner())) {
      return &ultimate;
    }
  } else if (ultimate.owner().IsDerivedType()) {
    return nullptr;
  } else if (&GetProgramUnitContaining(ultimate) !=
      &GetProgramUnitContaining(scope)) {
    return &object;
  } else if (const Symbol * block{FindCommonBlockContaining(ultimate)}) {
    return block;
  }
  return nullptr;
}

const Symbol &BypassGeneric(const Symbol &symbol) {
  const Symbol &ultimate{symbol.GetUltimate()};
  if (const auto *generic{ultimate.detailsIf<GenericDetails>()}) {
    if (const Symbol * specific{generic->specific()}) {
      return *specific;
    }
  }
  return symbol;
}

const Symbol &GetCrayPointer(const Symbol &crayPointee) {
  const Symbol *found{nullptr};
  for (const auto &[pointee, pointer] :
      crayPointee.GetUltimate().owner().crayPointers()) {
    if (pointee == crayPointee.name()) {
      found = &pointer.get();
      break;
    }
  }
  return DEREF(found);
}

bool ExprHasTypeCategory(
    const SomeExpr &expr, const common::TypeCategory &type) {
  auto dynamicType{expr.GetType()};
  return dynamicType && dynamicType->category() == type;
}

bool ExprTypeKindIsDefault(
    const SomeExpr &expr, const SemanticsContext &context) {
  auto dynamicType{expr.GetType()};
  return dynamicType &&
      dynamicType->category() != common::TypeCategory::Derived &&
      dynamicType->kind() == context.GetDefaultKind(dynamicType->category());
}

// If an analyzed expr or assignment is missing, dump the node and die.
template <typename T>
static void CheckMissingAnalysis(
    bool crash, SemanticsContext *context, const T &x) {
  if (crash && !(context && context->AnyFatalError())) {
    std::string buf;
    llvm::raw_string_ostream ss{buf};
    ss << "node has not been analyzed:\n";
    parser::DumpTree(ss, x);
    common::die(buf.c_str());
  }
}

const SomeExpr *GetExprHelper::Get(const parser::Expr &x) {
  CheckMissingAnalysis(crashIfNoExpr_ && !x.typedExpr, context_, x);
  return x.typedExpr ? common::GetPtrFromOptional(x.typedExpr->v) : nullptr;
}
const SomeExpr *GetExprHelper::Get(const parser::Variable &x) {
  CheckMissingAnalysis(crashIfNoExpr_ && !x.typedExpr, context_, x);
  return x.typedExpr ? common::GetPtrFromOptional(x.typedExpr->v) : nullptr;
}
const SomeExpr *GetExprHelper::Get(const parser::DataStmtConstant &x) {
  CheckMissingAnalysis(crashIfNoExpr_ && !x.typedExpr, context_, x);
  return x.typedExpr ? common::GetPtrFromOptional(x.typedExpr->v) : nullptr;
}
const SomeExpr *GetExprHelper::Get(const parser::AllocateObject &x) {
  CheckMissingAnalysis(crashIfNoExpr_ && !x.typedExpr, context_, x);
  return x.typedExpr ? common::GetPtrFromOptional(x.typedExpr->v) : nullptr;
}
const SomeExpr *GetExprHelper::Get(const parser::PointerObject &x) {
  CheckMissingAnalysis(crashIfNoExpr_ && !x.typedExpr, context_, x);
  return x.typedExpr ? common::GetPtrFromOptional(x.typedExpr->v) : nullptr;
}

const evaluate::Assignment *GetAssignment(const parser::AssignmentStmt &x) {
  return x.typedAssignment ? common::GetPtrFromOptional(x.typedAssignment->v)
                           : nullptr;
}
const evaluate::Assignment *GetAssignment(
    const parser::PointerAssignmentStmt &x) {
  return x.typedAssignment ? common::GetPtrFromOptional(x.typedAssignment->v)
                           : nullptr;
}

const Symbol *FindInterface(const Symbol &symbol) {
  return common::visit(
      common::visitors{
          [](const ProcEntityDetails &details) {
            const Symbol *interface{details.procInterface()};
            return interface ? FindInterface(*interface) : nullptr;
          },
          [](const ProcBindingDetails &details) {
            return FindInterface(details.symbol());
          },
          [&](const SubprogramDetails &) { return &symbol; },
          [](const UseDetails &details) {
            return FindInterface(details.symbol());
          },
          [](const HostAssocDetails &details) {
            return FindInterface(details.symbol());
          },
          [](const GenericDetails &details) {
            return details.specific() ? FindInterface(*details.specific())
                                      : nullptr;
          },
          [](const auto &) -> const Symbol * { return nullptr; },
      },
      symbol.details());
}

const Symbol *FindSubprogram(const Symbol &symbol) {
  return common::visit(
      common::visitors{
          [&](const ProcEntityDetails &details) -> const Symbol * {
            if (details.procInterface()) {
              return FindSubprogram(*details.procInterface());
            } else {
              return &symbol;
            }
          },
          [](const ProcBindingDetails &details) {
            return FindSubprogram(details.symbol());
          },
          [&](const SubprogramDetails &) { return &symbol; },
          [](const UseDetails &details) {
            return FindSubprogram(details.symbol());
          },
          [](const HostAssocDetails &details) {
            return FindSubprogram(details.symbol());
          },
          [](const GenericDetails &details) {
            return details.specific() ? FindSubprogram(*details.specific())
                                      : nullptr;
          },
          [](const auto &) -> const Symbol * { return nullptr; },
      },
      symbol.details());
}

const Symbol *FindOverriddenBinding(
    const Symbol &symbol, bool &isInaccessibleDeferred) {
  isInaccessibleDeferred = false;
  if (symbol.has<ProcBindingDetails>()) {
    if (const DeclTypeSpec * parentType{FindParentTypeSpec(symbol.owner())}) {
      if (const DerivedTypeSpec * parentDerived{parentType->AsDerived()}) {
        if (const Scope * parentScope{parentDerived->typeSymbol().scope()}) {
          if (const Symbol *
              overridden{parentScope->FindComponent(symbol.name())}) {
            // 7.5.7.3 p1: only accessible bindings are overridden
            if (IsAccessible(*overridden, symbol.owner())) {
              return overridden;
            } else if (overridden->attrs().test(Attr::DEFERRED)) {
              isInaccessibleDeferred = true;
              return overridden;
            }
          }
        }
      }
    }
  }
  return nullptr;
}

const Symbol *FindGlobal(const Symbol &original) {
  const Symbol &ultimate{original.GetUltimate()};
  if (ultimate.owner().IsGlobal()) {
    return &ultimate;
  }
  bool isLocal{false};
  if (IsDummy(ultimate)) {
  } else if (IsPointer(ultimate)) {
  } else if (ultimate.has<ProcEntityDetails>()) {
    isLocal = IsExternal(ultimate);
  } else if (const auto *subp{ultimate.detailsIf<SubprogramDetails>()}) {
    isLocal = subp->isInterface();
  }
  if (isLocal) {
    const std::string *bind{ultimate.GetBindName()};
    if (!bind || ultimate.name() == *bind) {
      const Scope &globalScope{ultimate.owner().context().globalScope()};
      if (auto iter{globalScope.find(ultimate.name())};
          iter != globalScope.end()) {
        const Symbol &global{*iter->second};
        const std::string *globalBind{global.GetBindName()};
        if (!globalBind || global.name() == *globalBind) {
          return &global;
        }
      }
    }
  }
  return nullptr;
}

const DeclTypeSpec *FindParentTypeSpec(const DerivedTypeSpec &derived) {
  return FindParentTypeSpec(derived.typeSymbol());
}

const DeclTypeSpec *FindParentTypeSpec(const DeclTypeSpec &decl) {
  if (const DerivedTypeSpec * derived{decl.AsDerived()}) {
    return FindParentTypeSpec(*derived);
  } else {
    return nullptr;
  }
}

const DeclTypeSpec *FindParentTypeSpec(const Scope &scope) {
  if (scope.kind() == Scope::Kind::DerivedType) {
    if (const auto *symbol{scope.symbol()}) {
      return FindParentTypeSpec(*symbol);
    }
  }
  return nullptr;
}

const DeclTypeSpec *FindParentTypeSpec(const Symbol &symbol) {
  if (const Scope * scope{symbol.scope()}) {
    if (const auto *details{symbol.detailsIf<DerivedTypeDetails>()}) {
      if (const Symbol * parent{details->GetParentComponent(*scope)}) {
        return parent->GetType();
      }
    }
  }
  return nullptr;
}

const EquivalenceSet *FindEquivalenceSet(const Symbol &symbol) {
  const Symbol &ultimate{symbol.GetUltimate()};
  for (const EquivalenceSet &set : ultimate.owner().equivalenceSets()) {
    for (const EquivalenceObject &object : set) {
      if (object.symbol == ultimate) {
        return &set;
      }
    }
  }
  return nullptr;
}

bool IsOrContainsEventOrLockComponent(const Symbol &original) {
  const Symbol &symbol{ResolveAssociations(original, /*stopAtTypeGuard=*/true)};
  if (evaluate::IsVariable(symbol)) {
    if (const DeclTypeSpec * type{symbol.GetType()}) {
      if (const DerivedTypeSpec * derived{type->AsDerived()}) {
        return IsEventTypeOrLockType(derived) ||
            FindEventOrLockPotentialComponent(*derived);
      }
    }
  }
  return false;
}

// Check this symbol suitable as a type-bound procedure - C769
bool CanBeTypeBoundProc(const Symbol &symbol) {
  if (IsDummy(symbol) || IsProcedurePointer(symbol)) {
    return false;
  } else if (symbol.has<SubprogramNameDetails>()) {
    return symbol.owner().kind() == Scope::Kind::Module;
  } else if (auto *details{symbol.detailsIf<SubprogramDetails>()}) {
    if (details->isInterface()) {
      return !symbol.attrs().test(Attr::ABSTRACT);
    } else {
      return symbol.owner().kind() == Scope::Kind::Module;
    }
  } else if (const auto *proc{symbol.detailsIf<ProcEntityDetails>()}) {
    return !symbol.attrs().test(Attr::INTRINSIC) &&
        proc->HasExplicitInterface();
  } else {
    return false;
  }
}

bool HasDeclarationInitializer(const Symbol &symbol) {
  if (IsNamedConstant(symbol)) {
    return false;
  } else if (const auto *object{symbol.detailsIf<ObjectEntityDetails>()}) {
    return object->init().has_value();
  } else if (const auto *proc{symbol.detailsIf<ProcEntityDetails>()}) {
    return proc->init().has_value();
  } else {
    return false;
  }
}

bool IsInitialized(const Symbol &symbol, bool ignoreDataStatements,
    bool ignoreAllocatable, bool ignorePointer) {
  if (!ignoreAllocatable && IsAllocatable(symbol)) {
    return true;
  } else if (!ignoreDataStatements && symbol.test(Symbol::Flag::InDataStmt)) {
    return true;
  } else if (HasDeclarationInitializer(symbol)) {
    return true;
  } else if (IsPointer(symbol)) {
    return !ignorePointer;
  } else if (IsNamedConstant(symbol)) {
    return false;
  } else if (const auto *object{symbol.detailsIf<ObjectEntityDetails>()}) {
    if ((!object->isDummy() || IsIntentOut(symbol)) && object->type()) {
      if (const auto *derived{object->type()->AsDerived()}) {
        return derived->HasDefaultInitialization(
            ignoreAllocatable, ignorePointer);
      }
    }
  }
  return false;
}

bool IsDestructible(const Symbol &symbol, const Symbol *derivedTypeSymbol) {
  if (IsAllocatable(symbol) || IsAutomatic(symbol)) {
    return true;
  } else if (IsNamedConstant(symbol) || IsFunctionResult(symbol) ||
      IsPointer(symbol)) {
    return false;
  } else if (const auto *object{symbol.detailsIf<ObjectEntityDetails>()}) {
    if ((!object->isDummy() || IsIntentOut(symbol)) && object->type()) {
      if (const auto *derived{object->type()->AsDerived()}) {
        return &derived->typeSymbol() != derivedTypeSymbol &&
            derived->HasDestruction();
      }
    }
  }
  return false;
}

bool HasIntrinsicTypeName(const Symbol &symbol) {
  std::string name{symbol.name().ToString()};
  if (name == "doubleprecision") {
    return true;
  } else if (name == "derived") {
    return false;
  } else {
    for (int i{0}; i != common::TypeCategory_enumSize; ++i) {
      if (name == parser::ToLowerCaseLetters(EnumToString(TypeCategory{i}))) {
        return true;
      }
    }
    return false;
  }
}

bool IsSeparateModuleProcedureInterface(const Symbol *symbol) {
  if (symbol && symbol->attrs().test(Attr::MODULE)) {
    if (auto *details{symbol->detailsIf<SubprogramDetails>()}) {
      return details->isInterface();
    }
  }
  return false;
}

SymbolVector FinalsForDerivedTypeInstantiation(const DerivedTypeSpec &spec) {
  SymbolVector result;
  const Symbol &typeSymbol{spec.typeSymbol()};
  if (const auto *derived{typeSymbol.detailsIf<DerivedTypeDetails>()}) {
    for (const auto &pair : derived->finals()) {
      const Symbol &subr{*pair.second};
      // Errors in FINAL subroutines are caught in CheckFinal
      // in check-declarations.cpp.
      if (const auto *subprog{subr.detailsIf<SubprogramDetails>()};
          subprog && subprog->dummyArgs().size() == 1) {
        if (const Symbol * arg{subprog->dummyArgs()[0]}) {
          if (const DeclTypeSpec * type{arg->GetType()}) {
            if (type->category() == DeclTypeSpec::TypeDerived &&
                evaluate::AreSameDerivedType(spec, type->derivedTypeSpec())) {
              result.emplace_back(subr);
            }
          }
        }
      }
    }
  }
  return result;
}

const Symbol *IsFinalizable(const Symbol &symbol,
    std::set<const DerivedTypeSpec *> *inProgress, bool withImpureFinalizer) {
  if (IsPointer(symbol) || evaluate::IsAssumedRank(symbol)) {
    return nullptr;
  }
  if (const auto *object{symbol.detailsIf<ObjectEntityDetails>()}) {
    if (object->isDummy() && !IsIntentOut(symbol)) {
      return nullptr;
    }
    const DeclTypeSpec *type{object->type()};
    if (const DerivedTypeSpec * typeSpec{type ? type->AsDerived() : nullptr}) {
      return IsFinalizable(
          *typeSpec, inProgress, withImpureFinalizer, symbol.Rank());
    }
  }
  return nullptr;
}

const Symbol *IsFinalizable(const DerivedTypeSpec &derived,
    std::set<const DerivedTypeSpec *> *inProgress, bool withImpureFinalizer,
    std::optional<int> rank) {
  const Symbol *elemental{nullptr};
  for (auto ref : FinalsForDerivedTypeInstantiation(derived)) {
    const Symbol *symbol{&ref->GetUltimate()};
    if (const auto *binding{symbol->detailsIf<ProcBindingDetails>()}) {
      symbol = &binding->symbol();
    }
    if (const auto *proc{symbol->detailsIf<ProcEntityDetails>()}) {
      symbol = proc->procInterface();
    }
    if (!symbol) {
    } else if (IsElementalProcedure(*symbol)) {
      elemental = symbol;
    } else {
      if (rank) {
        if (const SubprogramDetails *
            subp{symbol->detailsIf<SubprogramDetails>()}) {
          if (const auto &args{subp->dummyArgs()}; !args.empty() &&
              args.at(0) && !evaluate::IsAssumedRank(*args.at(0)) &&
              args.at(0)->Rank() != *rank) {
            continue; // not a finalizer for this rank
          }
        }
      }
      if (!withImpureFinalizer || !IsPureProcedure(*symbol)) {
        return symbol;
      }
      // Found non-elemental pure finalizer of matching rank, but still
      // need to check components for an impure finalizer.
      elemental = nullptr;
      break;
    }
  }
  if (elemental && (!withImpureFinalizer || !IsPureProcedure(*elemental))) {
    return elemental;
  }
  // Check components (including ancestors)
  std::set<const DerivedTypeSpec *> basis;
  if (inProgress) {
    if (inProgress->find(&derived) != inProgress->end()) {
      return nullptr; // don't loop on recursive type
    }
  } else {
    inProgress = &basis;
  }
  auto iterator{inProgress->insert(&derived).first};
  const Symbol *result{nullptr};
  for (const Symbol &component : PotentialComponentIterator{derived}) {
    result = IsFinalizable(component, inProgress, withImpureFinalizer);
    if (result) {
      break;
    }
  }
  inProgress->erase(iterator);
  return result;
}

static const Symbol *HasImpureFinal(
    const DerivedTypeSpec &derived, std::optional<int> rank) {
  return IsFinalizable(derived, nullptr, /*withImpureFinalizer=*/true, rank);
}

const Symbol *HasImpureFinal(const Symbol &original, std::optional<int> rank) {
  const Symbol &symbol{ResolveAssociations(original, /*stopAtTypeGuard=*/true)};
  if (symbol.has<ObjectEntityDetails>()) {
    if (const DeclTypeSpec * symType{symbol.GetType()}) {
      if (const DerivedTypeSpec * derived{symType->AsDerived()}) {
        if (evaluate::IsAssumedRank(symbol)) {
          // finalizable assumed-rank not allowed (C839)
          return nullptr;
        } else {
          int actualRank{rank.value_or(symbol.Rank())};
          return HasImpureFinal(*derived, actualRank);
        }
      }
    }
  }
  return nullptr;
}

bool MayRequireFinalization(const DerivedTypeSpec &derived) {
  return IsFinalizable(derived) ||
      FindPolymorphicAllocatablePotentialComponent(derived);
}

bool HasAllocatableDirectComponent(const DerivedTypeSpec &derived) {
  DirectComponentIterator directs{derived};
  return std::any_of(directs.begin(), directs.end(), IsAllocatable);
}

static bool MayHaveDefinedAssignment(
    const DerivedTypeSpec &derived, std::set<const Scope *> &checked) {
  if (const Scope *scope{derived.GetScope()};
      scope && checked.find(scope) == checked.end()) {
    checked.insert(scope);
    for (const auto &[_, symbolRef] : *scope) {
      if (const auto *generic{symbolRef->detailsIf<GenericDetails>()}) {
        if (generic->kind().IsAssignment()) {
          return true;
        }
      } else if (symbolRef->has<ObjectEntityDetails>() &&
          !IsPointer(*symbolRef)) {
        if (const DeclTypeSpec *type{symbolRef->GetType()}) {
          if (type->IsPolymorphic()) {
            return true;
          } else if (const DerivedTypeSpec *derived{type->AsDerived()}) {
            if (MayHaveDefinedAssignment(*derived, checked)) {
              return true;
            }
          }
        }
      }
    }
  }
  return false;
}

bool MayHaveDefinedAssignment(const DerivedTypeSpec &derived) {
  std::set<const Scope *> checked;
  return MayHaveDefinedAssignment(derived, checked);
}

bool IsAssumedLengthCharacter(const Symbol &symbol) {
  if (const DeclTypeSpec * type{symbol.GetType()}) {
    return type->category() == DeclTypeSpec::Character &&
        type->characterTypeSpec().length().isAssumed();
  } else {
    return false;
  }
}

bool IsInBlankCommon(const Symbol &symbol) {
  const Symbol *block{FindCommonBlockContaining(symbol)};
  return block && block->name().empty();
}

// C722 and C723:  For a function to be assumed length, it must be external and
// of CHARACTER type
bool IsExternal(const Symbol &symbol) {
  return ClassifyProcedure(symbol) == ProcedureDefinitionClass::External;
}

// Most scopes have no EQUIVALENCE, and this function is a fast no-op for them.
std::list<std::list<SymbolRef>> GetStorageAssociations(const Scope &scope) {
  UnorderedSymbolSet distinct;
  for (const EquivalenceSet &set : scope.equivalenceSets()) {
    for (const EquivalenceObject &object : set) {
      distinct.emplace(object.symbol);
    }
  }
  // This set is ordered by ascending offsets, with ties broken by greatest
  // size.  A multiset is used here because multiple symbols may have the
  // same offset and size; the symbols in the set, however, are distinct.
  std::multiset<SymbolRef, SymbolOffsetCompare> associated;
  for (SymbolRef ref : distinct) {
    associated.emplace(*ref);
  }
  std::list<std::list<SymbolRef>> result;
  std::size_t limit{0};
  const Symbol *currentCommon{nullptr};
  for (const Symbol &symbol : associated) {
    const Symbol *thisCommon{FindCommonBlockContaining(symbol)};
    if (result.empty() || symbol.offset() >= limit ||
        thisCommon != currentCommon) {
      // Start a new group
      result.emplace_back(std::list<SymbolRef>{});
      limit = 0;
      currentCommon = thisCommon;
    }
    result.back().emplace_back(symbol);
    limit = std::max(limit, symbol.offset() + symbol.size());
  }
  return result;
}

bool IsModuleProcedure(const Symbol &symbol) {
  return ClassifyProcedure(symbol) == ProcedureDefinitionClass::Module;
}

class ImageControlStmtHelper {
  using ImageControlStmts =
      std::variant<parser::ChangeTeamConstruct, parser::CriticalConstruct,
          parser::EventPostStmt, parser::EventWaitStmt, parser::FormTeamStmt,
          parser::LockStmt, parser::SyncAllStmt, parser::SyncImagesStmt,
          parser::SyncMemoryStmt, parser::SyncTeamStmt, parser::UnlockStmt>;

public:
  template <typename T> bool operator()(const T &) {
    return common::HasMember<T, ImageControlStmts>;
  }
  template <typename T> bool operator()(const common::Indirection<T> &x) {
    return (*this)(x.value());
  }
  template <typename A> bool operator()(const parser::Statement<A> &x) {
    return (*this)(x.statement);
  }
  bool operator()(const parser::AllocateStmt &stmt) {
    const auto &allocationList{std::get<std::list<parser::Allocation>>(stmt.t)};
    for (const auto &allocation : allocationList) {
      const auto &allocateObject{
          std::get<parser::AllocateObject>(allocation.t)};
      if (IsCoarrayObject(allocateObject)) {
        return true;
      }
    }
    return false;
  }
  bool operator()(const parser::DeallocateStmt &stmt) {
    const auto &allocateObjectList{
        std::get<std::list<parser::AllocateObject>>(stmt.t)};
    for (const auto &allocateObject : allocateObjectList) {
      if (IsCoarrayObject(allocateObject)) {
        return true;
      }
    }
    return false;
  }
  bool operator()(const parser::CallStmt &stmt) {
    const auto &procedureDesignator{
        std::get<parser::ProcedureDesignator>(stmt.call.t)};
    if (auto *name{std::get_if<parser::Name>(&procedureDesignator.u)}) {
      // TODO: also ensure that the procedure is, in fact, an intrinsic
      if (name->source == "move_alloc") {
        const auto &args{
            std::get<std::list<parser::ActualArgSpec>>(stmt.call.t)};
        if (!args.empty()) {
          const parser::ActualArg &actualArg{
              std::get<parser::ActualArg>(args.front().t)};
          if (const auto *argExpr{
                  std::get_if<common::Indirection<parser::Expr>>(
                      &actualArg.u)}) {
            return HasCoarray(argExpr->value());
          }
        }
      }
    }
    return false;
  }
  bool operator()(const parser::StopStmt &stmt) {
    // STOP is an image control statement; ERROR STOP is not
    return std::get<parser::StopStmt::Kind>(stmt.t) ==
        parser::StopStmt::Kind::Stop;
  }
  bool operator()(const parser::IfStmt &stmt) {
    return (*this)(
        std::get<parser::UnlabeledStatement<parser::ActionStmt>>(stmt.t)
            .statement);
  }
  bool operator()(const parser::ActionStmt &stmt) {
    return common::visit(*this, stmt.u);
  }

private:
  bool IsCoarrayObject(const parser::AllocateObject &allocateObject) {
    const parser::Name &name{GetLastName(allocateObject)};
    return name.symbol && evaluate::IsCoarray(*name.symbol);
  }
};

bool IsImageControlStmt(const parser::ExecutableConstruct &construct) {
  return common::visit(ImageControlStmtHelper{}, construct.u);
}

std::optional<parser::MessageFixedText> GetImageControlStmtCoarrayMsg(
    const parser::ExecutableConstruct &construct) {
  if (const auto *actionStmt{
          std::get_if<parser::Statement<parser::ActionStmt>>(&construct.u)}) {
    return common::visit(
        common::visitors{
            [](const common::Indirection<parser::AllocateStmt> &)
                -> std::optional<parser::MessageFixedText> {
              return "ALLOCATE of a coarray is an image control"
                     " statement"_en_US;
            },
            [](const common::Indirection<parser::DeallocateStmt> &)
                -> std::optional<parser::MessageFixedText> {
              return "DEALLOCATE of a coarray is an image control"
                     " statement"_en_US;
            },
            [](const common::Indirection<parser::CallStmt> &)
                -> std::optional<parser::MessageFixedText> {
              return "MOVE_ALLOC of a coarray is an image control"
                     " statement "_en_US;
            },
            [](const auto &) -> std::optional<parser::MessageFixedText> {
              return std::nullopt;
            },
        },
        actionStmt->statement.u);
  }
  return std::nullopt;
}

parser::CharBlock GetImageControlStmtLocation(
    const parser::ExecutableConstruct &executableConstruct) {
  return common::visit(
      common::visitors{
          [](const common::Indirection<parser::ChangeTeamConstruct>
                  &construct) {
            return std::get<parser::Statement<parser::ChangeTeamStmt>>(
                construct.value().t)
                .source;
          },
          [](const common::Indirection<parser::CriticalConstruct> &construct) {
            return std::get<parser::Statement<parser::CriticalStmt>>(
                construct.value().t)
                .source;
          },
          [](const parser::Statement<parser::ActionStmt> &actionStmt) {
            return actionStmt.source;
          },
          [](const auto &) { return parser::CharBlock{}; },
      },
      executableConstruct.u);
}

bool HasCoarray(const parser::Expr &expression) {
  if (const auto *expr{GetExpr(nullptr, expression)}) {
    for (const Symbol &symbol : evaluate::CollectSymbols(*expr)) {
      if (evaluate::IsCoarray(symbol)) {
        return true;
      }
    }
  }
  return false;
}

bool IsAssumedType(const Symbol &symbol) {
  if (const DeclTypeSpec * type{symbol.GetType()}) {
    return type->IsAssumedType();
  }
  return false;
}

bool IsPolymorphic(const Symbol &symbol) {
  if (const DeclTypeSpec * type{symbol.GetType()}) {
    return type->IsPolymorphic();
  }
  return false;
}

bool IsUnlimitedPolymorphic(const Symbol &symbol) {
  if (const DeclTypeSpec * type{symbol.GetType()}) {
    return type->IsUnlimitedPolymorphic();
  }
  return false;
}

bool IsPolymorphicAllocatable(const Symbol &symbol) {
  return IsAllocatable(symbol) && IsPolymorphic(symbol);
}

const Scope *FindCUDADeviceContext(const Scope *scope) {
  return !scope ? nullptr : FindScopeContaining(*scope, [](const Scope &s) {
    return IsCUDADeviceContext(&s);
  });
}

bool IsDeviceAllocatable(const Symbol &symbol) {
  if (IsAllocatable(symbol)) {
    if (const auto *details{
            symbol.GetUltimate().detailsIf<semantics::ObjectEntityDetails>()}) {
      if (details->cudaDataAttr() &&
          *details->cudaDataAttr() != common::CUDADataAttr::Pinned) {
        return true;
      }
    }
  }
  return false;
}

UltimateComponentIterator::const_iterator
FindCUDADeviceAllocatableUltimateComponent(const DerivedTypeSpec &derived) {
  UltimateComponentIterator ultimates{derived};
  return std::find_if(ultimates.begin(), ultimates.end(), IsDeviceAllocatable);
}

bool CanCUDASymbolBeGlobal(const Symbol &sym) {
  const Symbol &symbol{GetAssociationRoot(sym)};
  const Scope &scope{symbol.owner()};
  auto scopeKind{scope.kind()};
  const common::LanguageFeatureControl &features{
      scope.context().languageFeatures()};
  if (features.IsEnabled(common::LanguageFeature::CUDA) &&
      scopeKind == Scope::Kind::MainProgram) {
    if (const auto *details{
            sym.GetUltimate().detailsIf<semantics::ObjectEntityDetails>()}) {
      const Fortran::semantics::DeclTypeSpec *type{details->type()};
      const Fortran::semantics::DerivedTypeSpec *derived{
          type ? type->AsDerived() : nullptr};
      if (derived) {
        if (FindCUDADeviceAllocatableUltimateComponent(*derived)) {
          return false;
        }
      }
      if (details->cudaDataAttr() &&
          *details->cudaDataAttr() != common::CUDADataAttr::Unified) {
        return false;
      }
    }
  }
  return true;
}

std::optional<common::CUDADataAttr> GetCUDADataAttr(const Symbol *symbol) {
  const auto *details{
      symbol ? symbol->detailsIf<ObjectEntityDetails>() : nullptr};
  if (details) {
    const Fortran::semantics::DeclTypeSpec *type{details->type()};
    const Fortran::semantics::DerivedTypeSpec *derived{
        type ? type->AsDerived() : nullptr};
    if (derived) {
      if (FindCUDADeviceAllocatableUltimateComponent(*derived)) {
        return common::CUDADataAttr::Managed;
      }
    }
    return details->cudaDataAttr();
  }
  return std::nullopt;
}

bool IsAccessible(const Symbol &original, const Scope &scope) {
  const Symbol &ultimate{original.GetUltimate()};
  if (ultimate.attrs().test(Attr::PRIVATE)) {
    const Scope *module{FindModuleContaining(ultimate.owner())};
    return !module || module->Contains(scope);
  } else {
    return true;
  }
}

std::optional<parser::MessageFormattedText> CheckAccessibleSymbol(
    const Scope &scope, const Symbol &symbol) {
  if (IsAccessible(symbol, scope)) {
    return std::nullopt;
  } else if (FindModuleFileContaining(scope)) {
    // Don't enforce component accessibility checks in module files;
    // there may be forward-substituted named constants of derived type
    // whose structure constructors reference private components.
    return std::nullopt;
  } else {
    return parser::MessageFormattedText{
        "PRIVATE name '%s' is accessible only within module '%s'"_err_en_US,
        symbol.name(),
        DEREF(FindModuleContaining(symbol.owner())).GetName().value()};
  }
}

SymbolVector OrderParameterNames(const Symbol &typeSymbol) {
  SymbolVector result;
  if (const DerivedTypeSpec * spec{typeSymbol.GetParentTypeSpec()}) {
    result = OrderParameterNames(spec->typeSymbol());
  }
  const auto &paramNames{typeSymbol.get<DerivedTypeDetails>().paramNameOrder()};
  result.insert(result.end(), paramNames.begin(), paramNames.end());
  return result;
}

SymbolVector OrderParameterDeclarations(const Symbol &typeSymbol) {
  SymbolVector result;
  if (const DerivedTypeSpec * spec{typeSymbol.GetParentTypeSpec()}) {
    result = OrderParameterDeclarations(spec->typeSymbol());
  }
  const auto &paramDecls{typeSymbol.get<DerivedTypeDetails>().paramDeclOrder()};
  result.insert(result.end(), paramDecls.begin(), paramDecls.end());
  return result;
}

const DeclTypeSpec &FindOrInstantiateDerivedType(
    Scope &scope, DerivedTypeSpec &&spec, DeclTypeSpec::Category category) {
  spec.EvaluateParameters(scope.context());
  if (const DeclTypeSpec *
      type{scope.FindInstantiatedDerivedType(spec, category)}) {
    return *type;
  }
  // Create a new instantiation of this parameterized derived type
  // for this particular distinct set of actual parameter values.
  DeclTypeSpec &type{scope.MakeDerivedType(category, std::move(spec))};
  type.derivedTypeSpec().Instantiate(scope);
  return type;
}

const Symbol *FindSeparateModuleSubprogramInterface(const Symbol *proc) {
  if (proc) {
    if (const auto *subprogram{proc->detailsIf<SubprogramDetails>()}) {
      if (const Symbol * iface{subprogram->moduleInterface()}) {
        return iface;
      }
    }
  }
  return nullptr;
}

ProcedureDefinitionClass ClassifyProcedure(const Symbol &symbol) { // 15.2.2
  const Symbol &ultimate{symbol.GetUltimate()};
  if (!IsProcedure(ultimate)) {
    return ProcedureDefinitionClass::None;
  } else if (ultimate.attrs().test(Attr::INTRINSIC)) {
    return ProcedureDefinitionClass::Intrinsic;
  } else if (IsDummy(ultimate)) {
    return ProcedureDefinitionClass::Dummy;
  } else if (IsProcedurePointer(symbol)) {
    return ProcedureDefinitionClass::Pointer;
  } else if (ultimate.attrs().test(Attr::EXTERNAL)) {
    return ProcedureDefinitionClass::External;
  } else if (const auto *nameDetails{
                 ultimate.detailsIf<SubprogramNameDetails>()}) {
    switch (nameDetails->kind()) {
    case SubprogramKind::Module:
      return ProcedureDefinitionClass::Module;
    case SubprogramKind::Internal:
      return ProcedureDefinitionClass::Internal;
    }
  } else if (const Symbol * subp{FindSubprogram(symbol)}) {
    if (const auto *subpDetails{subp->detailsIf<SubprogramDetails>()}) {
      if (subpDetails->stmtFunction()) {
        return ProcedureDefinitionClass::StatementFunction;
      }
    }
    switch (ultimate.owner().kind()) {
    case Scope::Kind::Global:
    case Scope::Kind::IntrinsicModules:
      return ProcedureDefinitionClass::External;
    case Scope::Kind::Module:
      return ProcedureDefinitionClass::Module;
    case Scope::Kind::MainProgram:
    case Scope::Kind::Subprogram:
      return ProcedureDefinitionClass::Internal;
    default:
      break;
    }
  }
  return ProcedureDefinitionClass::None;
}

// ComponentIterator implementation

template <ComponentKind componentKind>
typename ComponentIterator<componentKind>::const_iterator
ComponentIterator<componentKind>::const_iterator::Create(
    const DerivedTypeSpec &derived) {
  const_iterator it{};
  it.componentPath_.emplace_back(derived);
  it.Increment(); // cue up first relevant component, if any
  return it;
}

template <ComponentKind componentKind>
const DerivedTypeSpec *
ComponentIterator<componentKind>::const_iterator::PlanComponentTraversal(
    const Symbol &component) const {
  if (const auto *details{component.detailsIf<ObjectEntityDetails>()}) {
    if (const DeclTypeSpec * type{details->type()}) {
      if (const auto *derived{type->AsDerived()}) {
        bool traverse{false};
        if constexpr (componentKind == ComponentKind::Ordered) {
          // Order Component (only visit parents)
          traverse = component.test(Symbol::Flag::ParentComp);
        } else if constexpr (componentKind == ComponentKind::Direct) {
          traverse = !IsAllocatableOrObjectPointer(&component);
        } else if constexpr (componentKind == ComponentKind::Ultimate) {
          traverse = !IsAllocatableOrObjectPointer(&component);
        } else if constexpr (componentKind == ComponentKind::Potential) {
          traverse = !IsPointer(component);
        } else if constexpr (componentKind == ComponentKind::Scope) {
          traverse = !IsAllocatableOrObjectPointer(&component);
        } else if constexpr (componentKind ==
            ComponentKind::PotentialAndPointer) {
          traverse = !IsPointer(component);
        }
        if (traverse) {
          const Symbol &newTypeSymbol{derived->typeSymbol()};
          // Avoid infinite loop if the type is already part of the types
          // being visited. It is possible to have "loops in type" because
          // C744 does not forbid to use not yet declared type for
          // ALLOCATABLE or POINTER components.
          for (const auto &node : componentPath_) {
            if (&newTypeSymbol == &node.GetTypeSymbol()) {
              return nullptr;
            }
          }
          return derived;
        }
      }
    } // intrinsic & unlimited polymorphic not traversable
  }
  return nullptr;
}

template <ComponentKind componentKind>
static bool StopAtComponentPre(const Symbol &component) {
  if constexpr (componentKind == ComponentKind::Ordered) {
    // Parent components need to be iterated upon after their
    // sub-components in structure constructor analysis.
    return !component.test(Symbol::Flag::ParentComp);
  } else if constexpr (componentKind == ComponentKind::Direct) {
    return true;
  } else if constexpr (componentKind == ComponentKind::Ultimate) {
    return component.has<ProcEntityDetails>() ||
        IsAllocatableOrObjectPointer(&component) ||
        (component.has<ObjectEntityDetails>() &&
            component.get<ObjectEntityDetails>().type() &&
            component.get<ObjectEntityDetails>().type()->AsIntrinsic());
  } else if constexpr (componentKind == ComponentKind::Potential) {
    return !IsPointer(component);
  } else if constexpr (componentKind == ComponentKind::PotentialAndPointer) {
    return true;
  } else {
    DIE("unexpected ComponentKind");
  }
}

template <ComponentKind componentKind>
static bool StopAtComponentPost(const Symbol &component) {
  return componentKind == ComponentKind::Ordered &&
      component.test(Symbol::Flag::ParentComp);
}

template <ComponentKind componentKind>
void ComponentIterator<componentKind>::const_iterator::Increment() {
  while (!componentPath_.empty()) {
    ComponentPathNode &deepest{componentPath_.back()};
    if (deepest.component()) {
      if (!deepest.descended()) {
        deepest.set_descended(true);
        if (const DerivedTypeSpec *
            derived{PlanComponentTraversal(*deepest.component())}) {
          componentPath_.emplace_back(*derived);
          continue;
        }
      } else if (!deepest.visited()) {
        deepest.set_visited(true);
        return; // this is the next component to visit, after descending
      }
    }
    auto &nameIterator{deepest.nameIterator()};
    if (nameIterator == deepest.nameEnd()) {
      componentPath_.pop_back();
    } else if constexpr (componentKind == ComponentKind::Scope) {
      deepest.set_component(*nameIterator++->second);
      deepest.set_descended(false);
      deepest.set_visited(true);
      return; // this is the next component to visit, before descending
    } else {
      const Scope &scope{deepest.GetScope()};
      auto scopeIter{scope.find(*nameIterator++)};
      if (scopeIter != scope.cend()) {
        const Symbol &component{*scopeIter->second};
        deepest.set_component(component);
        deepest.set_descended(false);
        if (StopAtComponentPre<componentKind>(component)) {
          deepest.set_visited(true);
          return; // this is the next component to visit, before descending
        } else {
          deepest.set_visited(!StopAtComponentPost<componentKind>(component));
        }
      }
    }
  }
}

template <ComponentKind componentKind>
SymbolVector
ComponentIterator<componentKind>::const_iterator::GetComponentPath() const {
  SymbolVector result;
  for (const auto &node : componentPath_) {
    result.push_back(DEREF(node.component()));
  }
  return result;
}

template <ComponentKind componentKind>
std::string
ComponentIterator<componentKind>::const_iterator::BuildResultDesignatorName()
    const {
  std::string designator;
  for (const Symbol &component : GetComponentPath()) {
    designator += "%"s + component.name().ToString();
  }
  return designator;
}

template class ComponentIterator<ComponentKind::Ordered>;
template class ComponentIterator<ComponentKind::Direct>;
template class ComponentIterator<ComponentKind::Ultimate>;
template class ComponentIterator<ComponentKind::Potential>;
template class ComponentIterator<ComponentKind::Scope>;
template class ComponentIterator<ComponentKind::PotentialAndPointer>;

PotentialComponentIterator::const_iterator FindCoarrayPotentialComponent(
    const DerivedTypeSpec &derived) {
  PotentialComponentIterator potentials{derived};
  return std::find_if(potentials.begin(), potentials.end(),
      [](const Symbol &symbol) { return evaluate::IsCoarray(symbol); });
}

PotentialAndPointerComponentIterator::const_iterator
FindPointerPotentialComponent(const DerivedTypeSpec &derived) {
  PotentialAndPointerComponentIterator potentials{derived};
  return std::find_if(potentials.begin(), potentials.end(), IsPointer);
}

UltimateComponentIterator::const_iterator FindCoarrayUltimateComponent(
    const DerivedTypeSpec &derived) {
  UltimateComponentIterator ultimates{derived};
  return std::find_if(ultimates.begin(), ultimates.end(),
      [](const Symbol &symbol) { return evaluate::IsCoarray(symbol); });
}

UltimateComponentIterator::const_iterator FindPointerUltimateComponent(
    const DerivedTypeSpec &derived) {
  UltimateComponentIterator ultimates{derived};
  return std::find_if(ultimates.begin(), ultimates.end(), IsPointer);
}

PotentialComponentIterator::const_iterator FindEventOrLockPotentialComponent(
    const DerivedTypeSpec &derived, bool ignoreCoarrays) {
  PotentialComponentIterator potentials{derived};
  auto iter{potentials.begin()};
  for (auto end{potentials.end()}; iter != end; ++iter) {
    const Symbol &component{*iter};
    if (const auto *object{component.detailsIf<ObjectEntityDetails>()}) {
      if (const DeclTypeSpec * type{object->type()}) {
        if (IsEventTypeOrLockType(type->AsDerived())) {
          if (!ignoreCoarrays) {
            break; // found one
          }
          auto path{iter.GetComponentPath()};
          path.pop_back();
          if (std::find_if(path.begin(), path.end(), [](const Symbol &sym) {
                return evaluate::IsCoarray(sym);
              }) == path.end()) {
            break; // found one not in a coarray
          }
        }
      }
    }
  }
  return iter;
}

UltimateComponentIterator::const_iterator FindAllocatableUltimateComponent(
    const DerivedTypeSpec &derived) {
  UltimateComponentIterator ultimates{derived};
  return std::find_if(ultimates.begin(), ultimates.end(), IsAllocatable);
}

DirectComponentIterator::const_iterator FindAllocatableOrPointerDirectComponent(
    const DerivedTypeSpec &derived) {
  DirectComponentIterator directs{derived};
  return std::find_if(directs.begin(), directs.end(), IsAllocatableOrPointer);
}

PotentialComponentIterator::const_iterator
FindPolymorphicAllocatablePotentialComponent(const DerivedTypeSpec &derived) {
  PotentialComponentIterator potentials{derived};
  return std::find_if(
      potentials.begin(), potentials.end(), IsPolymorphicAllocatable);
}

const Symbol *FindUltimateComponent(const DerivedTypeSpec &derived,
    const std::function<bool(const Symbol &)> &predicate) {
  UltimateComponentIterator ultimates{derived};
  if (auto it{std::find_if(ultimates.begin(), ultimates.end(),
          [&predicate](const Symbol &component) -> bool {
            return predicate(component);
          })}) {
    return &*it;
  }
  return nullptr;
}

const Symbol *FindUltimateComponent(const Symbol &symbol,
    const std::function<bool(const Symbol &)> &predicate) {
  if (predicate(symbol)) {
    return &symbol;
  } else if (const auto *object{symbol.detailsIf<ObjectEntityDetails>()}) {
    if (const auto *type{object->type()}) {
      if (const auto *derived{type->AsDerived()}) {
        return FindUltimateComponent(*derived, predicate);
      }
    }
  }
  return nullptr;
}

const Symbol *FindImmediateComponent(const DerivedTypeSpec &type,
    const std::function<bool(const Symbol &)> &predicate) {
  if (const Scope * scope{type.scope()}) {
    const Symbol *parent{nullptr};
    for (const auto &pair : *scope) {
      const Symbol *symbol{&*pair.second};
      if (predicate(*symbol)) {
        return symbol;
      }
      if (symbol->test(Symbol::Flag::ParentComp)) {
        parent = symbol;
      }
    }
    if (parent) {
      if (const auto *object{parent->detailsIf<ObjectEntityDetails>()}) {
        if (const auto *type{object->type()}) {
          if (const auto *derived{type->AsDerived()}) {
            return FindImmediateComponent(*derived, predicate);
          }
        }
      }
    }
  }
  return nullptr;
}

const Symbol *IsFunctionResultWithSameNameAsFunction(const Symbol &symbol) {
  if (IsFunctionResult(symbol)) {
    if (const Symbol * function{symbol.owner().symbol()}) {
      if (symbol.name() == function->name()) {
        return function;
      }
    }
    // Check ENTRY result symbols too
    const Scope &outer{symbol.owner().parent()};
    auto iter{outer.find(symbol.name())};
    if (iter != outer.end()) {
      const Symbol &outerSym{*iter->second};
      if (const auto *subp{outerSym.detailsIf<SubprogramDetails>()}) {
        if (subp->entryScope() == &symbol.owner() &&
            symbol.name() == outerSym.name()) {
          return &outerSym;
        }
      }
    }
  }
  return nullptr;
}

void LabelEnforce::Post(const parser::GotoStmt &gotoStmt) {
  CheckLabelUse(gotoStmt.v);
}
void LabelEnforce::Post(const parser::ComputedGotoStmt &computedGotoStmt) {
  for (auto &i : std::get<std::list<parser::Label>>(computedGotoStmt.t)) {
    CheckLabelUse(i);
  }
}

void LabelEnforce::Post(const parser::ArithmeticIfStmt &arithmeticIfStmt) {
  CheckLabelUse(std::get<1>(arithmeticIfStmt.t));
  CheckLabelUse(std::get<2>(arithmeticIfStmt.t));
  CheckLabelUse(std::get<3>(arithmeticIfStmt.t));
}

void LabelEnforce::Post(const parser::AssignStmt &assignStmt) {
  CheckLabelUse(std::get<parser::Label>(assignStmt.t));
}

void LabelEnforce::Post(const parser::AssignedGotoStmt &assignedGotoStmt) {
  for (auto &i : std::get<std::list<parser::Label>>(assignedGotoStmt.t)) {
    CheckLabelUse(i);
  }
}

void LabelEnforce::Post(const parser::AltReturnSpec &altReturnSpec) {
  CheckLabelUse(altReturnSpec.v);
}

void LabelEnforce::Post(const parser::ErrLabel &errLabel) {
  CheckLabelUse(errLabel.v);
}
void LabelEnforce::Post(const parser::EndLabel &endLabel) {
  CheckLabelUse(endLabel.v);
}
void LabelEnforce::Post(const parser::EorLabel &eorLabel) {
  CheckLabelUse(eorLabel.v);
}

void LabelEnforce::CheckLabelUse(const parser::Label &labelUsed) {
  if (labels_.find(labelUsed) == labels_.end()) {
    SayWithConstruct(context_, currentStatementSourcePosition_,
        parser::MessageFormattedText{
            "Control flow escapes from %s"_err_en_US, construct_},
        constructSourcePosition_);
  }
}

parser::MessageFormattedText LabelEnforce::GetEnclosingConstructMsg() {
  return {"Enclosing %s statement"_en_US, construct_};
}

void LabelEnforce::SayWithConstruct(SemanticsContext &context,
    parser::CharBlock stmtLocation, parser::MessageFormattedText &&message,
    parser::CharBlock constructLocation) {
  context.Say(stmtLocation, message)
      .Attach(constructLocation, GetEnclosingConstructMsg());
}

bool HasAlternateReturns(const Symbol &subprogram) {
  for (const auto *dummyArg : subprogram.get<SubprogramDetails>().dummyArgs()) {
    if (!dummyArg) {
      return true;
    }
  }
  return false;
}

bool IsAutomaticallyDestroyed(const Symbol &symbol) {
  return symbol.has<ObjectEntityDetails>() &&
      (symbol.owner().kind() == Scope::Kind::Subprogram ||
          symbol.owner().kind() == Scope::Kind::BlockConstruct) &&
      !IsNamedConstant(symbol) && (!IsDummy(symbol) || IsIntentOut(symbol)) &&
      !IsPointer(symbol) && !IsSaved(symbol) &&
      !FindCommonBlockContaining(symbol);
}

const std::optional<parser::Name> &MaybeGetNodeName(
    const ConstructNode &construct) {
  return common::visit(
      common::visitors{
          [&](const parser::BlockConstruct *blockConstruct)
              -> const std::optional<parser::Name> & {
            return std::get<0>(blockConstruct->t).statement.v;
          },
          [&](const auto *a) -> const std::optional<parser::Name> & {
            return std::get<0>(std::get<0>(a->t).statement.t);
          },
      },
      construct);
}

std::optional<ArraySpec> ToArraySpec(
    evaluate::FoldingContext &context, const evaluate::Shape &shape) {
  if (auto extents{evaluate::AsConstantExtents(context, shape)};
      extents && !evaluate::HasNegativeExtent(*extents)) {
    ArraySpec result;
    for (const auto &extent : *extents) {
      result.emplace_back(ShapeSpec::MakeExplicit(Bound{extent}));
    }
    return {std::move(result)};
  } else {
    return std::nullopt;
  }
}

std::optional<ArraySpec> ToArraySpec(evaluate::FoldingContext &context,
    const std::optional<evaluate::Shape> &shape) {
  return shape ? ToArraySpec(context, *shape) : std::nullopt;
}

static const DeclTypeSpec *GetDtvArgTypeSpec(const Symbol &proc) {
  if (const auto *subp{proc.detailsIf<SubprogramDetails>()};
      subp && !subp->dummyArgs().empty()) {
    if (const auto *arg{subp->dummyArgs()[0]}) {
      return arg->GetType();
    }
  }
  return nullptr;
}

const DerivedTypeSpec *GetDtvArgDerivedType(const Symbol &proc) {
  if (const auto *type{GetDtvArgTypeSpec(proc)}) {
    return type->AsDerived();
  } else {
    return nullptr;
  }
}

bool HasDefinedIo(common::DefinedIo which, const DerivedTypeSpec &derived,
    const Scope *scope) {
  if (const Scope * dtScope{derived.scope()}) {
    for (const auto &pair : *dtScope) {
      const Symbol &symbol{*pair.second};
      if (const auto *generic{symbol.detailsIf<GenericDetails>()}) {
        GenericKind kind{generic->kind()};
        if (const auto *io{std::get_if<common::DefinedIo>(&kind.u)}) {
          if (*io == which) {
            return true; // type-bound GENERIC exists
          }
        }
      }
    }
  }
  if (scope) {
    SourceName name{GenericKind::AsFortran(which)};
    evaluate::DynamicType dyDerived{derived};
    for (; scope && !scope->IsGlobal(); scope = &scope->parent()) {
      auto iter{scope->find(name)};
      if (iter != scope->end()) {
        const auto &generic{iter->second->GetUltimate().get<GenericDetails>()};
        for (auto ref : generic.specificProcs()) {
          const Symbol &procSym{ref->GetUltimate()};
          if (const DeclTypeSpec * dtSpec{GetDtvArgTypeSpec(procSym)}) {
            if (auto dyDummy{evaluate::DynamicType::From(*dtSpec)}) {
              if (dyDummy->IsTkCompatibleWith(dyDerived)) {
                return true; // GENERIC or INTERFACE not in type
              }
            }
          }
        }
      }
    }
  }
  // Check for inherited defined I/O
  const auto *parentType{derived.typeSymbol().GetParentTypeSpec()};
  return parentType && HasDefinedIo(which, *parentType, scope);
}

template <typename E>
std::forward_list<std::string> GetOperatorNames(
    const SemanticsContext &context, E opr) {
  std::forward_list<std::string> result;
  for (const char *name : context.languageFeatures().GetNames(opr)) {
    result.emplace_front("operator("s + name + ')');
  }
  return result;
}

std::forward_list<std::string> GetAllNames(
    const SemanticsContext &context, const SourceName &name) {
  std::string str{name.ToString()};
  if (!name.empty() && name.back() == ')' &&
      name.ToString().rfind("operator(", 0) == 0) {
    for (int i{0}; i != common::LogicalOperator_enumSize; ++i) {
      auto names{GetOperatorNames(context, common::LogicalOperator{i})};
      if (llvm::is_contained(names, str)) {
        return names;
      }
    }
    for (int i{0}; i != common::RelationalOperator_enumSize; ++i) {
      auto names{GetOperatorNames(context, common::RelationalOperator{i})};
      if (llvm::is_contained(names, str)) {
        return names;
      }
    }
  }
  return {str};
}

void WarnOnDeferredLengthCharacterScalar(SemanticsContext &context,
    const SomeExpr *expr, parser::CharBlock at, const char *what) {
  if (context.languageFeatures().ShouldWarn(
          common::UsageWarning::F202XAllocatableBreakingChange)) {
    if (const Symbol *
        symbol{evaluate::UnwrapWholeSymbolOrComponentDataRef(expr)}) {
      const Symbol &ultimate{ResolveAssociations(*symbol)};
      if (const DeclTypeSpec * type{ultimate.GetType()}; type &&
          type->category() == DeclTypeSpec::Category::Character &&
          type->characterTypeSpec().length().isDeferred() &&
          IsAllocatable(ultimate) && ultimate.Rank() == 0) {
        context.Say(at,
            "The deferred length allocatable character scalar variable '%s' may be reallocated to a different length under the new Fortran 202X standard semantics for %s"_port_en_US,
            symbol->name(), what);
      }
    }
  }
}

bool CouldBeDataPointerValuedFunction(const Symbol *original) {
  if (original) {
    const Symbol &ultimate{original->GetUltimate()};
    if (const Symbol * result{FindFunctionResult(ultimate)}) {
      return IsPointer(*result) && !IsProcedure(*result);
    }
    if (const auto *generic{ultimate.detailsIf<GenericDetails>()}) {
      for (const SymbolRef &ref : generic->specificProcs()) {
        if (CouldBeDataPointerValuedFunction(&*ref)) {
          return true;
        }
      }
    }
  }
  return false;
}

std::string GetModuleOrSubmoduleName(const Symbol &symbol) {
  const auto &details{symbol.get<ModuleDetails>()};
  std::string result{symbol.name().ToString()};
  if (details.ancestor() && details.ancestor()->symbol()) {
    result = details.ancestor()->symbol()->name().ToString() + ':' + result;
  }
  return result;
}

std::string GetCommonBlockObjectName(const Symbol &common, bool underscoring) {
  if (const std::string * bind{common.GetBindName()}) {
    return *bind;
  }
  if (common.name().empty()) {
    return Fortran::common::blankCommonObjectName;
  }
  return underscoring ? common.name().ToString() + "_"s
                      : common.name().ToString();
}

bool HadUseError(
    SemanticsContext &context, SourceName at, const Symbol *symbol) {
  if (const auto *details{
          symbol ? symbol->detailsIf<UseErrorDetails>() : nullptr}) {
    auto &msg{context.Say(
        at, "Reference to '%s' is ambiguous"_err_en_US, symbol->name())};
    for (const auto &[location, sym] : details->occurrences()) {
      const Symbol &ultimate{sym->GetUltimate()};
      if (sym->owner().IsModule()) {
        auto &attachment{msg.Attach(location,
            "'%s' was use-associated from module '%s'"_en_US, at,
            sym->owner().GetName().value())};
        if (&*sym != &ultimate) {
          // For incompatible definitions where one comes from a hermetic
          // module file's incorporated dependences and the other from another
          // module of the same name.
          attachment.Attach(ultimate.name(),
              "ultimately from '%s' in module '%s'"_en_US, ultimate.name(),
              ultimate.owner().GetName().value());
        }
      } else {
        msg.Attach(sym->name(), "declared here"_en_US);
      }
    }
    context.SetError(*symbol);
    return true;
  } else {
    return false;
  }
}

} // namespace Fortran::semantics
