//===--- Interpreter.h - Swift incremental REPL interpreter ---------------===//
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
/// Declares swift::Interpreter -- the public API for incremental Swift
/// execution, directly analogous to clang::Interpreter in clang-repl.
///
/// The caller is responsible for setting up and owning the CompilerInstance,
/// mirroring how the Swift frontend calls runREPL(Instance, ...) from
/// performAction().  Embedders such as xeus-swift follow the same pattern:
/// parse args into a CompilerInvocation, call CI.setup(), then create an
/// Interpreter and call parseAndExecute() for each input cell.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IMMEDIATE_INTERPRETER_H
#define SWIFT_IMMEDIATE_INTERPRETER_H

#include "swift/Immediate/Immediate.h"   // ProcessCmdLine
#include "llvm/ADT/StringRef.h"
#include <memory>

namespace swift {
class CompilerInstance;
class ModuleDecl;
class SwiftJIT;

/// Per-session compiler and execution environment for incremental Swift.
///
/// Analogous to clang::Interpreter: the caller owns the CompilerInstance and
/// is responsible for keeping it alive for the duration of the session.
///
/// Typical usage from an embedding tool:
/// \code
///   swift::CompilerInvocation invocation;
///   invocation.parseArgs(args, diags);
///   invocation.getFrontendOptions().RequestedAction = ActionType::REPL;
///
///   swift::CompilerInstance CI;
///   CI.setup(invocation, err, args);
///
///   swift::Interpreter interp(CI, swift::ProcessCmdLine{}, parseStdlib);
///   interp.parseAndExecute("let x = 42");
///   interp.parseAndExecute("print(x)");  // prints "42"
/// \endcode
class Interpreter {
  CompilerInstance &CI;
  ModuleDecl *MostRecentModule;
  unsigned InputNumber = 1;
  /// Long-lived JIT session -- all cells share the same JITDylib.
  std::unique_ptr<SwiftJIT> JIT;

public:
  Interpreter(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
              bool ParseStdlib);

  /// Destructor defined in REPL.cpp where SwiftJIT is a complete type.
  ~Interpreter();

  Interpreter(const Interpreter &) = delete;
  Interpreter &operator=(const Interpreter &) = delete;

  unsigned getInputNumber() const { return InputNumber; }

  /// Returns true if the JIT session was successfully initialised.
  bool isReady() const { return (bool)JIT; }

  /// Parse and JIT-execute one input cell.
  ///
  /// \returns true to continue the session, false to quit (e.g. ':quit').
  bool parseAndExecute(llvm::StringRef Line);
};

} // namespace swift

#endif // SWIFT_IMMEDIATE_INTERPRETER_H
