//===--- REPLTransforms.h - AST transforms for the Swift REPL -------------===//
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
///
/// \file
/// Post-type-check AST transforms applied to each REPL input before SIL
/// lowering.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IMMEDIATE_REPLTRANSFORMS_H
#define SWIFT_IMMEDIATE_REPLTRANSFORMS_H

#include "swift/Basic/LLVM.h"

namespace swift {
class FuncDecl;
class SourceFile;

/// Wraps all TopLevelCodeDecl bodies found in \p SF into a single synthesized
/// \c () -> Void function named \p funcName, removing those TopLevelCodeDecl
/// nodes from \p SF.  If no TopLevelCodeDecl nodes are present (pure
/// declaration input), an empty stub function is still added so the JIT always
/// has a well-known entry point to invoke.
///
/// Must be called after type-checking and before SIL lowering.
///
/// Returns a pointer to the synthesized FuncDecl so callers can compute its
/// linker-level symbol via SILDeclRef(result).mangle().
FuncDecl *wrapTopLevelCodeInFunction(SourceFile &SF, StringRef funcName);

/// Walks all declarations in \p SF and promotes their access level to
/// \c public (or \c open for non-final classes and syntactically overridable
/// members).  This is required before SIL lowering so that the JIT can
/// resolve every declaration emitted by previous REPL cells.
///
/// Property-wrapper backing variables inside structs are intentionally left
/// alone because changing their access would break the implicit memberwise
/// initialiser that the compiler synthesises for them.
///
/// Must be called after wrapTopLevelCodeInFunction() and before SIL lowering.
void makeDeclarationsPublic(SourceFile &SF);

/// Returns true if \p WrapperFunc's body contains exactly one expression
/// whose post-typecheck type is non-Void and error-free.
///
/// This is the cheapest possible test for "was this REPL input a bare
/// value-producing expression?": after wrapTopLevelCodeInFunction() has run,
/// declarations produce an empty wrapper body, void-returning calls produce a
/// ()-typed expression, and bare value expressions (e.g. \c 1+1, \c x)
/// produce a single non-Void Expr*.
///
/// Must be called after type-checking and wrapTopLevelCodeInFunction().
bool hasSingleNonVoidBareExpr(FuncDecl *WrapperFunc);

} // end namespace swift

#endif // SWIFT_IMMEDIATE_REPLTRANSFORMS_H
