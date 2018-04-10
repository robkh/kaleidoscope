//===- KaleidoscopeJIT.h - A simple JIT for Kaleidoscope --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Contains a simple JIT definition for use in the kaleidoscope tutorials.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H
#define LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
namespace orc {

class KaleidoscopeJIT {
public:
  using ObjLayerT = RTDyldObjectLinkingLayer;
  using CompileLayerT = IRCompileLayer<ObjLayerT, SimpleCompiler>;
  using OptimizeFunction =
      std::function<std::shared_ptr<Module>(std::shared_ptr<Module>)>;
  using TransformLayerT = IRTransformLayer<CompileLayerT, OptimizeFunction>;
  using CODLayerT = CompileOnDemandLayer<TransformLayerT>;
  using CompileCBManagerT = JITCompileCallbackManager;
  using ModuleHandleT = CODLayerT::ModuleHandleT;

  KaleidoscopeJIT()
      : TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
        ObjectLayer([]() { return std::make_shared<SectionMemoryManager>(); }),
        CompileLayer(ObjectLayer, SimpleCompiler(*TM)),
        OptimizeLayer(CompileLayer,
                      [this](std::shared_ptr<Module> M) {
                        return optimizeModule(std::move(M));
                      }),
        CompileCallbackManager(
            orc::createLocalCompileCallbackManager(TM->getTargetTriple(), 0)),
        CODLayer(OptimizeLayer,
                 [this](Function &F) { return std::set<Function *>({&F}); },
                 *CompileCallbackManager,
                 orc::createLocalIndirectStubsManagerBuilder(
                     TM->getTargetTriple())) {
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }

  TargetMachine &getTargetMachine() { return *TM; }

  ModuleHandleT addModule(std::unique_ptr<Module> M) {
    // We need a memory manager to allocate memory and resolve symbols for this
    // new module. Create one that resolves symbols by looking back into the
    // JIT.
    auto Resolver = createLambdaResolver(
        [&](const std::string &Name) {
          if (auto Sym = CODLayer.findSymbol(Name, false))
            return Sym;
          return JITSymbol(nullptr);
        },
        [](const std::string &S) { return nullptr; });
    auto H = cantFail(CODLayer.addModule(std::move(M), std::move(Resolver)));

    ModuleHandles.push_back(H);
    return H;
  }

  void removeModule(ModuleHandleT H) {
    ModuleHandles.erase(find(ModuleHandles, H));
    cantFail(CODLayer.removeModule(H));
  }

  JITSymbol findSymbol(const std::string Name) {
    return findMangledSymbol(mangle(Name));
  }

private:
  std::string mangle(const std::string &Name) {
    std::string MangledName;
    {
      raw_string_ostream MangledNameStream(MangledName);
      Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
    }
    return MangledName;
  }

  JITSymbol findMangledSymbol(const std::string &Name) {
#ifdef LLVM_ON_WIN32
    // The symbol lookup of ObjectLinkingLayer uses the SymbolRef::SF_Exported
    // flag to decide whether a symbol will be visible or not, when we call
    // IRCompileLayer::findSymbolIn with ExportedSymbolsOnly set to true.
    //
    // But for Windows COFF objects, this flag is currently never set.
    // For a potential solution see: https://reviews.llvm.org/rL258665
    // For now, we allow non-exported symbols on Windows as a workaround.
    const bool ExportedSymbolsOnly = false;
#else
    const bool ExportedSymbolsOnly = true;
#endif

    // Search modules in reverse order: from last added to first added.
    // This is the opposite of the usual search order for dlsym, but makes more
    // sense in a REPL where we want to bind to the newest available definition.
    for (auto H : make_range(ModuleHandles.rbegin(), ModuleHandles.rend()))
      if (auto Sym = CODLayer.findSymbolIn(H, Name, ExportedSymbolsOnly))
        return Sym;

    // If we can't find the symbol in the JIT, try looking in the host process.
    if (auto SymAddr = RTDyldMemoryManager::getSymbolAddressInProcess(Name))
      return JITSymbol(SymAddr, JITSymbolFlags::Exported);

#ifdef LLVM_ON_WIN32
    // For Windows retry without "_" at beginning, as RTDyldMemoryManager uses
    // GetProcAddress and standard libraries like msvcrt.dll use names
    // with and without "_" (for example "_itoa" but "sin").
    if (Name.length() > 2 && Name[0] == '_')
      if (auto SymAddr =
              RTDyldMemoryManager::getSymbolAddressInProcess(Name.substr(1)))
        return JITSymbol(SymAddr, JITSymbolFlags::Exported);
#endif

    return nullptr;
  }

  std::shared_ptr<Module> optimizeModule(std::shared_ptr<Module> M) {
    // create a function pass manager
    auto FPM = std::make_unique<legacy::FunctionPassManager>(M.get());

    // Add some optimizations.
    FPM->add(createInstructionCombiningPass());
    FPM->add(createReassociatePass());
    FPM->add(createGVNPass());
    FPM->add(createCFGSimplificationPass());
    FPM->doInitialization();

    // Run the optimizations.
    for (auto &F : *M)
      FPM->run(F);

    return M;
  }

  std::unique_ptr<TargetMachine> TM;
  const DataLayout DL;
  ObjLayerT ObjectLayer;
  CompileLayerT CompileLayer;
  TransformLayerT OptimizeLayer;
  std::unique_ptr<CompileCBManagerT> CompileCallbackManager;
  CODLayerT CODLayer;
  std::vector<ModuleHandleT> ModuleHandles;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_KALEIDOSCOPEJIT_H
