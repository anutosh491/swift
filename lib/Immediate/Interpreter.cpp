//===--- Interpreter.cpp - Swift incremental REPL interpreter -------------===//
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

#include "swift/Immediate/Interpreter.h"
#include "REPLTransforms.h"
#include "ImmediateImpl.h"
#include "swift/Immediate/Immediate.h"
#ifndef __EMSCRIPTEN__
#include "swift/Immediate/SwiftMaterializationUnit.h"
#else
#include "WasmExecutor.h"
#endif
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/Import.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/Frontend/Frontend.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/IDE/REPLCodeCompletion.h"
#include "swift/IDE/Utils.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILModule.h"
#include "swift/Subsystems.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <string>

using namespace swift;

/// Result of type-checking a single REPL input cell.
struct REPLInputResults {
  ModuleDecl *Module;
  SourceFile *InputFile;
  /// The synthesized __repl_N() wrapper function. Use SILDeclRef(WrapperFunc)
  /// to compute the Swift-mangled symbol name for JIT lookup.
  FuncDecl *WrapperFunc;
};

/// Type-check a single REPL input cell, creating a new ModuleDecl for it.
///
/// Each cell gets its own ModuleDecl, which implicitly imports the previous
/// module so that declarations are visible across cells.  User-written imports
/// (e.g. 'import Foundation') from the previous module are carried forward as
/// exported imports so they remain in scope in later cells.
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
  // it, but the user's own declarations -- structs, classes, lets, vars -- get
  // their access raised here).
  makeDeclarationsPublic(*REPLInputFile);

  return {REPLModule, REPLInputFile, WrapperFD};
}

// ── Interpreter implementation ────────────────────────────────────────────────

Interpreter::Interpreter(CompilerInstance &CI,
                         const ProcessCmdLine &CmdLine,
                         bool ParseStdlib)
    : CI(CI), MostRecentModule(CI.getMainModule()) {

  // Ensure Swift-in-Swift optimizer passes (e.g. LifetimeCompletion,
  // SILGenCleanup utilities) are registered exactly once per process.
  // In the standalone swift-frontend binary this is called from driver.cpp,
  // but when Interpreter is used as a library (xeus-swift kernel) no driver
  // startup code runs, so we must call it here.
  // Guard with swiftModulesInitialized() so we don't double-register when
  // driver.cpp has already called initializeSwiftModules() before us.
  {
    static std::once_flag s_initSwiftModulesOnce;
    std::call_once(s_initSwiftModulesOnce, []() {
      if (!swiftModulesInitialized())
        initializeSwiftModules();
    });
  }

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
  // yet, so we must skip this step -- and the warmup below -- entirely.
#ifndef __EMSCRIPTEN__
  if (!ParseStdlib) {
#if defined(_WIN32)
    // llvm::outs() << "[REPL] Loading Swift runtime (Windows)...\n";
    auto *stdlib =
        swift::immediate::loadSwiftRuntime(Ctx.SearchPathOpts.RuntimeLibraryPaths);
    if (!stdlib) {
      CI.getDiags().diagnose(SourceLoc(),
                             diag::error_immediate_mode_missing_stdlib);
      return;
    }
    // llvm::outs() << "[REPL] Swift runtime loaded.\n";
#else
    // On non-Windows, swift-frontend is often a Swift binary and already
    // has libswiftCore linked in.  Check for a known runtime symbol before
    // attempting a second dlopen.
    if (dlsym(RTLD_DEFAULT, "swift_retain")) {
      // llvm::outs() << "[REPL] Swift runtime already in process (linked).\n";
    } else {
      dlerror(); // clear stale error from the failed dlsym
      // llvm::outs() << "[REPL] Loading Swift runtime from: ";
      // for (const auto &p : Ctx.SearchPathOpts.RuntimeLibraryPaths)
      //   llvm::outs() << p << " ";
      // llvm::outs() << "\n";
      auto *stdlib = swift::immediate::loadSwiftRuntime(
          Ctx.SearchPathOpts.RuntimeLibraryPaths);
      if (!stdlib) {
        CI.getDiags().diagnose(SourceLoc(),
                               diag::error_immediate_mode_missing_stdlib);
        return;
      }
      // llvm::outs() << "[REPL] Swift runtime loaded via dlopen.\n";
    }
#endif
  }

  // ── Step 2: Autolink any libraries the main module depends on ────────────
  //
  // autolinkImportedModules walks IRGenOpts.LinkLibraries plus the set of
  // modules that the given ModuleDecl imports, and calls dlopen for each.
  // At REPL startup the main module is empty (no cells parsed yet), so this
  // primarily picks up command-line -l flags from IRGenOpts.LinkLibraries.
  // Per-cell imports (e.g. `import Foundation`) are handled in parseAndExecute
  // after each cell's type-checking.
  // llvm::outs() << "[REPL] Autolinking startup libraries...\n";
  if (swift::immediate::autolinkImportedModules(CI.getMainModule(), IRGenOpts)) {
    // llvm::errs() << "[REPL] error: autolinkImportedModules failed\n";
    return;
  }
  // llvm::outs() << "[REPL] Startup autolinking done.\n";

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
  // llvm::outs() << "[REPL] Creating ORC JIT session...\n";
  auto JITOrErr = SwiftJIT::Create(CI);
  if (auto Err = JITOrErr.takeError()) {
    logAllUnhandledErrors(std::move(Err), llvm::errs());
    return;
  }
  JIT = std::move(*JITOrErr);
  // llvm::outs() << "[REPL] ORC JIT session ready.\n";
#else
  // ── Wasm: create the side-module executor ────────────────────────────────
  // llvm::outs() << "[REPL] Creating Wasm executor...\n";
  auto ExecOrErr = SwiftWasmExecutor::Create();
  if (auto Err = ExecOrErr.takeError()) {
    logAllUnhandledErrors(std::move(Err), llvm::errs());
    return;
  }
  WasmExec = std::move(*ExecOrErr);
  // llvm::outs() << "[REPL] Wasm executor ready.\n";

  // === NEW: Explicitly preload Swift runtime SIDE_MODULEs ===
  // Belt-and-suspenders after Emscripten's loadDylibs has already loaded them
  // at module init time. dlopen on an already-loaded library is a no-op that
  // returns the existing handle. Using full VFS paths so this also works if
  // the explicit dlopen fires before loadDylibs has run a given library.
  // llvm::outs() << "[REPL] Preloading Swift runtime libraries...\n";

  const char* runtimeLibs[] = {
    "libswiftCore.so",
    "libswift_Builtin_float.so",
    "libswift_Concurrency.so",
    "libswiftDistributed.so",
    "libswiftEmscriptenLibc.so",
    "libswiftObservation.so",
    "libswiftRegexBuilder.so",
    "libswiftRuntimeUnittest.so",
    "libswiftStdlibUnittest.so",
    "libswiftSwiftOnoneSupport.so",
    "libswiftSwiftPrivate.so",
    "libswiftSwiftPrivateLibcExtras.so",
    "libswiftSwiftPrivateThreadExtras.so",
    "libswiftSynchronization.so",
    "libswift_RegexParser.so",
    "libswift_StringProcessing.so",
    "libswift_Volatile.so",
    nullptr
  };

  for (const char** p = runtimeLibs; *p; ++p) {
    std::string path = "/lib/swift/emscripten/wasm32/" + std::string(*p);
    if (dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL) == nullptr) {
      // llvm::errs() << "[REPL] Warning: Failed to dlopen " << *p
      //              << " - " << dlerror() << "\n";
    } else {
      // llvm::outs() << "[REPL] Loaded " << *p << "\n";
    }
  }
  // llvm::outs() << "[REPL] Runtime preloading done.\n";
#endif

  // ── Step 4: Stdlib type-checker warmup ───────────────────────────────────
  //
  // ParseStdlib == false is the normal case (we are NOT building the stdlib).
  // Parsing a trivial statement forces the Swift type-checker to lazily load
  // all stdlib AST nodes upfront, so the first real user input does not
  // incur an unexpected pause.  This is a type-check-only step; the warmup
  // cell is intentionally NOT compiled or executed.
  if (!ParseStdlib) {
    // llvm::outs() << "[REPL] Running stdlib type-checker warmup...\n";
    static const char WarmUpStmt[] = "Void()\n";
    auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(
        WarmUpStmt, "<REPL Initialization>");
    (void)typeCheckREPLInput(MostRecentModule, "__Warmup", std::move(Buffer));
    if (Ctx.hadError()) {
      // llvm::errs() << "[REPL] error: stdlib warmup failed\n";
      return;
    }
    // llvm::outs() << "[REPL] Stdlib warmup done.\n";
  }
}

Interpreter::~Interpreter() = default;

Interpreter::InputCompleteness
Interpreter::isInputComplete(llvm::StringRef Text) const {
  // Use the Swift parser to determine whether Text is syntactically complete.
  // isSourceInputComplete() creates a throwaway ParserUnit, runs the parser
  // on Text, and returns whether any bracket/brace/paren is still open.
  const auto &LangOpts = CI.getASTContext().LangOpts;
  auto Result =
      ide::isSourceInputComplete(Text, SourceFileKind::REPL, LangOpts);
  return {Result.IsComplete, Result.IndentPrefix, Result.IndentLevel};
}

Interpreter::CompletionResult
Interpreter::complete(llvm::StringRef Code) const {
  // Suppress diagnostics for the duration of this call so that partial/
  // erroneous input during a completion probe does not surface as errors.
  DiagnosticSuppression Suppression(CI.getASTContext().Diags);

  // Pass a source file from MostRecentModule — not CI.getMainModule() — so
  // that declarations from previously-executed cells are in scope.  Each cell
  // module imports the previous one (see typeCheckREPLInput), so
  // MostRecentModule has transitive access to all prior declarations.
  SourceFile *SF = nullptr;
  for (auto *F : MostRecentModule->getFiles()) {
    if (auto *Src = llvm::dyn_cast<SourceFile>(F)) {
      SF = Src;
      break;
    }
  }
  if (!SF)
    return {};

  swift::REPLCompletions Completions;
  Completions.populate(*SF, Code);

  CompletionResult Result;
  Result.Prefix = Completions.getPrefix().str();

  // Build fully-insertable completion strings: prefix typed by the user +
  // the suffix recommended by the engine.
  llvm::StringRef Prefix = Completions.getPrefix();
  for (auto &Cooked : Completions.getCookedResults())
    Result.Matches.push_back((Prefix + Cooked.InsertableString).str());

  return Result;
}

void Interpreter::setLastValueDisplayFunc(std::string funcName) {
  DisplayFunc = std::move(funcName);
}

bool Interpreter::loadLibrary(llvm::StringRef path) {
#if defined(_WIN32)
  if (LoadLibraryA(path.str().c_str()))
    return true;
  llvm::errs() << "Interpreter::loadLibrary: failed to load '" << path << "'\n";
  return false;
#else
  if (dlopen(path.str().c_str(), RTLD_LAZY | RTLD_GLOBAL))
    return true;
  llvm::errs() << "Interpreter::loadLibrary: " << dlerror() << "\n";
  return false;
#endif
}

Interpreter::REPLResult Interpreter::parseAndExecute(llvm::StringRef Line) {
  ASTContext &Ctx = CI.getASTContext();

  // Bail out if execution setup failed during construction.
#ifndef __EMSCRIPTEN__
  if (!JIT) {
    llvm::errs() << "REPL: JIT unavailable\n";
    return REPLResult::Fatal;
  }
#else
  if (!WasmExec) {
    llvm::errs() << "REPL: Wasm executor unavailable\n";
    return REPLResult::Fatal;
  }
#endif

  // Check for quit commands.
  llvm::StringRef Trimmed = Line.trim();
  if (Trimmed == ":quit" || Trimmed == ":exit" || Trimmed == ":q")
    return REPLResult::Fatal;

  // Reset error state from previous input.
  Ctx.Diags.resetHadAnyError();

  // Create a unique module name for this input.
  SmallString<16> Name;
  llvm::raw_svector_ostream(Name) << "__repl_" << InputNumber;

  // llvm::outs() << "[REPL] Step 1: type-checking input (cell " << InputNumber << ")...\n";
  // llvm::outs().flush();

  // Typecheck the original input.  After typeChecking,
  // wrapTopLevelCodeInFunction has already run and populated the wrapper body:
  //   - declarations (let/func/struct/…) → empty wrapper body
  //   - void-returning calls (print(x)) → wrapper body has a ()-typed Expr*
  //   - bare value expressions (1+1, x) → wrapper body has a non-Void Expr*
  // This lets us cheaply detect the auto-print case without a speculative
  // probe typecheck on every declaration the user enters.
  bool IsBareExpr = false;
  auto Buffer = llvm::MemoryBuffer::getMemBufferCopy(Line, Name);
  auto Result = typeCheckREPLInput(MostRecentModule, Name, std::move(Buffer));
  // llvm::outs() << "[REPL] Step 1: type-check done.\n";
  // llvm::outs().flush();

  if (Ctx.hadError()) {
    // Non-fatal compile error (type-check failure etc.).
    // Diagnostics have already been emitted to any registered consumers.
    // Flush them now so the Swift-style PrintingDiagnosticConsumer releases
    // any output queued in DiagnosticBridge (it normally waits for the next
    // error before printing, which causes delayed output in a REPL).
    Ctx.Diags.resetHadAnyError();
    Ctx.Diags.flushConsumers();
    return REPLResult::CompileError;
  }

  // If the wrapper body has a single non-Void expression, re-typecheck with
  // auto-print injected so the result is displayed to the user.
  //
  // If the embedder registered a last-value display function via
  // setLastValueDisplayFunc(name), call it instead of Swift.print.  The
  // function must provide two overloads — one constrained on the rich-display
  // protocol, one unconstrained fallback — so Swift resolves the correct one at
  // typecheck time (concrete type is always known in the wrapper).  If the
  // function is not yet in scope, DiagnosticSuppression discards the typecheck
  // error and the expression executes without output.
  if (hasSingleNonVoidBareExpr(Result.WrapperFunc)) {
    std::string WrappedInput;
    llvm::raw_string_ostream WOS(WrappedInput);
    WOS << "let __repl_r" << ResultIdx << " = (" << Line << ")\n";
    if (!DisplayFunc.empty()) {
      // Call the two-overload display function.  Swift resolves the correct
      // overload — constrained (rich display) or unconstrained (Swift.print
      // fallback) — at typecheck time.  No runtime conformance lookup, no
      // as? cast, no extra pass.
      WOS << DisplayFunc << "(__repl_r" << ResultIdx
          << ", \"$R" << ResultIdx << "\")";
    } else {
      WOS << "Swift.print(\"$R" << ResultIdx
          << ": \\(Swift.type(of: __repl_r" << ResultIdx << ")) = "
          << "\\(String(reflecting: __repl_r" << ResultIdx << "))\")";
    }
    DiagnosticSuppression Suppress(Ctx.Diags);
    Ctx.Diags.resetHadAnyError();
    auto WrappedBuf =
        llvm::MemoryBuffer::getMemBufferCopy(WrappedInput, Name);
    auto Wrapped =
        typeCheckREPLInput(MostRecentModule, Name, std::move(WrappedBuf));
    if (!Ctx.hadError()) {
      Result = Wrapped;
      IsBareExpr = true;
    }
    Ctx.Diags.resetHadAnyError();
  }

  // Autolink any libraries introduced by this cell's imports (e.g.
  // `import Foundation` in cell N triggers dlopen of libswiftFoundation so
  // the JIT can resolve Foundation symbols when compiling cell N+1).
  // On Emscripten, the core Swift runtime .so files (libswiftCore.so etc.) are
  // already explicitly dlopen'd in the Interpreter constructor. Calling
  // autolinkImportedModules here would additionally require
  // SearchPathOpts.RuntimeLibraryPaths to point at the VFS path
  // /lib/swift/emscripten/wasm32 — which is not yet configured. Skip it for
  // now; cell symbols are resolved at dlopen time from the already-loaded libs.
  const auto &IRGenOpts = CI.getInvocation().getIRGenOptions();
#ifndef __EMSCRIPTEN__
  swift::immediate::autolinkImportedModules(Result.Module, IRGenOpts);
#endif

  // Compute the linker-level symbol name for __repl_N() now, while we still
  // hold the FuncDecl pointer and before the SILModule consumes it.
  std::string SILMangledName = SILDeclRef(Result.WrapperFunc).mangle();
  // llvm::outs() << "[REPL] Step 2: wrapper symbol = " << SILMangledName << "\n";
  // llvm::outs().flush();

  // ── Step 5: SIL lowering ──────────────────────────────────────────────
  // llvm::outs() << "[REPL] Step 3: SIL lowering...\n";
  // llvm::outs() << "[REPL] AST dump:\n";
  // Result.Module->dump(llvm::outs());
  // llvm::outs() << "\n[REPL] AST dump done.\n";
  // llvm::outs().flush();
  auto SILMod = performASTLowering(*Result.InputFile,
                                   CI.getSILTypes(),
                                   CI.getSILOptions());
  // llvm::outs() << "[REPL] Step 3: SIL lowering done.\n";
  // llvm::outs() << "[REPL] SIL dump:\n";
  // SILMod->print(llvm::outs());
  // llvm::outs() << "\n[REPL] SIL dump done.\n";
  // llvm::outs().flush();

  if (runSILDiagnosticPasses(*SILMod) || Ctx.hadError()) {
    Ctx.Diags.resetHadAnyError();
    Ctx.Diags.flushConsumers();
    return REPLResult::CompileError;
  }
  // llvm::outs() << "[REPL] Step 4: SIL diagnostic passes done.\n";
  // llvm::outs().flush();

  // Run SIL performance optimizations when the user asked for them (-O / -Osize).
  // Must run BEFORE runSILLoweringPasses: the perf pipeline (PerformanceSILLinker,
  // inlining, generic specialisation, ARC elimination, etc.) operates on
  // canonical SIL and tries to deserialize function bodies from imported modules.
  // runSILLoweringPasses advances the module stage to Lowered; after that point
  // SILDeserializer refuses to load any more SIL into the module
  // ("cannot deserialize into a module that has entered Lowered stage").
  //
  // Correct order mirrors performCompileStepsPostSILGen in the real compiler:
  //   runSILDiagnosticPasses  → runSILOptimizationPasses → runSILLoweringPasses
  //
  // LLDB's SwiftExpressionParser hardcodes NoOptimization unconditionally;
  // this block is what gives our ORC JIT REPL a concrete performance advantage
  // over the LLDB REPL when -O is requested.
#ifndef __EMSCRIPTEN__
  // Performance SIL linker uses LinkAll mode and aggressively deserializes
  // all reachable [serialized] function bodies. On wasm32, deserializing generic
  // stdlib functions crashes in GenericEnvironment::getForwardingSubstitutionMap().
  // Skip optimization passes entirely on wasm — the cell runs at -Onone.
  if (CI.getSILOptions().shouldOptimize()) {
    // The full performance pipeline contains a SerializeSILPass that asserts a
    // serialization action is set — that action exists only in whole-module
    // compilation, not in JIT mode.  Install a no-op callback so the pass
    // runs (and safely does nothing) instead of crashing.
    SILMod->setSerializeSILAction([](){});
    runSILOptimizationPasses(*SILMod);
  }
#endif

  if (Ctx.hadError()) {
    Ctx.Diags.resetHadAnyError();
    Ctx.Diags.flushConsumers();
    return REPLResult::CompileError;
  }

  runSILLoweringPasses(*SILMod);
  // llvm::outs() << "[REPL] Step 5: SIL lowering passes done.\n";
  // llvm::outs().flush();

  if (Ctx.hadError()) {
    Ctx.Diags.resetHadAnyError();
    Ctx.Diags.flushConsumers();
    return REPLResult::CompileError;
  }

  // ── Step 6: IRGen ─────────────────────────────────────────────────────
  // llvm::outs() << "[REPL] Step 6: IRGen...\n";
  // llvm::outs().flush();
  const auto &TBDOpts = CI.getInvocation().getTBDGenOptions();
  const auto PSPs = CI.getPrimarySpecificPathsForAtMostOnePrimary();

  auto GenModule = performIRGeneration(
      Result.Module, IRGenOpts, TBDOpts, std::move(SILMod),
      Name.str(), PSPs, /*CAS=*/nullptr,
      /*parallelOutputFilenames=*/ArrayRef<std::string>(),
      /*parallelIROutputFilenames=*/ArrayRef<std::string>());

  if (Ctx.hadError()) {
    Ctx.Diags.resetHadAnyError();
    Ctx.Diags.flushConsumers();
    return REPLResult::CompileError;
  }
  assert(GenModule && "IR generation succeeded without emitting a module?");
  // llvm::outs() << "[REPL] Step 6: IRGen done.\n";
  // llvm::outs() << "[REPL] LLVM IR dump:\n";
  // GenModule.getModule()->print(llvm::outs(), nullptr);
  // llvm::outs() << "\n[REPL] LLVM IR dump done.\n";
  // llvm::outs().flush();

  auto *LLVMModule = GenModule.getModule();
  performLLVMOptimizations(IRGenOpts, Ctx.Diags, /*diagMutex=*/nullptr,
                           LLVMModule, GenModule.getTargetMachine(),
                           Ctx.SourceMgr.getFileSystem(),
                           /*out=*/nullptr);

  if (Ctx.hadError()) {
    Ctx.Diags.resetHadAnyError();
    Ctx.Diags.flushConsumers();
    return REPLResult::CompileError;
  }

  // ── Step 7: Execute ───────────────────────────────────────────────────
#ifndef __EMSCRIPTEN__
  auto TSM = std::move(GenModule).intoThreadSafeContext();
  if (auto Err = JIT->addIRModule(std::move(TSM))) {
    logAllUnhandledErrors(std::move(Err), llvm::errs());
    return REPLResult::CompileError;
  }

  auto Sym = JIT->lookup(SILMangledName);
  if (!Sym) {
    logAllUnhandledErrors(Sym.takeError(), llvm::errs());
    return REPLResult::CompileError;
  }

  auto *Fn = Sym->toPtr<void (*)()>();
  Fn();
#else
  // Wasm path: emit object → wasm-ld side module → dlopen → call.
  // llvm::outs() << "[REPL] Step 7: executing via WasmExecutor...\n";
  // llvm::outs().flush();
  if (auto Err = WasmExec->executeModule(LLVMModule, SILMangledName)) {
    logAllUnhandledErrors(std::move(Err), llvm::errs());
    return REPLResult::CompileError;
  }
  // llvm::outs() << "[REPL] Step 7: execution done.\n";
  // llvm::outs().flush();
#endif

  MostRecentModule = Result.Module;

  if (IsBareExpr)
    ++ResultIdx;

  ++InputNumber;
  Ctx.Diags.flushConsumers();
  return REPLResult::Success;
}
