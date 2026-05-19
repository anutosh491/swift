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
#include <string>
#include <vector>

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
  /// Counter for auto-printed result variables ($R0, $R1, …).
  unsigned ResultIdx = 0;
  /// Long-lived JIT session -- all cells share the same JITDylib.
  std::unique_ptr<SwiftJIT> JIT;
  /// Swift function called for last-value auto-display (e.g. "_xeusAutoDisplay").
  /// Empty string means use the default Swift.print behaviour.
  std::string DisplayFunc;

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

  /// Result of an input-completeness check.
  struct InputCompleteness {
    /// True if the input is syntactically complete and ready to execute.
    bool IsComplete;
    /// Leading whitespace of the line containing the last unbalanced opener
    /// ('{'/'('/'[').  Clients use this as the base indentation prefix.
    std::string IndentPrefix;
    /// Number of additional indent levels beyond IndentPrefix.
    uint32_t IndentLevel;
  };

  /// Check whether \p Text is syntactically complete (all brackets balanced,
  /// no dangling expression).  Used by the REPL loop to decide whether to
  /// show a continuation prompt instead of executing immediately, and by
  /// Jupyter kernels to implement the is_complete_request handler.
  InputCompleteness isInputComplete(llvm::StringRef Text) const;

  /// Outcome of a single parseAndExecute call.
  enum class REPLResult {
    /// Cell was parsed, compiled, and JIT-executed without errors.
    Success,
    /// Non-fatal compile/type-check/SIL error.  The session continues; the
    /// caller should surface the diagnostic text to the user.
    CompileError,
    /// The session must end (:quit command or unrecoverable JIT failure).
    Fatal,
  };

  /// Register a Swift function name for last-value auto-display.
  ///
  /// When set, auto-print generates:
  /// \code
  ///   let __repl_rN = (expr)
  ///   funcName(__repl_rN, "$RN")
  /// \endcode
  ///
  /// The function must be defined with two overloads — one constrained on the
  /// rich-display protocol, one unconstrained fallback:
  /// \code
  ///   // Rich display for MimeBundleRepresentable (or any protocol):
  ///   public func funcName<T: Protocol>(_ v: T, _ label: String) { v.method() }
  ///   // Plain fallback for every other type:
  ///   public func funcName<T>(_ v: T, _ label: String) { Swift.print(...) }
  /// \endcode
  ///
  /// Swift resolves the correct overload at typecheck time (concrete type
  /// is always known in the auto-print wrapper) — no runtime conformance
  /// lookup, no \c as? cast, no existential boxing overhead.
  ///
  /// If \p funcName is not yet in scope at typecheck time, the wrapped
  /// typecheck fails silently (DiagnosticSuppression) and the expression
  /// runs with no output — safe fallback.
  ///
  /// Pass an empty string to restore the default \c Swift.print behaviour.
  void setLastValueDisplayFunc(std::string funcName);

  /// Load a dynamic library into the process at runtime.
  ///
  /// Analogous to \c clang::Interpreter::LoadDynamicLibrary.  Calls
  /// \c dlopen(path, RTLD_LAZY | RTLD_GLOBAL) so the library's symbols become
  /// visible to the \c DynamicLibrarySearchGenerator on the next JIT link step.
  ///
  /// Use this for pure-C dylibs (no .swiftmodule) or as an escape hatch for
  /// libraries whose .swiftmodule lacks \c -module-link-name metadata.
  /// For well-built Swift modules, prefer passing \c -L to the kernel so that
  /// \c autolinkImportedModules resolves the library automatically on
  /// \c import.
  ///
  /// \returns \c true on success, \c false if \c dlopen failed (message on
  ///          stderr).
  bool loadLibrary(llvm::StringRef path);

  /// Parse and JIT-execute one input cell.
  ///
  /// \returns the outcome — Success, CompileError, or Fatal.
  REPLResult parseAndExecute(llvm::StringRef Line);

  /// One-shot code-completion result for an embedding UI (e.g. Jupyter).
  ///
  /// \p Matches contains the full insertable strings (prefix typed by the
  /// user + completion suffix), e.g. \c "remove(at:" from \c "remov".
  /// \p Prefix is the partial identifier the user typed; callers use
  /// \c cursor_start = cursor_pos - Prefix.size() to define the replacement
  /// region passed back to the Jupyter \c complete_reply.
  struct CompletionResult {
    std::vector<std::string> Matches;
    std::string Prefix;
  };

  /// Compute completions for \p Code (text up to the cursor).
  ///
  /// Uses Swift's REPL code-completion engine internally.  Safe to call at any
  /// point in the session; diagnostics generated during completion are
  /// suppressed and do not affect subsequent executions.
  CompletionResult complete(llvm::StringRef Code) const;
};

} // namespace swift

#endif // SWIFT_IMMEDIATE_INTERPRETER_H
