//===--- MachORewriteInstance.cpp - Instance of a rewriting process. ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "MachORewriteInstance.h"
#include "BinaryContext.h"
#include "BinaryEmitter.h"
#include "BinaryFunction.h"
#include "BinaryPassManager.h"
#include "ExecutableFileMemoryManager.h"
#include "JumpTable.h"
#include "Passes/Instrumentation.h"
#include "Passes/PatchEntries.h"
#include "Utils.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/ToolOutputFile.h"

namespace opts {

using namespace llvm;
extern cl::opt<unsigned> AlignText;
extern cl::opt<bool> CheckOverlappingElements;
extern cl::opt<bool> ForcePatch;
extern cl::opt<bool> Instrument;
extern cl::opt<bool> InstrumentCalls;
extern cl::opt<bolt::JumpTableSupportLevel> JumpTables;
extern cl::opt<bool> KeepTmp;
extern cl::opt<bool> NeverPrint;
extern cl::opt<std::string> OutputFilename;
extern cl::opt<bool> PrintAfterBranchFixup;
extern cl::opt<bool> PrintFinalized;
extern cl::opt<bool> PrintReordered;
extern cl::opt<bool> PrintSections;
extern cl::opt<bool> PrintDisasm;
extern cl::opt<bool> PrintCFG;
extern cl::opt<std::string> RuntimeInstrumentationLib;
extern cl::opt<unsigned> Verbosity;
} // namespace opts

namespace llvm {
namespace bolt {

#undef DEBUG_TYPE
#define DEBUG_TYPE "bolt"

MachORewriteInstance::MachORewriteInstance(object::MachOObjectFile *InputFile,
                                           StringRef ToolPath)
    : InputFile(InputFile), ToolPath(ToolPath),
      BC(BinaryContext::createBinaryContext(
          InputFile, /* IsPIC */ true,
          DWARFContext::create(*InputFile, nullptr,
                               DWARFContext::defaultErrorHandler, "",
                               false))) {}

void MachORewriteInstance::readSpecialSections() {
  for (const auto &Section : InputFile->sections()) {
    StringRef SectionName;
    check_error(Section.getName(SectionName), "cannot get section name");
    // Only register sections with names.
    if (!SectionName.empty()) {
      BC->registerSection(Section);
      DEBUG(dbgs() << "BOLT-DEBUG: registering section " << SectionName
                   << " @ 0x" << Twine::utohexstr(Section.getAddress()) << ":0x"
                   << Twine::utohexstr(Section.getAddress() + Section.getSize())
                   << "\n");
    }
  }

  if (opts::PrintSections) {
    outs() << "BOLT-INFO: Sections from original binary:\n";
    BC->printSections(outs());
  }
}

namespace {

struct DataInCodeRegion {
  explicit DataInCodeRegion(DiceRef D) {
    D.getOffset(Offset);
    D.getLength(Length);
    D.getKind(Kind);
  }

  uint32_t Offset;
  uint16_t Length;
  uint16_t Kind;
};

std::vector<DataInCodeRegion> readDataInCode(const MachOObjectFile &O) {
  const MachO::linkedit_data_command DataInCodeLC =
      O.getDataInCodeLoadCommand();
  const uint32_t NumberOfEntries =
      DataInCodeLC.datasize / sizeof(MachO::data_in_code_entry);
  std::vector<DataInCodeRegion> DataInCode;
  DataInCode.reserve(NumberOfEntries);
  for (auto I = O.begin_dices(), E = O.end_dices(); I != E; ++I)
    DataInCode.emplace_back(*I);
  std::stable_sort(DataInCode.begin(), DataInCode.end(),
                   [](DataInCodeRegion LHS, DataInCodeRegion RHS) {
                     return LHS.Offset < RHS.Offset;
                   });
  return DataInCode;
}

Optional<uint64_t> readStartAddress(const MachOObjectFile &O) {
  Optional<uint64_t> StartOffset;
  Optional<uint64_t> TextVMAddr;
  for (const auto &LC : O.load_commands()) {
    switch (LC.C.cmd) {
    case MachO::LC_MAIN: {
      MachO::entry_point_command LCMain = O.getEntryPointCommand(LC);
      StartOffset = LCMain.entryoff;
      break;
    }
    case MachO::LC_SEGMENT: {
      MachO::segment_command LCSeg = O.getSegmentLoadCommand(LC);
      StringRef SegmentName(LCSeg.segname,
                            strnlen(LCSeg.segname, sizeof(LCSeg.segname)));
      if (SegmentName == "__TEXT")
        TextVMAddr = LCSeg.vmaddr;
      break;
    }
    case MachO::LC_SEGMENT_64: {
      MachO::segment_command_64 LCSeg = O.getSegment64LoadCommand(LC);
      StringRef SegmentName(LCSeg.segname,
                            strnlen(LCSeg.segname, sizeof(LCSeg.segname)));
      if (SegmentName == "__TEXT")
        TextVMAddr = LCSeg.vmaddr;
      break;
    }
    default:
      continue;
    }
  }
  return (TextVMAddr && StartOffset)
             ? Optional<uint64_t>(*TextVMAddr + *StartOffset)
             : llvm::None;
}

} // anonymous namespace

void MachORewriteInstance::discoverFileObjects() {
  std::vector<SymbolRef> FunctionSymbols;
  for (const SymbolRef &S : InputFile->symbols()) {
    SymbolRef::Type Type = cantFail(S.getType(), "cannot get symbol type");
    if (Type == SymbolRef::ST_Function)
      FunctionSymbols.push_back(S);
  }
  if (FunctionSymbols.empty())
    return;
  std::stable_sort(FunctionSymbols.begin(), FunctionSymbols.end(),
                   [](const SymbolRef &LHS, const SymbolRef &RHS) {
                     return LHS.getValue() < RHS.getValue();
                   });
  for (size_t Index = 0; Index < FunctionSymbols.size(); ++Index) {
    const uint64_t Address = FunctionSymbols[Index].getValue();
    auto Section = BC->getSectionForAddress(Address);
    // TODO: It happens for some symbols (e.g. __mh_execute_header).
    // Add proper logic to handle them correctly.
    if (!Section) {
      errs() << "BOLT-WARNING: no section found for address " << Address
             << "\n";
      continue;
    }

    std::string SymbolName =
        cantFail(FunctionSymbols[Index].getName(), "cannot get symbol name")
            .str();
    // Uniquify names of local symbols.
    if (!(FunctionSymbols[Index].getFlags() & SymbolRef::SF_Global))
      SymbolName = NR.uniquify(SymbolName);

    section_iterator S = cantFail(FunctionSymbols[Index].getSection());
    uint64_t EndAddress = S->getAddress() + S->getSize();

    size_t NFIndex = Index + 1;
    // Skip aliases.
    while (NFIndex < FunctionSymbols.size() &&
           FunctionSymbols[NFIndex].getValue() == Address)
      ++NFIndex;
    if (NFIndex < FunctionSymbols.size() &&
        S == cantFail(FunctionSymbols[NFIndex].getSection()))
      EndAddress = FunctionSymbols[NFIndex].getValue();

    const uint64_t SymbolSize = EndAddress - Address;
    const auto It = BC->getBinaryFunctions().find(Address);
    if (It == BC->getBinaryFunctions().end()) {
      BinaryFunction *Function = BC->createBinaryFunction(
          std::move(SymbolName), *Section, Address, SymbolSize);
      if (!opts::Instrument)
        Function->setOutputAddress(Function->getAddress());

    } else {
      It->second.addAlternativeName(std::move(SymbolName));
    }
  }

  const std::vector<DataInCodeRegion> DataInCode = readDataInCode(*InputFile);

  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    Function.setMaxSize(Function.getSize());

    auto FunctionData = Function.getData();
    if (!FunctionData) {
      errs() << "BOLT-ERROR: corresponding section is non-executable or "
             << "empty for function " << Function << '\n';
      continue;
    }

    // Treat zero-sized functions as non-simple ones.
    if (Function.getSize() == 0) {
      Function.setSimple(false);
      continue;
    }

    // Offset of the function in the file.
    const auto *FileBegin =
        reinterpret_cast<const uint8_t *>(InputFile->getData().data());
    Function.setFileOffset(FunctionData->begin() - FileBegin);

    // Treat functions which contain data in code as non-simple ones.
    const auto It = std::lower_bound(
        DataInCode.cbegin(), DataInCode.cend(), Function.getFileOffset(),
        [](DataInCodeRegion D, uint64_t Offset) { return D.Offset < Offset; });
    if (It != DataInCode.cend() &&
        It->Offset + It->Length <=
            Function.getFileOffset() + Function.getMaxSize())
      Function.setSimple(false);
  }

  BC->StartFunctionAddress = readStartAddress(*InputFile);
}

void MachORewriteInstance::disassembleFunctions() {
  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    if (!Function.isSimple())
      continue;
    Function.disassemble();
    if (opts::PrintDisasm)
      Function.print(outs(), "after disassembly", true);
    if (!Function.buildCFG(/*AllocId*/ 0)) {
      errs() << "BOLT-WARNING: failed to build CFG for the function "
             << Function << "\n";
    }
  }
}

void MachORewriteInstance::postProcessFunctions() {
  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    if (Function.empty())
      continue;
    Function.postProcessCFG();
    if (opts::PrintCFG)
      Function.print(outs(), "after building cfg", true);
  }
}

void MachORewriteInstance::runOptimizationPasses() {
  BinaryFunctionPassManager Manager(*BC);
  if (opts::Instrument) {
    Manager.registerPass(llvm::make_unique<PatchEntries>());
    Manager.registerPass(llvm::make_unique<Instrumentation>(opts::NeverPrint));
  }
  Manager.registerPass(
      llvm::make_unique<ReorderBasicBlocks>(opts::PrintReordered));
  Manager.registerPass(
      llvm::make_unique<FixupBranches>(opts::PrintAfterBranchFixup));
  // This pass should always run last.*
  Manager.registerPass(
      llvm::make_unique<FinalizeFunctions>(opts::PrintFinalized));

  Manager.runPasses();
}

void MachORewriteInstance::mapInstrumentationSection(orc::VModuleKey Key, StringRef SectionName) {
  if (!opts::Instrument)
    return;
  ErrorOr<BinarySection &> Section = BC->getUniqueSectionByName(SectionName);
  if (!Section) {
    llvm::errs() << "Cannot find " + SectionName + " section\n";
    exit(1);
  }
  if (!Section->hasValidSectionID())
    return;
  OLT->mapSectionAddress(Key, Section->getSectionID(), Section->getAddress());
}

void MachORewriteInstance::mapCodeSections(orc::VModuleKey Key) {
  for (BinaryFunction *Function : BC->getAllBinaryFunctions()) {
    if (!Function->isEmitted())
      continue;
    if (Function->getOutputAddress() == 0)
      continue;
    ErrorOr<BinarySection &> FuncSection = Function->getCodeSection();
    if (!FuncSection)
      report_error(
          (Twine("Cannot find section for function ") + Function->getOneName())
              .str(),
          FuncSection.getError());

    FuncSection->setOutputAddress(Function->getOutputAddress());
    DEBUG(dbgs() << "BOLT: mapping 0x"
                 << Twine::utohexstr(FuncSection->getAllocAddress()) << " to 0x"
                 << Twine::utohexstr(Function->getOutputAddress()) << '\n');
    OLT->mapSectionAddress(Key, FuncSection->getSectionID(),
                           Function->getOutputAddress());
    Function->setImageAddress(FuncSection->getAllocAddress());
    Function->setImageSize(FuncSection->getOutputSize());
  }

  if (opts::Instrument) {
    ErrorOr<BinarySection &> BOLT = BC->getUniqueSectionByName("__bolt");
    if (!BOLT) {
      llvm::errs() << "Cannot find __bolt section\n";
      exit(1);
    }
    uint64_t Addr = BOLT->getAddress();
    for (BinaryFunction *Function : BC->getAllBinaryFunctions()) {
      if (!Function->isEmitted())
        continue;
      if (Function->getOutputAddress() != 0)
        continue;
      ErrorOr<BinarySection &> FuncSection = Function->getCodeSection();
      assert(FuncSection && "cannot find section for function");
      Addr = llvm::alignTo(Addr, 4);
      FuncSection->setOutputAddress(Addr);
      OLT->mapSectionAddress(Key, FuncSection->getSectionID(), Addr);
      Function->setFileOffset(Addr - BOLT->getAddress() +
                              BOLT->getInputFileOffset());
      Function->setImageAddress(FuncSection->getAllocAddress());
      Function->setImageSize(FuncSection->getOutputSize());
      Addr += FuncSection->getOutputSize();
    }
  }
}

void MachORewriteInstance::emitAndLink() {
  std::error_code EC;
  std::unique_ptr<::llvm::ToolOutputFile> TempOut =
      llvm::make_unique<::llvm::ToolOutputFile>(
          opts::OutputFilename + ".bolt.o", EC, sys::fs::F_None);
  check_error(EC, "cannot create output object file");

  if (opts::KeepTmp)
    TempOut->keep();

  std::unique_ptr<buffer_ostream> BOS =
      make_unique<buffer_ostream>(TempOut->os());
  raw_pwrite_stream *OS = BOS.get();

  MCCodeEmitter *MCE =
      BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  MCAsmBackend *MAB =
      BC->TheTarget->createMCAsmBackend(*BC->STI, *BC->MRI, MCTargetOptions());
  std::unique_ptr<MCStreamer> Streamer(BC->TheTarget->createMCObjectStreamer(
      *BC->TheTriple, *BC->Ctx, std::unique_ptr<MCAsmBackend>(MAB), *OS,
      std::unique_ptr<MCCodeEmitter>(MCE), *BC->STI,
      /* RelaxAll */ false,
      /* IncrementalLinkerCompatible */ false,
      /* DWARFMustBeAtTheEnd */ false));
  emitBinaryContext(*Streamer, *BC, getOrgSecPrefix());
  Streamer->Finish();

  std::unique_ptr<MemoryBuffer> ObjectMemBuffer =
      MemoryBuffer::getMemBuffer(BOS->str(), "in-memory object file", false);
  std::unique_ptr<object::ObjectFile> Obj = cantFail(
      object::ObjectFile::createObjectFile(ObjectMemBuffer->getMemBufferRef()),
      "error creating in-memory object");
  assert(Obj && "createObjectFile cannot return nullptr");

  auto Resolver = orc::createLegacyLookupResolver(
      [&](const std::string &Name) -> JITSymbol {
        llvm::errs() << "looking for " << Name << "\n";
        assert(!BC->EFMM->ObjectsLoaded &&
               "Linking multiple objects is unsupported");
        DEBUG(dbgs() << "BOLT: looking for " << Name << "\n");
        if (auto *I = BC->getBinaryDataByName(Name)) {
          const uint64_t Address = I->isMoved() && !I->isJumpTable()
                                       ? I->getOutputAddress()
                                       : I->getAddress();
          DEBUG(dbgs() << "Resolved to address 0x" << Twine::utohexstr(Address)
                       << "\n");
          return JITSymbol(Address, JITSymbolFlags());
        }
        DEBUG(dbgs() << "Resolved to address 0x0\n");
        return JITSymbol(nullptr);
      },
      [](Error Err) { cantFail(std::move(Err), "lookup failed"); });

  Resolver->setAllowsZeroSymbols(true);

  MCAsmLayout FinalLayout(
      static_cast<MCObjectStreamer *>(Streamer.get())->getAssembler());

  SSP.reset(new decltype(SSP)::element_type());
  ES.reset(new decltype(ES)::element_type(*SSP));
  BC->EFMM.reset(new ExecutableFileMemoryManager(*BC, /*AllowStubs*/ false));

  const orc::VModuleKey K = ES->allocateVModule();
  OLT.reset(new decltype(OLT)::element_type(
      *ES,
      [this, &Resolver](orc::VModuleKey Key) {
        orc::RTDyldObjectLinkingLayer::Resources R;
        R.MemMgr = BC->EFMM;
        R.Resolver = Resolver;
        return R;
      },
      [&](orc::VModuleKey Key, const object::ObjectFile &Obj,
          const RuntimeDyld::LoadedObjectInfo &) {
        if (Key == K) {
          mapCodeSections(Key);
          mapInstrumentationSection(Key, "__counters");
        } else {
          // TODO: Refactor addRuntimeLibSections to work properly on Mach-O
          // and use it here.
          mapInstrumentationSection(Key, "I__setup");
          mapInstrumentationSection(Key, "I__fini");
          mapInstrumentationSection(Key, "I__data");
          mapInstrumentationSection(Key, "I__text");
          mapInstrumentationSection(Key, "I__cstring");
        }
      },
      [&](orc::VModuleKey Key) {
      }));

  OLT->setProcessAllSections(true);
  cantFail(OLT->addObject(K, std::move(ObjectMemBuffer)));
  cantFail(OLT->emitAndFinalize(K));

  if (auto *RtLibrary = BC->getRuntimeLibrary()) {
    RtLibrary->link(*BC, ToolPath, *ES, *OLT);
  }
}

void MachORewriteInstance::writeInstrumentationSection(StringRef SectionName,
                                                       raw_pwrite_stream &OS) {
  if (!opts::Instrument)
    return;
  ErrorOr<BinarySection &> Section = BC->getUniqueSectionByName(SectionName);
  if (!Section) {
    llvm::errs() << "Cannot find " + SectionName + " section\n";
    exit(1);
  }
  if (!Section->hasValidSectionID())
    return;
  assert(Section->getInputFileOffset() &&
         "Section input offset cannot be zero");
  assert(Section->getAllocAddress() && "Section alloc address cannot be zero");
  assert(Section->getOutputSize() && "Section output size cannot be zero");
  OS.pwrite(reinterpret_cast<char *>(Section->getAllocAddress()),
            Section->getOutputSize(), Section->getInputFileOffset());
}

void MachORewriteInstance::rewriteFile() {
  std::error_code EC;
  Out = llvm::make_unique<ToolOutputFile>(
      opts::OutputFilename, EC, sys::fs::F_None,
      sys::fs::all_read | sys::fs::all_write | sys::fs::all_exe);
  check_error(EC, "cannot create output executable file");
  raw_fd_ostream &OS = Out->os();
  OS << InputFile->getData();

  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;
    if (!Function.isSimple())
      continue;
    assert(Function.isEmitted() && "Simple function has not been emitted");
    if (!opts::Instrument && (Function.getImageSize() > Function.getMaxSize()))
      continue;
    if (opts::Verbosity >= 2)
      outs() << "BOLT: rewriting function \"" << Function << "\"\n";
    OS.pwrite(reinterpret_cast<char *>(Function.getImageAddress()),
              Function.getImageSize(), Function.getFileOffset());
  }

  for (const BinaryFunction *Function : BC->getInjectedBinaryFunctions()) {
    OS.pwrite(reinterpret_cast<char *>(Function->getImageAddress()),
              Function->getImageSize(), Function->getFileOffset());
  }

  // TODO: Refactor addRuntimeLibSections to work properly on Mach-O and
  // use it here.
  writeInstrumentationSection("I__setup", OS);
  writeInstrumentationSection("I__fini", OS);
  writeInstrumentationSection("I__data", OS);
  writeInstrumentationSection("I__text", OS);
  writeInstrumentationSection("I__cstring", OS);

  Out->keep();
}

void MachORewriteInstance::adjustCommandLineOptions() {
  opts::CheckOverlappingElements = false;
  if (!opts::AlignText.getNumOccurrences())
    opts::AlignText = BC->PageAlign;
  if (opts::Instrument.getNumOccurrences())
    opts::ForcePatch = true;
  opts::JumpTables = JTS_MOVE;
  opts::InstrumentCalls = false;
  opts::RuntimeInstrumentationLib = "libbolt_rt_instr_osx.a";
}

void MachORewriteInstance::run() {
  adjustCommandLineOptions();
  readSpecialSections();
  discoverFileObjects();
  disassembleFunctions();
  postProcessFunctions();
  runOptimizationPasses();
  emitAndLink();
  rewriteFile();
}

MachORewriteInstance::~MachORewriteInstance() {}

} // namespace bolt
} // namespace llvm
