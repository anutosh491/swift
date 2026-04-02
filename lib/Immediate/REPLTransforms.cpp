//===--- REPLTransforms.cpp - AST transforms for the Swift REPL -----------===//
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

#include "REPLTransforms.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTNode.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/STLExtras.h"

using namespace swift;

FuncDecl *swift::wrapTopLevelCodeInFunction(SourceFile &SF, StringRef funcName) {
  ASTContext &Ctx = SF.getASTContext();

  // Collect statements from all TopLevelCodeDecl nodes and build a new
  // top-level item list that excludes them.  TLCDs exist because the parser
  // wraps executable statements in them when allowsTopLevelCode() is true;
  // after type-checking we move those bodies into a single named function so
  // the JIT has a normal FuncDecl it can look up and invoke.
  SmallVector<ASTNode, 8> stmts;
  SmallVector<ASTNode, 16> newItems;

  for (auto &item : SF.getTopLevelItems()) {
    auto *D = item.dyn_cast<Decl *>();
    if (D) {
      if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
        for (auto &elem : TLCD->getBody()->getElements())
          stmts.push_back(elem);
        // Drop this TLCD — its body is being moved into the wrapper below.
        continue;
      }
    }
    newItems.push_back(item);
  }

  // Build the wrapper function: func <funcName>() -> Void
  auto *params = ParameterList::createEmpty(Ctx);
  DeclName name(Ctx, Ctx.getIdentifier(funcName), params);
  auto *func = FuncDecl::createImplicit(
      Ctx, StaticSpellingKind::None, name, /*NameLoc=*/SourceLoc(),
      /*Async=*/false, /*Throws=*/false, /*ThrownType=*/Type(),
      /*GenericParams=*/nullptr, params,
      /*FnRetType=*/Ctx.TheEmptyTupleType, &SF);

  // createImplicit only calls setResultInterfaceType; explicitly set the full
  // interface type so SILGen can lower this function without a type-check pass.
  // The type is trivially () -> Void — no inference needed.
  // Must pass an explicit ExtInfo; the no-argument overload produces the
  // @_NO_EXTINFO sentinel (hasExtInfo()==false) which causes the mangler to
  // assert when it calls getExtInfo().  A default-constructed ExtInfoBuilder
  // gives @convention(swift), non-async, non-throwing — correct for a plain
  // module-level wrapper function.
  func->setInterfaceType(FunctionType::get(
      {}, Ctx.TheEmptyTupleType, AnyFunctionType::ExtInfoBuilder().build()));

  // This is a synthesized REPL wrapper, but it is a first-class callable that
  // the JIT will look up by name — not an invisible compiler-internal node.
  func->setImplicit(false);

  // Move the collected statements (already type-checked) into the body.
  auto *body = BraceStmt::create(Ctx, SourceLoc(), stmts, SourceLoc(),
                                  /*implicit=*/true);
  func->setBody(body, AbstractFunctionDecl::BodyKind::TypeChecked);

  // Make the function public so the JIT can look it up by name.
  //
  // Two separate steps are required:
  // 1. setAccess() writes AccessLevel into the TypeAndAccess bitfield.
  //    SILDeclRef::getDefinitionLinkage() reads getEffectiveAccess() through
  //    this bitfield → SILLinkage::Public.  This is what matters for JIT.
  // 2. Adding an AccessControlAttr to getAttrs() is needed for the SIL module
  //    printer (PrintOptions::printSIL() has PrintAccess=false, so it only
  //    prints access if an explicit attr is present).  Without this the
  //    declarations header shows "func __repl_N()" instead of
  //    "public func __repl_N()" — purely cosmetic, but confusing.
  func->setAccess(AccessLevel::Public);
  func->getAttrs().add(new (Ctx) AccessControlAttr(
      SourceLoc(), SourceRange(), AccessLevel::Public, /*implicit=*/true));

  // Replace the source file's item list (TLCDs dropped, FuncDecl appended).
  newItems.push_back(func);
  SF.replaceTopLevelItems(newItems);

  // Invariant: no TopLevelCodeDecl nodes remain after this transform.
  assert(llvm::none_of(SF.getTopLevelDecls(),
                       [](Decl *D) { return isa<TopLevelCodeDecl>(D); }) &&
         "wrapTopLevelCodeInFunction: all TopLevelCodeDecl nodes must be gone");

  return func;
}

void swift::makeDeclarationsPublic(SourceFile &SF) {
  class Publicist : public ASTWalker {
    /// Returns false for declarations whose access level must NOT be raised.
    ///
    /// Property-wrapper backing storage inside a struct gets a compiler-
    /// synthesised memberwise initialiser whose parameter labels and types
    /// are derived from the stored property access levels.  Overwriting those
    /// access levels after synthesis breaks the initialiser, so we skip:
    ///   - vars that have an attached property wrapper
    ///   - vars that are the backing storage of a wrapped property
    ///   - accessors whose storage falls into either category above
    static bool canMakePublic(Decl *D) {
      if (llvm::isa<StructDecl>(D->getDeclContext())) {
        if (auto *var = llvm::dyn_cast<VarDecl>(D)) {
          if (var->hasAttachedPropertyWrapper() ||
              var->getOriginalWrappedProperty())
            return false;
          return true;
        }
        if (auto *accessor = llvm::dyn_cast<AccessorDecl>(D))
          return canMakePublic(accessor->getStorage());
      }
      return true;
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      if (!canMakePublic(D))
        return Action::Continue();

      if (auto *VD = llvm::dyn_cast<ValueDecl>(D)) {
        // Default: public access so the JIT and later REPL cells can see it.
        auto access = AccessLevel::Public;

        // Non-final classes and syntactically overridable members (methods,
        // properties, subscripts) should be 'open' so that later cells can
        // subclass or override them — 'public' would block that.
        if (llvm::isa<ClassDecl>(VD) || VD->isSyntacticallyOverridable()) {
          if (!VD->isFinal())
            access = AccessLevel::Open;
        }

        VD->overwriteAccess(access);
        // Raise setter access in lockstep for stored/computed properties and
        // subscripts so that 'var' declarations remain mutable from other cells.
        if (auto *ASD = llvm::dyn_cast<AbstractStorageDecl>(D))
          ASD->overwriteSetterAccess(access);
      }
      return Action::Continue();
    }
  };

  Publicist p;
  SF.walk(p);
}

bool swift::hasSingleNonVoidBareExpr(FuncDecl *WrapperFunc) {
  auto *Body = WrapperFunc->getBody();
  if (!Body || Body->getNumElements() != 1)
    return false;
  auto *E = Body->getElements()[0].dyn_cast<Expr *>();
  if (!E)
    return false;
  auto Ty = E->getType();
  return Ty && !Ty->hasError() && !Ty->isVoid();
}
