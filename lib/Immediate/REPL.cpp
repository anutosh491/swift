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

#include "swift/Immediate/Immediate.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/Frontend/Frontend.h"
#include "llvm/Support/raw_ostream.h"

#include <iostream>
#include <string>

using namespace swift;

namespace {

/// The compiler and execution environment for the REPL.
class REPLEnvironment {
  CompilerInstance &CI;
  ModuleDecl *MostRecentModule;
  unsigned InputNumber = 1;

public:
  REPLEnvironment(CompilerInstance &CI, const ProcessCmdLine &CmdLine,
                  bool ParseStdlib)
      : CI(CI), MostRecentModule(CI.getMainModule()) {}

  unsigned getInputNumber() const { return InputNumber; }

  /// Execute one line of REPL input. Returns true to continue, false to quit.
  bool handleInput(llvm::StringRef Line) {
    // Check for quit commands.
    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed == ":quit" || Trimmed == ":exit" || Trimmed == ":q")
      return false;

    // TODO: Parse, type-check, transform AST, lower to SIL, JIT, and execute.
    llvm::outs() << "(not yet implemented) You entered: " << Line << "\n";

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