//===--- WasmExecutor.h - Wasm incremental executor for Swift REPL -*-C++-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Declares SwiftWasmExecutor, which replaces ORC LLJIT for the
// Emscripten/browser target.  For each REPL cell it emits a wasm32 object
// file, links it into a SIDE_MODULE .wasm via wasm-ld, and executes the
// resulting wrapper function via dlopen/dlsym.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IMMEDIATE_WASMEXECUTOR_H
#define SWIFT_IMMEDIATE_WASMEXECUTOR_H

#ifdef __EMSCRIPTEN__

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace llvm {
class Module;
} // namespace llvm

namespace swift {

/// Wasm-side incremental executor for the Swift REPL.
///
/// For each REPL cell, after IRGen produces an LLVM Module:
///   1. Emit a wasm32 object file (WebAssembly backend, PIC relocation).
///   2. Link into a SIDE_MODULE .wasm via wasm-ld (lld::lldMain).
///   3. dlopen() the side module, making its symbols globally available.
///   4. dlsym() + call the cell's wrapper function (SIL-mangled name).
class SwiftWasmExecutor {
public:
  static llvm::Expected<std::unique_ptr<SwiftWasmExecutor>> Create();

  ~SwiftWasmExecutor() = default;

  /// Emit, link, dlopen, and call \p FunctionName from \p Module.
  llvm::Error executeModule(llvm::Module *Module,
                            llvm::StringRef FunctionName);

private:
  SwiftWasmExecutor() = default;

  llvm::SmallString<256> TempDir;
  unsigned ModuleIndex = 0;
};

} // namespace swift

#endif // __EMSCRIPTEN__

#endif // SWIFT_IMMEDIATE_WASMEXECUTOR_H
