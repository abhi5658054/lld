//===- LTO.cpp ------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "LTO.h"
#include "Config.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Common/Args.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Strings.h"
#include "lld/Common/TargetOptionsCommandFlags.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/LTO/Caching.h"
#include "llvm/LTO/Config.h"
#include "llvm/LTO/LTO.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace lld;
using namespace lld::wasm;

static std::unique_ptr<lto::LTO> createLTO() {
  lto::Config C;
  C.Options = initTargetOptionsFromCodeGenFlags();

  // Always emit a section per function/data with LTO.
  C.Options.FunctionSections = true;
  C.Options.DataSections = true;

  C.DisableVerify = Config->DisableVerify;
  C.DiagHandler = diagnosticHandler;
  C.OptLevel = Config->LTOO;
  C.MAttrs = getMAttrs();
  C.CGOptLevel = args::getCGOptLevel(Config->LTOO);

  if (Config->Relocatable)
    C.RelocModel = None;
  else if (Config->Pic)
    C.RelocModel = Reloc::PIC_;
  else
    C.RelocModel = Reloc::Static;

  if (Config->SaveTemps)
    checkError(C.addSaveTemps(Config->OutputFile.str() + ".",
                              /*UseInputModulePath*/ true));

  lto::ThinBackend Backend;
  if (Config->ThinLTOJobs != -1U)
    Backend = lto::createInProcessThinBackend(Config->ThinLTOJobs);
  return llvm::make_unique<lto::LTO>(std::move(C), Backend,
                                     Config->LTOPartitions);
}

BitcodeCompiler::BitcodeCompiler() : LTOObj(createLTO()) {}

BitcodeCompiler::~BitcodeCompiler() = default;

static void undefine(Symbol *S) {
  if (auto F = dyn_cast<DefinedFunction>(S))
    replaceSymbol<UndefinedFunction>(F, F->getName(), F->getName(),
                                     DefaultModule, 0,
                                     F->getFile(), F->Signature);
  else if (isa<DefinedData>(S))
    replaceSymbol<UndefinedData>(S, S->getName(), 0, S->getFile());
  else
    llvm_unreachable("unexpected symbol kind");
}

void BitcodeCompiler::add(BitcodeFile &F) {
  lto::InputFile &Obj = *F.Obj;
  unsigned SymNum = 0;
  ArrayRef<Symbol *> Syms = F.getSymbols();
  std::vector<lto::SymbolResolution> Resols(Syms.size());

  // Provide a resolution to the LTO API for each symbol.
  for (const lto::InputFile::Symbol &ObjSym : Obj.symbols()) {
    Symbol *Sym = Syms[SymNum];
    lto::SymbolResolution &R = Resols[SymNum];
    ++SymNum;

    // Ideally we shouldn't check for SF_Undefined but currently IRObjectFile
    // reports two symbols for module ASM defined. Without this check, lld
    // flags an undefined in IR with a definition in ASM as prevailing.
    // Once IRObjectFile is fixed to report only one symbol this hack can
    // be removed.
    R.Prevailing = !ObjSym.isUndefined() && Sym->getFile() == &F;
    R.VisibleToRegularObj = Config->Relocatable || Sym->IsUsedInRegularObj ||
                            (R.Prevailing && Sym->isExported());
    if (R.Prevailing)
      undefine(Sym);

    // We tell LTO to not apply interprocedural optimization for wrapped
    // (with --wrap) symbols because otherwise LTO would inline them while
    // their values are still not final.
    R.LinkerRedefined = !Sym->CanInline;
  }
  checkError(LTOObj->add(std::move(F.Obj), Resols));
}

// Merge all the bitcode files we have seen, codegen the result
// and return the resulting objects.
std::vector<StringRef> BitcodeCompiler::compile() {
  unsigned MaxTasks = LTOObj->getMaxTasks();
  Buf.resize(MaxTasks);
  Files.resize(MaxTasks);

  // The --thinlto-cache-dir option specifies the path to a directory in which
  // to cache native object files for ThinLTO incremental builds. If a path was
  // specified, configure LTO to use it as the cache directory.
  lto::NativeObjectCache Cache;
  if (!Config->ThinLTOCacheDir.empty())
    Cache = check(
        lto::localCache(Config->ThinLTOCacheDir,
                        [&](size_t Task, std::unique_ptr<MemoryBuffer> MB) {
                          Files[Task] = std::move(MB);
                        }));

  checkError(LTOObj->run(
      [&](size_t Task) {
        return llvm::make_unique<lto::NativeObjectStream>(
            llvm::make_unique<raw_svector_ostream>(Buf[Task]));
      },
      Cache));

  if (!Config->ThinLTOCacheDir.empty())
    pruneCache(Config->ThinLTOCacheDir, Config->ThinLTOCachePolicy);

  std::vector<StringRef> Ret;
  for (unsigned I = 0; I != MaxTasks; ++I) {
    if (Buf[I].empty())
      continue;
    if (Config->SaveTemps) {
      if (I == 0)
        saveBuffer(Buf[I], Config->OutputFile + ".lto.o");
      else
        saveBuffer(Buf[I], Config->OutputFile + Twine(I) + ".lto.o");
    }
    Ret.emplace_back(Buf[I].data(), Buf[I].size());
  }

  for (std::unique_ptr<MemoryBuffer> &File : Files)
    if (File)
      Ret.push_back(File->getBuffer());

  return Ret;
}
