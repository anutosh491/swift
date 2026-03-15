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
class SourceFile;

/// Wraps all TopLevelCodeDecl bodies found in \p SF into a single synthesized
/// \c () -> Void function named \p funcName, removing those TopLevelCodeDecl
/// nodes from \p SF.  If no TopLevelCodeDecl nodes are present (pure
/// declaration input), an empty stub function is still added so the JIT always
/// has a well-known entry point to invoke.
///
/// Must be called after type-checking and before SIL lowering.
void wrapTopLevelCodeInFunction(SourceFile &SF, StringRef funcName);

} // end namespace swift

#endif // SWIFT_IMMEDIATE_REPLTRANSFORMS_H
