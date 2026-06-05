if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  add_swift_host_library(swiftImmediate STATIC
    Immediate.cpp
    Interpreter.cpp
    REPLTransforms.cpp
    WasmExecutor.cpp
    LLVM_LINK_COMPONENTS
      transformutils)
else()
  add_swift_host_library(swiftImmediate STATIC
    SwiftMaterializationUnit.cpp
    Immediate.cpp
    Interpreter.cpp
    REPLTransforms.cpp
    LLVM_LINK_COMPONENTS
      executionengine
      linker
      mcjit
      orcjit
      orctargetprocess
      jitlink
      transformutils)
endif()

target_link_libraries(swiftImmediate PRIVATE
  swiftFrontend
  swiftIDE
  swiftIRGen
  swiftSILGen
  swiftSILOptimizer)

# Mirror what clangInterpreter does: pull in wasm-ld for in-process linking
# of REPL side-modules when building for the browser.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  target_link_libraries(swiftImmediate PRIVATE lldWasm lldCommon)
endif()
