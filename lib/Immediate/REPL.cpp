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
#include "swift/Immediate/Immediate.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Import.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Subsystems.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <string>

using namespace swift;

namespace {

/// Result of type-checking a single REPL input.
struct REPLInputResults {
  ModuleDecl *Module;
  SourceFile *InputFile;
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

  // Carry over the non-exported imports from the previous module
  // (e.g. user-written 'import Foundation') so they remain visible.
  SmallVector<ImportedModule, 8> imports;
  MostRecentModule->getImportedModules(
      imports, ModuleDecl::ImportFilterKind::InternalOrBelow);
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
  wrapTopLevelCodeInFunction(*REPLInputFile, Name);

  return {REPLModule, REPLInputFile};
}

/// The compiler and execution environment for the REPL.
class REPLEnvironment {
  CompilerInstance &CI;
  ModuleDecl *MostRecentModule;
  unsigned InputNumber = 1;

public:
  REPLEnvironment(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                  bool ParseStdlib)
      : CI(CI), MostRecentModule(CI.getMainModule()) {

    ASTContext &Ctx = CI.getASTContext();

    if (!ParseStdlib) {
      // Force standard library to be loaded immediately. This forces any
      // errors to appear upfront, and helps eliminate some nasty lag after the
      // first statement is typed into the REPL.
      static const char WarmUpStmt[] = "Void()\n";

      auto Buffer =
          llvm::MemoryBuffer::getMemBufferCopy(WarmUpStmt,
                                               "<REPL Initialization>");
      (void)typeCheckREPLInput(MostRecentModule, "__Warmup", std::move(Buffer));

      if (Ctx.hadError())
        return;
    }
  }

  unsigned getInputNumber() const { return InputNumber; }

  /// Execute one line of REPL input. Returns true to continue, false to quit.
  bool handleInput(llvm::StringRef Line) {
    ASTContext &Ctx = CI.getASTContext();

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

    // Dump the type-checked AST.
    Result.InputFile->dump(llvm::outs());

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

    // Dump the fully-lowered SIL — this is what goes into IRGen, and the
    // right point to compare against `swiftc -emit-sil -parse-as-library`.
    SILMod->print(llvm::outs(), Result.Module);

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