//===--- REPL.cpp - the integrated REPL -----------------------------------===//
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
#include "ImmediateImpl.h"
#include "swift/Immediate/Immediate.h"
#include "swift/Immediate/SwiftMaterializationUnit.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/Import.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/Frontend/Frontend.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/Subsystems.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <iostream>
#include <string>

using namespace swift;

namespace {

/// Result of type-checking a single REPL input.
struct REPLInputResults {
  ModuleDecl *Module;
  SourceFile *InputFile;
  /// The synthesized __repl_N() wrapper function. Use SILDeclRef(WrapperFunc)
  /// to compute the Swift-mangled symbol name for JIT lookup.
  FuncDecl *WrapperFunc;
};

/// Type-check a single REPL input line, creating a new module for it.
///
/// Each input gets its own ModuleDecl, which implicitly imports the previous
/// module so that declarations are visible across inputs. Private imports
/// from the previous module are carried forward so that e.g. 'import Foundation'
/// in line 1 is visible in line 2.
static REPLInputResults
typeCheckREPLInput(ModuleDecl *MostRecentModule, StringRef Name,
                   std::unique_ptr<llvm::MemoryBuffer> Buffer) {
  assert(MostRecentModule);
  ASTContext &Ctx = MostRecentModule->getASTContext();

  // Build implicit imports: stdlib + the previous REPL module + its imports.
  ImplicitImportInfo importInfo;
  importInfo.StdlibKind = ImplicitStdlibKind::Stdlib;

  // Import the previous REPL module.
  importInfo.AdditionalImports.emplace_back(
      AttributedImport<ImportedModule>(ImportedModule(MostRecentModule)));

  // TODO: Once we want 'internal' declarations to be visible across cells
  // without requiring 'public', switch to a @testable import instead:
  //   importInfo.AdditionalImports.emplace_back(AttributedImport<ImportedModule>(
  //       ImportedModule(MostRecentModule), SourceLoc(),
  //       ImportOptions({ImportFlags::Testable})));
  // and call REPLModule->setTestingEnabled(true) after module creation.

  // Carry over all locally-written imports from the previous module
  // (e.g. user-written 'import Foundation') so they remain visible.
  // We use getImportFilterLocal() to capture every import kind the user
  // could write: @_exported, regular public (Default), @_implementationOnly,
  // package, internal/fileprivate/private, and @_spiOnly.  In particular
  // 'import Foundation' is a Default (public) import and would be silently
  // dropped if we only queried InternalOrBelow.
  SmallVector<ImportedModule, 8> imports;
  MostRecentModule->getImportedModules(
      imports, ModuleDecl::getImportFilterLocal());
  for (auto &import : imports) {
    importInfo.AdditionalImports.emplace_back(AttributedImport<ImportedModule>(
        import, SourceLoc(), ImportOptions({ImportFlags::Exported})));
  }

  auto BufferID = Ctx.SourceMgr.addNewSourceBuffer(std::move(Buffer));

  // Create a new module for this REPL input, with the source file
  // created inside the PopulateFilesFn lambda.
  SourceFile *REPLInputFile = nullptr;
  auto *REPLModule = ModuleDecl::create(
      Ctx.getIdentifier(Name), Ctx, importInfo,
      [&](ModuleDecl *M, llvm::function_ref<void(FileUnit *)> addFile) {
        REPLInputFile = new (Ctx) SourceFile(
            *M, SourceFileKind::REPL, BufferID,
            SourceFile::ParsingFlags::DisableDelayedBodies);
        addFile(REPLInputFile);
      });

  assert(REPLInputFile && "PopulateFilesFn should have created the SourceFile");

  // Run the frontend pipeline: import resolution, extension binding,
  // and type checking.
  performImportResolution(*REPLInputFile);
  bindExtensions(*REPLModule);
  performTypeChecking(*REPLInputFile);

  // AST transform: collect any top-level executable statements into a
  // single __repl_N() function.  Pure-declaration inputs get an empty stub.
  FuncDecl *WrapperFD = wrapTopLevelCodeInFunction(*REPLInputFile, Name);

  // AST transform: promote every declaration in this cell to public/open so
  // the JIT can resolve them and later cells can reference them by name.
  // Must run after wrapTopLevelCodeInFunction() so the synthesised wrapper
  // function is also walked (it is already Public, so the walk is a no-op on
  // it, but the user's own declarations — structs, classes, lets, vars — get
  // their access raised here).
  makeDeclarationsPublic(*REPLInputFile);

  return {REPLModule, REPLInputFile, WrapperFD};
}

/// The compiler and execution environment for the REPL.
class REPLEnvironment {
  CompilerInstance &CI;
  ModuleDecl *MostRecentModule;
  unsigned InputNumber = 1;
  /// Long-lived JIT session.  All REPL cells share the same JITDylib so that
  /// declarations from earlier cells are visible in later ones.
  std::unique_ptr<SwiftJIT> JIT;

public:
  REPLEnvironment(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                  bool ParseStdlib)
      : CI(CI), MostRecentModule(CI.getMainModule()) {

    ASTContext &Ctx = CI.getASTContext();
    const auto &IRGenOpts = CI.getInvocation().getIRGenOptions();

    // ── Step 1: Load libswiftCore into the host process ──────────────────────
    //
    // This MUST happen before SwiftJIT::Create, because the JIT installs a
    // DynamicLibrarySearchGenerator that forwards unresolved symbol lookups to
    // the host process's dynamic linker.  Every JIT'd cell that calls into the
    // Swift runtime (swift_retain, swift_release, swift_once, _swift_stdlib_*,
    // print(), etc.) resolves its symbols through that generator.  If
    // libswiftCore is not in the process at JIT-link time, all those calls will
    // produce "symbol not found" errors at runtime.
    //
    // ParseStdlib == true means we are building the stdlib itself (the
    // -parse-stdlib frontend flag).  In that case libswiftCore does not exist
    // yet, so we must skip this step – and the warmup below – entirely.
    if (!ParseStdlib) {
#if defined(_WIN32)
      llvm::outs() << "[REPL] Loading Swift runtime (Windows)...\n";
      auto *stdlib =
          swift::immediate::loadSwiftRuntime(Ctx.SearchPathOpts.RuntimeLibraryPaths);
      if (!stdlib) {
        CI.getDiags().diagnose(SourceLoc(),
                               diag::error_immediate_mode_missing_stdlib);
        return;
      }
      llvm::outs() << "[REPL] Swift runtime loaded.\n";
#else
      // On non-Windows, swift-frontend is often a Swift binary and already
      // has libswiftCore linked in.  Check for a known runtime symbol before
      // attempting a second dlopen.
      if (dlsym(RTLD_DEFAULT, "swift_retain")) {
        llvm::outs() << "[REPL] Swift runtime already in process (linked).\n";
      } else {
        dlerror(); // clear stale error from the failed dlsym
        llvm::outs() << "[REPL] Loading Swift runtime from: ";
        for (const auto &p : Ctx.SearchPathOpts.RuntimeLibraryPaths)
          llvm::outs() << p << " ";
        llvm::outs() << "\n";
        auto *stdlib = swift::immediate::loadSwiftRuntime(
            Ctx.SearchPathOpts.RuntimeLibraryPaths);
        if (!stdlib) {
          CI.getDiags().diagnose(SourceLoc(),
                                 diag::error_immediate_mode_missing_stdlib);
          return;
        }
        llvm::outs() << "[REPL] Swift runtime loaded via dlopen.\n";
      }
#endif
    }

    // ── Step 2: Autolink any libraries the main module depends on ────────────
    //
    // autolinkImportedModules walks IRGenOpts.LinkLibraries plus the set of
    // modules that the given ModuleDecl imports, and calls dlopen for each.
    // At REPL startup the main module is empty (no cells parsed yet), so this
    // primarily picks up command-line -l flags from IRGenOpts.LinkLibraries.
    // Per-cell imports (e.g. `import Foundation`) are handled in handleInput
    // after each cell's type-checking.
    llvm::outs() << "[REPL] Autolinking startup libraries...\n";
    if (swift::immediate::autolinkImportedModules(CI.getMainModule(),
                                                  IRGenOpts)) {
      llvm::errs() << "[REPL] error: autolinkImportedModules failed\n";
      return;
    }
    llvm::outs() << "[REPL] Startup autolinking done.\n";

    // ── Step 3: Create the JIT session ───────────────────────────────────────
    //
    // SwiftJIT wraps an ORC LLJIT with:
    //   - JITTargetMachineBuilder configured from IRGenOpts (target triple,
    //     CPU, features, relocation model PIC)
    //   - DynamicLibrarySearchGenerator for the host process, so any symbol
    //     dlopen'd in Steps 1-2 is visible to JIT'd code
    //   - EPCIndirectionUtils + LazyCallThroughManager for future lazy
    //     compilation support (not used in eager REPL mode)
    // The session lives for the entire REPL lifetime; all cells share the
    // same main JITDylib, so declarations from cell N are visible in cell N+1.
    llvm::outs() << "[REPL] Creating ORC JIT session...\n";
    auto JITOrErr = SwiftJIT::Create(CI);
    if (auto Err = JITOrErr.takeError()) {
      logAllUnhandledErrors(std::move(Err), llvm::errs());
      return;
    }
    JIT = std::move(*JITOrErr);
    llvm::outs() << "[REPL] ORC JIT session ready.\n";

    // ── Step 4: Stdlib type-checker warmup ───────────────────────────────────
    //
    // ParseStdlib == false is the normal case (we are NOT building the stdlib).
    // Parsing a trivial statement forces the Swift type-checker to lazily load
    // all stdlib AST nodes upfront, so the first real user input does not
    // incur an unexpected pause.  This is a type-check-only step; the warmup
    // cell is intentionally NOT JIT-compiled or executed.
    if (!ParseStdlib) {
      llvm::outs() << "[REPL] Running stdlib type-checker warmup...\n";
      static const char WarmUpStmt[] = "Void()\n";
      auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(
          WarmUpStmt, "<REPL Initialization>");
      (void)typeCheckREPLInput(MostRecentModule, "__Warmup",
                               std::move(Buffer));
      if (Ctx.hadError()) {
        llvm::errs() << "[REPL] error: stdlib warmup failed\n";
        return;
      }
      llvm::outs() << "[REPL] Stdlib warmup done.\n";
    }
  }

  unsigned getInputNumber() const { return InputNumber; }

  /// Execute one line of REPL input. Returns true to continue, false to quit.
  bool handleInput(llvm::StringRef Line) {
    ASTContext &Ctx = CI.getASTContext();

    // Bail out if JIT setup failed during construction.
    if (!JIT) {
      llvm::errs() << "REPL: JIT unavailable\n";
      return false;
    }

    // Check for quit commands.
    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed == ":quit" || Trimmed == ":exit" || Trimmed == ":q")
      return false;

    // Reset error state from previous input.
    Ctx.Diags.resetHadAnyError();

    // Create a unique module name for this input.
    SmallString<16> Name;
    llvm::raw_svector_ostream(Name) << "__repl_" << InputNumber;

    // Create a memory buffer for the input.
    auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(Line, Name);

    // Parse and type-check.
    auto Result = typeCheckREPLInput(MostRecentModule, Name,
                                     std::move(Buffer));

    if (Ctx.hadError()) {
      // Non-fatal error: reset and let the user try again.
      Ctx.Diags.resetHadAnyError();
      return true;
    }

    // Autolink any libraries introduced by this cell's imports (e.g.
    // `import Foundation` in cell N triggers dlopen of libswiftFoundation so
    // the JIT can resolve Foundation symbols when compiling cell N+1).
    const auto &IRGenOpts = CI.getInvocation().getIRGenOptions();
    swift::immediate::autolinkImportedModules(Result.Module, IRGenOpts);

    // Dump the type-checked AST.
    // Result.InputFile->dump(llvm::outs());

    // Compute the linker-level symbol name for __repl_N() now, while we still
    // hold the FuncDecl pointer and before the SILModule consumes it.
    // SILDeclRef::mangle() uses ASTMangler and doesn't need the SIL module.
    std::string SILMangledName = SILDeclRef(Result.WrapperFunc).mangle();
    // llvm::outs() << "-- JIT entry point: " << SILMangledName << "\n";

    // ── Step 5: SIL lowering ──────────────────────────────────────────────
    // Lower the transformed AST to SIL.  We use the CompilerInstance's shared
    // TypeConverter and SILOptions so that type-lowering state is consistent
    // across REPL cells.
    auto SILMod = performASTLowering(*Result.InputFile,
                                     CI.getSILTypes(),
                                     CI.getSILOptions());

    // Run the mandatory SIL diagnostic passes (definite initialisation,
    // memory lifetime, etc.).  Returns true if any error was emitted.
    if (runSILDiagnosticPasses(*SILMod) || Ctx.hadError()) {
      Ctx.Diags.resetHadAnyError();
      return true;
    }

    // Lower ownership and prepare for IRGen.
    runSILLoweringPasses(*SILMod);

    if (Ctx.hadError()) {
      Ctx.Diags.resetHadAnyError();
      return true;
    }

    // Dump the fully-lowered SIL.
    // SILMod->print(llvm::outs(), Result.Module);

    // ── Step 6: IRGen ─────────────────────────────────────────────────────
    // Lower the SIL module to LLVM IR.  Mirrors generateModule() inside
    // SwiftMaterializationUnit.cpp; we replicate it here so we control the
    // module operand (Result.Module, not CI.getMainModule()).
    const auto &TBDOpts = CI.getInvocation().getTBDGenOptions();
    const auto PSPs = CI.getPrimarySpecificPathsForAtMostOnePrimary();

    auto GenModule = performIRGeneration(
        Result.Module, IRGenOpts, TBDOpts, std::move(SILMod),
        Name.str(), PSPs, /*CAS=*/nullptr,
        /*parallelOutputFilenames=*/ArrayRef<std::string>(),
        /*parallelIROutputFilenames=*/ArrayRef<std::string>());

    if (Ctx.hadError()) {
      Ctx.Diags.resetHadAnyError();
      return true;
    }
    assert(GenModule && "IR generation succeeded without emitting a module?");

    // Run the Swift LLVM optimization pipeline (inlining, peephole, etc.)
    // in-memory.  We use performLLVMOptimizations rather than performLLVM
    // because the latter also runs the backend codegen, which lowers the module
    // to machine code and leaves it in a state the JIT cannot consume.  The
    // JIT's IRCompileLayer handles the IR → machine-code step itself.
    // Pass out=nullptr so no IR is written to disk.
    auto *LLVMModule = GenModule.getModule();
    performLLVMOptimizations(IRGenOpts, Ctx.Diags, /*diagMutex=*/nullptr,
                             LLVMModule, GenModule.getTargetMachine(),
                             /*out=*/nullptr);

    if (Ctx.hadError()) {
      Ctx.Diags.resetHadAnyError();
      return true;
    }

    // Dump the post-optimisation LLVM IR — this is exactly what the JIT will
    // compile to machine code; nothing transforms the module after this point.
    // llvm::outs() << "-- LLVM IR (post-optimisation, pre-JIT) --\n";
    // LLVMModule->print(llvm::outs(), /*AssemblyAnnotationWriter=*/nullptr);

    // ── Step 7: JIT add + execute ─────────────────────────────────────────
    // Register the LLVM IR module with the persistent JIT session.  All
    // public symbols become discoverable by subsequent cells via the shared
    // main JITDylib.
    auto TSM = std::move(GenModule).intoThreadSafeContext();
    if (auto Err = JIT->addIRModule(std::move(TSM))) {
      logAllUnhandledErrors(std::move(Err), llvm::errs());
      return true;
    }

    // Look up the cell-wrapper function by its Swift-mangled name.
    // LLJIT::lookup() applies the platform prefix (_) internally, so we pass
    // the bare Swift mangled name (e.g. "$s8__repl_1AAyyF").
    auto Sym = JIT->lookup(SILMangledName);
    if (!Sym) {
      logAllUnhandledErrors(Sym.takeError(), llvm::errs());
      return true;
    }

    // Execute the wrapper — this runs the user's statements for this cell.
    auto *Fn = Sym->toPtr<void (*)()>();
    Fn();

    // Success — this module becomes the most recent for import chaining.
    MostRecentModule = Result.Module;

    ++InputNumber;
    return true;
  }
};

} // end anonymous namespace

void swift::runREPL(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                    bool ParseStdlib) {
  REPLEnvironment Env(CI, CmdLine, ParseStdlib);
  if (CI.getASTContext().hadError())
    return;

  llvm::outs() << "Welcome to Swift REPL.\n"
               << "Type ':quit' to exit.\n\n";

  std::string Line;
  while (true) {
    llvm::outs() << Env.getInputNumber() << "> ";

    if (!std::getline(std::cin, Line))
      break; // EOF (Ctrl+D)

    // Skip empty lines.
    if (llvm::StringRef(Line).trim().empty())
      continue;

    if (!Env.handleInput(Line))
      break; // User requested quit.
  }

  llvm::outs() << "\n";
}