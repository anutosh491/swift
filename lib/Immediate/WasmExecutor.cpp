//===--- WasmExecutor.cpp - Wasm incremental executor for Swift REPL ------===//
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

#ifdef __EMSCRIPTEN__

#include "WasmExecutor.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include <cstdint>
#include <dlfcn.h>
#include <string>
#include <vector>

// Forward-declare lld symbols to avoid pulling in lld public headers, which
// require ORC components that are not built in the wasm32 LLVM.
namespace lld {
enum Flavor { Invalid, Gnu, MinGW, WinLink, Darwin, Wasm };
using Driver = bool (*)(llvm::ArrayRef<const char *>, llvm::raw_ostream &,
                        llvm::raw_ostream &, bool, bool);
struct DriverDef {
  Flavor f;
  Driver d;
};
struct Result {
  int retCode;
  bool canRunAgain;
};
Result lldMain(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
               llvm::raw_ostream &stderrOS,
               llvm::ArrayRef<DriverDef> drivers);
namespace wasm {
bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
          llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);
} // namespace wasm
} // namespace lld

namespace swift {

llvm::Expected<std::unique_ptr<SwiftWasmExecutor>>
SwiftWasmExecutor::Create() {
  auto Exec = std::unique_ptr<SwiftWasmExecutor>(new SwiftWasmExecutor());
  if (auto EC =
          llvm::sys::fs::createUniqueDirectory("swift-wasm-exec-", Exec->TempDir))
    return llvm::make_error<llvm::StringError>(
        "Failed to create temp dir for wasm executor: " + EC.message(),
        llvm::inconvertibleErrorCode());
  return std::move(Exec);
}

llvm::Error SwiftWasmExecutor::executeModule(llvm::Module *Module,
                                             llvm::StringRef FunctionName) {
  // llvm::outs() << "[WasmExec] executeModule: symbol=" << FunctionName << "\n";
  // llvm::outs().flush();

  std::string ErrorString;
  const llvm::Target *Target = llvm::TargetRegistry::lookupTarget(
      Module->getTargetTriple(), ErrorString);
  if (!Target)
    return llvm::make_error<llvm::StringError>(
        "Failed to find wasm target: " + ErrorString,
        llvm::inconvertibleErrorCode());
  // llvm::outs() << "[WasmExec] target triple: " << Module->getTargetTriple().str() << "\n";
  // llvm::outs().flush();

  llvm::TargetOptions TO;
  std::unique_ptr<llvm::TargetMachine> TM(Target->createTargetMachine(
      Module->getTargetTriple(), /*CPU=*/"", /*Features=*/"", TO,
      llvm::Reloc::Model::PIC_));
  Module->setDataLayout(TM->createDataLayout());
  // llvm::outs() << "[WasmExec] TargetMachine created.\n";
  // llvm::outs().flush();

  // Derive stable per-module file names inside the temp directory.
  llvm::SmallString<256> ObjFile(TempDir);
  llvm::sys::path::append(
      ObjFile, llvm::Twine("__repl_") + llvm::Twine(ModuleIndex) + ".o");
  llvm::SmallString<256> WasmFile(TempDir);
  llvm::sys::path::append(
      WasmFile, llvm::Twine("__repl_") + llvm::Twine(ModuleIndex) + ".wasm");
  // llvm::outs() << "[WasmExec] obj=" << ObjFile << " wasm=" << WasmFile << "\n";
  // llvm::outs().flush();

  // Swift IRGen embeds library names (e.g. "swiftCore") in
  // !llvm.dependent-libraries metadata. wasm-ld would turn those into NEEDED
  // entries in the per-cell side module, causing dlopen to redundantly re-load
  // libswiftCore.so. The Swift runtime .so files are already explicitly
  // dlopen'd (RTLD_GLOBAL) in the Interpreter constructor before any cell runs,
  // so all Swift runtime symbols are visible to the dynamic linker at dlopen
  // time via the already-loaded side modules. Strip the metadata to prevent
  // spurious NEEDED entries in the per-cell .wasm.
  if (auto *DepLibs = Module->getNamedMetadata("llvm.dependent-libraries"))
    DepLibs->eraseFromParent();

  // --- Step 1: emit wasm32 object file ---
  // llvm::outs() << "[WasmExec] Step 1: emitting object file...\n";
  // llvm::outs().flush();
  std::error_code EC;
  llvm::raw_fd_ostream ObjOut(ObjFile, EC);
  if (EC)
    return llvm::errorCodeToError(EC);

  llvm::legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, ObjOut, /*DwoOut=*/nullptr,
                              llvm::CodeGenFileType::ObjectFile))
    return llvm::make_error<llvm::StringError>(
        "Wasm backend cannot emit object file.",
        llvm::inconvertibleErrorCode());

  if (!PM.run(*Module))
    return llvm::make_error<llvm::StringError>(
        "Failed to emit wasm object file.", llvm::inconvertibleErrorCode());

  ObjOut.close();
  // llvm::outs() << "[WasmExec] Step 1: object file emitted.\n";
  // llvm::outs().flush();

  // --- Step 2: link as a SIDE_MODULE with wasm-ld ---
  // llvm::outs() << "[WasmExec] Step 2: linking with wasm-ld...\n";
  // llvm::outs().flush();
  std::vector<const char *> LinkerArgs = {"wasm-ld",
                                          "-shared",
                                          "--import-memory",
                                          "--experimental-pic",
                                          "--stack-first",
                                          "--allow-undefined",
                                          ObjFile.c_str(),
                                          "-o",
                                          WasmFile.c_str()};

  const lld::DriverDef WasmDriver = {lld::Flavor::Wasm, &lld::wasm::link};
  lld::Result LLDResult =
      lld::lldMain(LinkerArgs, llvm::outs(), llvm::errs(), {WasmDriver});

  if (LLDResult.retCode)
    return llvm::make_error<llvm::StringError>(
        "wasm-ld failed for incremental module.",
        llvm::inconvertibleErrorCode());
  // llvm::outs() << "[WasmExec] Step 2: wasm-ld done.\n";
  // llvm::outs().flush();

  // --- Step 3: dlopen the side module ---
  // llvm::outs() << "[WasmExec] Step 3: dlopen " << WasmFile << "...\n";
  // llvm::outs().flush();
  void *Handle = dlopen(WasmFile.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!Handle) {
    llvm::errs() << "Could not load dynamic lib: " << WasmFile << "\n";
    llvm::errs() << dlerror() << '\n';
    return llvm::make_error<llvm::StringError>(
        "dlopen failed for incremental wasm module.",
        llvm::inconvertibleErrorCode());
  }
  // llvm::outs() << "[WasmExec] Step 3: dlopen succeeded.\n";
  // NOTE: Do NOT manually call __wasm_call_ctors here.
  // Emscripten's JS dynamic linker (loadDynamicLibrary) already calls it
  // as part of the dlopen implementation. Calling it again would double-init
  // GOT.func table slots, corrupting function pointer entries.
  // llvm::outs().flush();

  // --- Step 4: resolve and call the wrapper function ---
  // llvm::outs() << "[WasmExec] Step 4: dlsym '" << FunctionName << "'...\n";
  // llvm::outs().flush();
  void *Sym = dlsym(Handle, FunctionName.str().c_str());
  if (!Sym) {
    llvm::errs() << dlerror() << '\n';
    return llvm::make_error<llvm::StringError>(
        "dlsym failed for symbol: " + FunctionName.str(),
        llvm::inconvertibleErrorCode());
  }
  // llvm::outs() << "[WasmExec] Step 4: calling wrapper function...\n";
  // llvm::outs().flush();

  reinterpret_cast<void (*)(int32_t, int32_t)>(Sym)(0, 0);
  // llvm::outs() << "[WasmExec] Step 4: wrapper returned.\n";
  // llvm::outs().flush();

  ++ModuleIndex;
  return llvm::Error::success();
}

} // namespace swift

#endif // __EMSCRIPTEN__
