//===--- RewriteInstance.cpp - Interface for machine-level function -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//


#include "BinaryBasicBlock.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "BinaryPassManager.h"
#include "DataReader.h"
#include "Exceptions.h"
#include "RewriteInstance.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <fstream>
#include <stack>
#include <system_error>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace object;
using namespace bolt;

namespace opts {

static cl::opt<std::string>
OutputFilename("o", cl::desc("<output file>"), cl::Required);

// The default verbosity level (0) is pretty terse, level 1 is fairly
// verbose and usually prints some informational message for every
// function processed.  Level 2 is for the noisiest of messages and
// often prints a message per basic block.
// Error messages should never be suppressed by the verbosity level.
// Only warnings and info messages should be affected.
//
// The rational behind stream usage is as follows:
// outs() for info and debugging controlled by command line flags.
// errs() for errors and warnings.
// dbgs() for output within DEBUG().
cl::opt<unsigned>
Verbosity("v",
          cl::desc("set verbosity level for diagnostic output"),
          cl::init(0),
          cl::ZeroOrMore);

static cl::list<std::string>
BreakFunctionNames("break-funcs",
                   cl::CommaSeparated,
                   cl::desc("list of functions to core dump on (debugging)"),
                   cl::value_desc("func1,func2,func3,..."),
                   cl::Hidden);

static cl::list<std::string>
FunctionNames("funcs",
              cl::CommaSeparated,
              cl::desc("list of functions to optimize"),
              cl::value_desc("func1,func2,func3,..."));

static cl::opt<std::string>
FunctionNamesFile("funcs-file",
                  cl::desc("file with list of functions to optimize"));

static cl::list<std::string>
SkipFunctionNames("skip-funcs",
                  cl::CommaSeparated,
                  cl::desc("list of functions to skip"),
                  cl::value_desc("func1,func2,func3,..."));

static cl::opt<std::string>
SkipFunctionNamesFile("skip-funcs-file",
                      cl::desc("file with list of functions to skip"));

static cl::opt<unsigned>
MaxFunctions("max-funcs",
             cl::desc("maximum # of functions to overwrite"),
             cl::ZeroOrMore);

cl::opt<BinaryFunction::SplittingType>
SplitFunctions("split-functions",
               cl::desc("split functions into hot and cold regions"),
               cl::init(BinaryFunction::ST_NONE),
               cl::values(clEnumValN(BinaryFunction::ST_NONE, "0",
                                     "do not split any function"),
                          clEnumValN(BinaryFunction::ST_EH, "1",
                                     "split all landing pads"),
                          clEnumValN(BinaryFunction::ST_LARGE, "2",
                                     "also split if function too large to fit"),
                          clEnumValN(BinaryFunction::ST_ALL, "3",
                                     "split all functions"),
                          clEnumValEnd),
               cl::ZeroOrMore);

static cl::opt<bool>
UpdateDebugSections("update-debug-sections",
                    cl::desc("update DWARF debug sections of the executable"),
                    cl::ZeroOrMore);

static cl::opt<bool>
FixDebugInfoLargeFunctions("fix-debuginfo-large-functions",
                           cl::init(true),
                           cl::desc("do another pass if we encounter large "
                                    "functions, to correct their debug info."),
                           cl::ZeroOrMore,
                           cl::ReallyHidden);

static cl::opt<bool>
AlignBlocks("align-blocks",
            cl::desc("try to align BBs inserting nops"),
            cl::ZeroOrMore);

static cl::opt<bool>
UseGnuStack("use-gnu-stack",
            cl::desc("use GNU_STACK program header for new segment"),
            cl::ZeroOrMore);

static cl::opt<bool>
DumpEHFrame("dump-eh-frame", cl::desc("dump parsed .eh_frame (debugging)"),
            cl::ZeroOrMore,
            cl::Hidden);

cl::opt<bool>
PrintAll("print-all", cl::desc("print functions after each stage"),
         cl::ZeroOrMore,
         cl::Hidden);

cl::opt<bool>
DumpDotAll("dump-dot-all",
           cl::desc("dump function CFGs to graphviz format after each stage"),
           cl::ZeroOrMore,
           cl::Hidden);

static cl::opt<bool>
PrintCFG("print-cfg", cl::desc("print functions after CFG construction"),
         cl::ZeroOrMore,
         cl::Hidden);

static cl::opt<bool>
PrintLoopInfo("print-loops", cl::desc("print loop related information"),
              cl::ZeroOrMore,
              cl::Hidden);

static cl::opt<bool>
PrintDisasm("print-disasm", cl::desc("print function after disassembly"),
            cl::ZeroOrMore,
            cl::Hidden);

static cl::opt<bool>
KeepTmp("keep-tmp",
        cl::desc("preserve intermediate .o file"),
        cl::Hidden);

cl::opt<bool>
AllowStripped("allow-stripped",
              cl::desc("allow processing of stripped binaries"),
              cl::Hidden);

// Check against lists of functions from options if we should
// optimize the function with a given name.
bool shouldProcess(const BinaryFunction &Function) {
  if (opts::MaxFunctions && Function.getFunctionNumber() > opts::MaxFunctions)
    return false;

  auto populateFunctionNames = [](cl::opt<std::string> &FunctionNamesFile,
                                  cl::list<std::string> &FunctionNames) {
    assert(!FunctionNamesFile.empty() && "unexpected empty file name");
    std::ifstream FuncsFile(FunctionNamesFile, std::ios::in);
    std::string FuncName;
    while (std::getline(FuncsFile, FuncName)) {
      FunctionNames.push_back(FuncName);
    }
    FunctionNamesFile = "";
  };

  if (!FunctionNamesFile.empty())
    populateFunctionNames(FunctionNamesFile, FunctionNames);

  if (!SkipFunctionNamesFile.empty())
    populateFunctionNames(SkipFunctionNamesFile, SkipFunctionNames);

  bool IsValid = true;
  if (!FunctionNames.empty()) {
    IsValid = false;
    for (auto &Name : FunctionNames) {
      if (Function.hasName(Name)) {
        IsValid = true;
        break;
      }
    }
  }
  if (!IsValid)
    return false;

  if (!SkipFunctionNames.empty()) {
    for (auto &Name : SkipFunctionNames) {
      if (Function.hasName(Name)) {
        IsValid = false;
        break;
      }
    }
  }

  return IsValid;
}

} // namespace opts

constexpr const char *RewriteInstance::DebugSectionsToOverwrite[];

static void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << "BOLT-ERROR: '" << Message << "': " << EC.message() << ".\n";
  exit(1);
}

static void check_error(std::error_code EC, StringRef Message) {
  if (!EC)
    return;
  report_error(Message, EC);
}

uint8_t *ExecutableFileMemoryManager::allocateSection(intptr_t Size,
                                                      unsigned Alignment,
                                                      unsigned SectionID,
                                                      StringRef SectionName,
                                                      bool IsCode,
                                                      bool IsReadOnly) {
  uint8_t *ret;
  if (IsCode) {
    ret = SectionMemoryManager::allocateCodeSection(Size, Alignment,
                                                    SectionID, SectionName);
  } else {
    ret = SectionMemoryManager::allocateDataSection(Size, Alignment,
                                                    SectionID, SectionName,
                                                    IsReadOnly);
  }

  DEBUG(dbgs() << "BOLT: allocating " << (IsCode ? "code" : "data")
               << " section : " << SectionName
               << " with size " << Size << ", alignment " << Alignment
               << " at 0x" << ret << "\n");

  SectionMapInfo[SectionName] = SectionInfo(reinterpret_cast<uint64_t>(ret),
                                            Size,
                                            Alignment,
                                            IsCode,
                                            IsReadOnly,
                                            0,
                                            0,
                                            SectionID);

  return ret;
}

/// Notifier for non-allocatable (note) section.
uint8_t *ExecutableFileMemoryManager::recordNoteSection(
    const uint8_t *Data,
    uintptr_t Size,
    unsigned Alignment,
    unsigned SectionID,
    StringRef SectionName) {
  DEBUG(dbgs() << "BOLT: note section "
               << SectionName
               << " with size " << Size << ", alignment " << Alignment
               << " at 0x"
               << Twine::utohexstr(reinterpret_cast<uint64_t>(Data)) << '\n');
  if (SectionName == ".debug_line") {
    // We need to make a copy of the section contents if we'll need it for
    // a future reference.
    uint8_t *DataCopy = new uint8_t[Size];
    memcpy(DataCopy, Data, Size);
    NoteSectionInfo[SectionName] =
      SectionInfo(reinterpret_cast<uint64_t>(DataCopy),
                  Size,
                  Alignment,
                  /*IsCode=*/false,
                  /*IsReadOnly*/true,
                  0,
                  0,
                  SectionID);
    return DataCopy;
  } else {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring section " << SectionName
                 << " in recordNoteSection()\n");
    return nullptr;
  }
}

bool ExecutableFileMemoryManager::finalizeMemory(std::string *ErrMsg) {
  DEBUG(dbgs() << "BOLT: finalizeMemory()\n");
  return SectionMemoryManager::finalizeMemory(ErrMsg);
}

ExecutableFileMemoryManager::~ExecutableFileMemoryManager() {
  for (auto &SII : NoteSectionInfo) {
    delete[] reinterpret_cast<uint8_t *>(SII.second.AllocAddress);
  }
}

namespace {

/// Create BinaryContext for a given architecture \p ArchName and
/// triple \p TripleName.
std::unique_ptr<BinaryContext> createBinaryContext(
    std::string ArchName,
    std::string TripleName,
    const DataReader &DR,
    std::unique_ptr<DWARFContext> DwCtx) {

  std::string Error;

  std::unique_ptr<Triple> TheTriple = llvm::make_unique<Triple>(TripleName);
  const Target *TheTarget = TargetRegistry::lookupTarget(ArchName,
                                                         *TheTriple,
                                                         Error);
  if (!TheTarget) {
    errs() << "BOLT-ERROR: " << Error;
    return nullptr;
  }

  std::unique_ptr<const MCRegisterInfo> MRI(
      TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "BOLT-ERROR: no register info for target " << TripleName << "\n";
    return nullptr;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> AsmInfo(
      TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!AsmInfo) {
    errs() << "BOLT-ERROR: no assembly info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "BOLT-ERROR: no subtarget info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "BOLT-ERROR: no instruction info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<MCObjectFileInfo> MOFI =
    llvm::make_unique<MCObjectFileInfo>();
  std::unique_ptr<MCContext> Ctx =
    llvm::make_unique<MCContext>(AsmInfo.get(), MRI.get(), MOFI.get());
  MOFI->InitMCObjectFileInfo(*TheTriple, Reloc::Default,
                             CodeModel::Default, *Ctx);

  std::unique_ptr<MCDisassembler> DisAsm(
    TheTarget->createMCDisassembler(*STI, *Ctx));

  if (!DisAsm) {
    errs() << "BOLT-ERROR: no disassembler for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCInstrAnalysis> MIA(
      TheTarget->createMCInstrAnalysis(MII.get()));
  if (!MIA) {
    errs() << "BOLT-ERROR: failed to create instruction analysis for target"
           << TripleName << "\n";
    return nullptr;
  }

  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  std::unique_ptr<MCInstPrinter> InstructionPrinter(
      TheTarget->createMCInstPrinter(Triple(TripleName), AsmPrinterVariant,
                                     *AsmInfo, *MII, *MRI));
  if (!InstructionPrinter) {
    errs() << "BOLT-ERROR: no instruction printer for target " << TripleName
           << '\n';
    return nullptr;
  }
  InstructionPrinter->setPrintImmHex(true);

  std::unique_ptr<MCCodeEmitter> MCE(
      TheTarget->createMCCodeEmitter(*MII, *MRI, *Ctx));

  // Make sure we don't miss any output on core dumps.
  outs().SetUnbuffered();
  errs().SetUnbuffered();
  dbgs().SetUnbuffered();

  auto BC =
      llvm::make_unique<BinaryContext>(std::move(Ctx),
                                       std::move(DwCtx),
                                       std::move(TheTriple),
                                       TheTarget,
                                       TripleName,
                                       std::move(MCE),
                                       std::move(MOFI),
                                       std::move(AsmInfo),
                                       std::move(MII),
                                       std::move(STI),
                                       std::move(InstructionPrinter),
                                       std::move(MIA),
                                       std::move(MRI),
                                       std::move(DisAsm),
                                       DR);

  return BC;
}

} // namespace

RewriteInstance::RewriteInstance(ELFObjectFileBase *File,
                                 const DataReader &DR)
    : InputFile(File),
      BC(createBinaryContext("x86-64", "x86_64-unknown-linux", DR,
         std::unique_ptr<DWARFContext>(new DWARFContextInMemory(*InputFile)))) {
}

RewriteInstance::~RewriteInstance() {}

void RewriteInstance::reset() {
  BinaryFunctions.clear();
  FileSymRefs.clear();
  auto &DR = BC->DR;
  BC = createBinaryContext("x86-64", "x86_64-unknown-linux", DR,
           std::unique_ptr<DWARFContext>(new DWARFContextInMemory(*InputFile)));
  CFIRdWrt.reset(nullptr);
  SectionMM.reset(nullptr);
  Out.reset(nullptr);
  EHFrame = nullptr;
  FailedAddresses.clear();
  RangesSectionsWriter.reset();
  TotalScore = 0;
}

void RewriteInstance::discoverStorage() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();

  // This is where the first segment and ELF header were allocated.
  uint64_t FirstAllocAddress = std::numeric_limits<uint64_t>::max();

  NextAvailableAddress = 0;
  uint64_t NextAvailableOffset = 0;
  for (const auto &Phdr : Obj->program_headers()) {
    if (Phdr.p_type == ELF::PT_LOAD) {
      FirstAllocAddress = std::min(FirstAllocAddress,
                                   static_cast<uint64_t>(Phdr.p_vaddr));
      NextAvailableAddress = std::max(NextAvailableAddress,
                                      Phdr.p_vaddr + Phdr.p_memsz);
      NextAvailableOffset = std::max(NextAvailableOffset,
                                     Phdr.p_offset + Phdr.p_filesz);
    }
  }

  assert(NextAvailableAddress && NextAvailableOffset &&
         "no PT_LOAD pheader seen");

  outs() << "BOLT-INFO: first alloc address is 0x"
         << Twine::utohexstr(FirstAllocAddress) << '\n';

  FirstNonAllocatableOffset = NextAvailableOffset;

  NextAvailableAddress = RoundUpToAlignment(NextAvailableAddress, PageAlign);
  NextAvailableOffset = RoundUpToAlignment(NextAvailableOffset, PageAlign);

  if (!opts::UseGnuStack) {
    // This is where the black magic happens. Creating PHDR table in a segment
    // other than that containing ELF header is tricky. Some loaders and/or
    // parts of loaders will apply e_phoff from ELF header assuming both are in
    // the same segment, while others will do the proper calculation.
    // We create the new PHDR table in such a way that both of the methods
    // of loading and locating the table work. There's a slight file size
    // overhead because of that.
    //
    // NB: bfd's strip command cannot do the above and will corrupt the
    //     binary during the process of stripping non-allocatable sections.
    if (NextAvailableOffset <= NextAvailableAddress - FirstAllocAddress) {
      NextAvailableOffset = NextAvailableAddress - FirstAllocAddress;
    } else {
      NextAvailableAddress = NextAvailableOffset + FirstAllocAddress;
    }
    assert(NextAvailableOffset == NextAvailableAddress - FirstAllocAddress &&
           "PHDR table address calculation error");

    outs() << "BOLT-INFO: creating new program header table at address 0x"
           << Twine::utohexstr(NextAvailableAddress) << ", offset 0x"
           << Twine::utohexstr(NextAvailableOffset) << '\n';

    PHDRTableAddress = NextAvailableAddress;
    PHDRTableOffset = NextAvailableOffset;

    // Reserve space for 3 extra pheaders.
    unsigned Phnum = Obj->getHeader()->e_phnum;
    Phnum += 3;

    NextAvailableAddress += Phnum * sizeof(ELFFile<ELF64LE>::Elf_Phdr);
    NextAvailableOffset  += Phnum * sizeof(ELFFile<ELF64LE>::Elf_Phdr);
  }

  // Align at cache line.
  NextAvailableAddress = RoundUpToAlignment(NextAvailableAddress, 64);
  NextAvailableOffset = RoundUpToAlignment(NextAvailableOffset, 64);

  NewTextSegmentAddress = NextAvailableAddress;
  NewTextSegmentOffset = NextAvailableOffset;
}

void RewriteInstance::run() {
  if (!BC) {
    errs() << "BOLT-ERROR: failed to create a binary context\n";
    return;
  }

  unsigned PassNumber = 1;

  // Main "loop".
  discoverStorage();
  readSpecialSections();
  discoverFileObjects();
  readDebugInfo();
  disassembleFunctions();
  readFunctionDebugInfo();
  runOptimizationPasses();
  emitFunctions();

  if (opts::SplitFunctions == BinaryFunction::ST_LARGE &&
      checkLargeFunctions()) {
    ++PassNumber;
    // Emit again because now some functions have been split
    outs() << "BOLT: split-functions: starting pass " << PassNumber << "...\n";
    reset();
    discoverStorage();
    readSpecialSections();
    discoverFileObjects();
    readDebugInfo();
    disassembleFunctions();
    readFunctionDebugInfo();
    runOptimizationPasses();
    emitFunctions();
  }

  // Emit functions again ignoring functions which still didn't fit in their
  // original space, so that we don't generate incorrect debugging information
  // for them (information that would reflect the optimized version).
  if (opts::UpdateDebugSections && opts::FixDebugInfoLargeFunctions &&
      checkLargeFunctions()) {
    ++PassNumber;
    outs() << "BOLT: starting pass (ignoring large functions) "
           << PassNumber << "...\n";
    reset();
    discoverStorage();
    readSpecialSections();
    discoverFileObjects();
    readDebugInfo();
    disassembleFunctions();

    for (uint64_t Address : LargeFunctions) {
      auto FunctionIt = BinaryFunctions.find(Address);
      assert(FunctionIt != BinaryFunctions.end() &&
             "Invalid large function address.");
      if (opts::Verbosity >= 1) {
        errs() << "BOLT-WARNING: Function " << FunctionIt->second
               << " is larger than its orginal size: emitting again marking it "
               << "as not simple.\n";
      }
      FunctionIt->second.setSimple(false);
    }

    readFunctionDebugInfo();
    runOptimizationPasses();
    emitFunctions();
  }

  if (opts::UpdateDebugSections)
    updateDebugInfo();

  // Copy allocatable part of the input.
  std::error_code EC;
  Out = llvm::make_unique<tool_output_file>(opts::OutputFilename, EC,
                                            sys::fs::F_None, 0777);
  check_error(EC, "cannot create output executable file");
  Out->os() << InputFile->getData().substr(0, FirstNonAllocatableOffset);

  // Rewrite allocatable contents and copy non-allocatable parts with mods.
  rewriteFile();
}

void RewriteInstance::discoverFileObjects() {
  std::string FileSymbolName;
  bool SeenFileName = false;

  FileSymRefs.clear();
  BinaryFunctions.clear();
  BC->GlobalAddresses.clear();

  // For local symbols we want to keep track of associated FILE symbol for
  // disambiguation by name.
  for (const SymbolRef &Symbol : InputFile->symbols()) {
    // Keep undefined symbols for pretty printing?
    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    ErrorOr<StringRef> NameOrError = Symbol.getName();
    check_error(NameOrError.getError(), "cannot get symbol name");

    if (Symbol.getType() == SymbolRef::ST_File) {
      // Could be used for local symbol disambiguation.
      FileSymbolName = *NameOrError;
      SeenFileName = true;
      continue;
    }

    ErrorOr<uint64_t> AddressOrErr = Symbol.getAddress();
    check_error(AddressOrErr.getError(), "cannot get symbol address");
    uint64_t Address = *AddressOrErr;
    if (Address == 0) {
      if (opts::Verbosity >= 1 && Symbol.getType() == SymbolRef::ST_Function)
        errs() << "BOLT-WARNING: function with 0 address seen\n";
      continue;
    }

    FileSymRefs[Address] = Symbol;

    // There's nothing horribly wrong with anonymous symbols, but let's
    // ignore them for now.
    if (NameOrError->empty())
      continue;

    /// It is possible we are seeing a globalized local. LLVM might treat it as
    /// a local if it has a "private global" prefix, e.g. ".L". Thus we have to
    /// change the prefix to enforce global scope of the symbol.
    std::string Name =
      NameOrError->startswith(BC->AsmInfo->getPrivateGlobalPrefix())
        ? "PG." + std::string(*NameOrError)
        : std::string(*NameOrError);

    // Disambiguate all local symbols before adding to symbol table.
    // Since we don't know if we will see a global with the same name,
    // always modify the local name.
    //
    // NOTE: the naming convention for local symbols should match
    //       the one we use for profile data.
    std::string UniqueName;
    std::string AlternativeName;
    if (Symbol.getFlags() & SymbolRef::SF_Global) {
      assert(BC->GlobalSymbols.find(Name) == BC->GlobalSymbols.end() &&
             "global name not unique");
      UniqueName = Name;
    } else {
      // If we have a local file name, we should create 2 variants for the
      // function name. The reason is that perf profile might have been
      // collected on a binary that did not have the local file name (e.g. as
      // a side effect of stripping debug info from the binary):
      //
      //   primary:     <function>/<id>
      //   alternative: <function>/<file>/<id2>
      //
      // The <id> field is used for disambiguation of local symbols since there
      // could be identical function names coming from identical file names
      // (e.g. from different directories).
      std::string Prefix = Name + "/";
      std::string AltPrefix;
      if (!FileSymbolName.empty())
        AltPrefix = Prefix + FileSymbolName + "/";

      auto uniquifyName = [&] (std::string NamePrefix) {
        unsigned LocalID = 1;
        while (BC->GlobalSymbols.find(NamePrefix + std::to_string(LocalID))
               != BC->GlobalSymbols.end())
          ++LocalID;
        return NamePrefix + std::to_string(LocalID);
      };
      UniqueName = uniquifyName(Prefix);
      if (!AltPrefix.empty())
        AlternativeName = uniquifyName(AltPrefix);
    }

    BC->registerNameAtAddress(UniqueName, Address);
    if (!AlternativeName.empty())
      BC->registerNameAtAddress(AlternativeName, Address);

    // Only consider ST_Function symbols for functions. Although this
    // assumption  could be broken by assembly functions for which the type
    // could be wrong, we skip such entries till the support for
    // assembly is implemented.
    if (Symbol.getType() != SymbolRef::ST_Function)
      continue;

    // TODO: populate address map with PLT entries for better readability.

    // Ignore function with 0 size for now (possibly coming from assembly).
    auto SymbolSize = ELFSymbolRef(Symbol).getSize();
    if (SymbolSize == 0)
      continue;

    ErrorOr<section_iterator> SectionOrErr = Symbol.getSection();
    check_error(SectionOrErr.getError(), "cannot get symbol section");
    section_iterator Section = *SectionOrErr;
    if (Section == InputFile->section_end()) {
      // Could be an absolute symbol. Could record for pretty printing.
      continue;
    }

    // Checkout for conflicts with function data from FDEs.
    bool IsSimple = true;
    auto FDEI = CFIRdWrt->getFDEs().lower_bound(Address);
    if (FDEI != CFIRdWrt->getFDEs().end()) {
      auto &FDE = *FDEI->second;
      if (FDEI->first != Address) {
        // There's no matching starting address in FDE. Make sure the previous
        // FDE does not contain this address.
        if (FDEI != CFIRdWrt->getFDEs().begin()) {
          --FDEI;
          auto &PrevFDE = *FDEI->second;
          auto PrevStart = PrevFDE.getInitialLocation();
          auto PrevLength = PrevFDE.getAddressRange();
          if (opts::Verbosity >= 1 &&
              Address > PrevStart && Address < PrevStart + PrevLength) {
            errs() << "BOLT-WARNING: function " << UniqueName
                   << " is in conflict with FDE ["
                   << Twine::utohexstr(PrevStart) << ", "
                   << Twine::utohexstr(PrevStart + PrevLength)
                   << "). Skipping.\n";
            IsSimple = false;
          }
        }
      } else if (FDE.getAddressRange() != SymbolSize) {
        // Function addresses match but sizes differ.
        if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: sizes differ for function " << UniqueName
                 << ". FDE : " << FDE.getAddressRange()
                 << "; symbol table : " << SymbolSize << ". Skipping.\n";
        }

        // Create maximum size non-simple function.
        IsSimple = false;
        SymbolSize = std::max(SymbolSize, FDE.getAddressRange());
      }
    }

    BinaryFunction *BF{nullptr};
    auto BFI = BinaryFunctions.find(Address);
    if (BFI != BinaryFunctions.end()) {
      BF = &BFI->second;
      // Duplicate function name. Make sure everything matches before we add
      // an alternative name.
      if (opts::Verbosity >= 1 && SymbolSize != BF->getSize()) {
        errs() << "BOLT-WARNING: size mismatch for duplicate entries "
               << UniqueName << ':' << SymbolSize << " and "
               << *BF << ':' << BF->getSize() << '\n';
      }
      BF->addAlternativeName(UniqueName);
    } else {
      BF = createBinaryFunction(UniqueName, *Section, Address, SymbolSize,
                                IsSimple);
    }
    if (!AlternativeName.empty())
      BF->addAlternativeName(AlternativeName);
  }

  if (!SeenFileName && BC->DR.hasLocalsWithFileName() && !opts::AllowStripped) {
    errs() << "BOLT-ERROR: input binary does not have local file symbols "
              "but profile data includes function names with embedded file "
              "names. It appears that the input binary was stripped while a "
              "profiled binary was not. If you know what you are doing and "
              "wish to proceed, use -allow-stripped option.\n";
    exit(1);
  }
}

BinaryFunction *RewriteInstance::createBinaryFunction(
    const std::string &Name, SectionRef Section, uint64_t Address,
    uint64_t Size, bool IsSimple) {
  auto Result = BinaryFunctions.emplace(
      Address, BinaryFunction(Name, Section, Address, Size, *BC, IsSimple));
  assert(Result.second == true && "unexpected duplicate function");
  auto *BF = &Result.first->second;
  BC->SymbolToFunctionMap[BF->getSymbol()] = BF;
  return BF;
}

void RewriteInstance::readSpecialSections() {
  // Process special sections.
  StringRef FrameHdrContents;
  for (const auto &Section : InputFile->sections()) {
    StringRef SectionName;
    check_error(Section.getName(SectionName), "cannot get section name");
    StringRef SectionContents;
    check_error(Section.getContents(SectionContents),
                "cannot get section contents");
    ArrayRef<uint8_t> SectionData(
        reinterpret_cast<const uint8_t *>(SectionContents.data()),
        Section.getSize());

    if (SectionName == ".gcc_except_table") {
      LSDAData = SectionData;
      LSDAAddress = Section.getAddress();
    } else if (SectionName == ".eh_frame_hdr") {
      FrameHdrAddress = Section.getAddress();
      FrameHdrContents = SectionContents;
      FrameHdrAlign = Section.getAlignment();
    } else if (SectionName == ".debug_loc") {
      DebugLocSize = Section.getSize();
    }

    // Ignore zero-size allocatable sections as they present no interest to us.
    if ((Section.isText() || Section.isData() || Section.isBSS()) &&
        Section.getSize() > 0) {
      BC->AllocatableSections.emplace(std::make_pair(Section.getAddress(),
                                                     Section));
    }
  }

  FrameHdrCopy =
      std::vector<char>(FrameHdrContents.begin(), FrameHdrContents.end());
  // Process debug sections.
  EHFrame = BC->DwCtx->getEHFrame();
  if (opts::DumpEHFrame) {
    EHFrame->dump(outs());
  }
  CFIRdWrt.reset(new CFIReaderWriter(*EHFrame, FrameHdrAddress, FrameHdrCopy));
  if (!EHFrame->ParseError.empty()) {
    errs() << "BOLT-ERROR: EHFrame reader failed with message \""
           << EHFrame->ParseError << "\"\n";
    exit(1);
  }
}

void RewriteInstance::readDebugInfo() {
  if (!opts::UpdateDebugSections)
    return;

  BC->preprocessDebugInfo(BinaryFunctions);
}

void RewriteInstance::readFunctionDebugInfo() {
  if (!opts::UpdateDebugSections)
    return;

  BC->preprocessFunctionDebugInfo(BinaryFunctions);
}

void RewriteInstance::disassembleFunctions() {
  // Disassemble every function and build it's control flow graph.
  TotalScore = 0;
  for (auto &BFI : BinaryFunctions) {
    BinaryFunction &Function = BFI.second;

    if (!opts::shouldProcess(Function)) {
      DEBUG(dbgs() << "BOLT: skipping processing function "
                   << Function << " per user request.\n");
      continue;
    }

    SectionRef Section = Function.getSection();
    assert(Section.getAddress() <= Function.getAddress() &&
           Section.getAddress() + Section.getSize()
             >= Function.getAddress() + Function.getSize() &&
          "wrong section for function");
    if (!Section.isText() || Section.isVirtual() || !Section.getSize()) {
      // When could it happen?
      if (opts::Verbosity >= 1) {
        errs() << "BOLT-WARNING: corresponding section is non-executable or empty "
               << "for function " << Function;
      }
      continue;
    }

    // Set the proper maximum size value after the whole symbol table
    // has been processed.
    auto SymRefI = FileSymRefs.upper_bound(Function.getAddress());
    if (SymRefI != FileSymRefs.end()) {
      uint64_t MaxSize;
      auto SectionIter = *SymRefI->second.getSection();
      if (SectionIter != InputFile->section_end() &&
          *SectionIter == Function.getSection()) {
        MaxSize = SymRefI->first - Function.getAddress();
      } else {
        // Function runs till the end of the containing section assuming
        // the section does not run over the next symbol.
        uint64_t SectionEnd = Function.getSection().getAddress() +
                              Function.getSection().getSize();
        if (SectionEnd > SymRefI->first) {
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: symbol after " << Function
                   << " should not be in the same section.\n";
          }
          MaxSize = 0;
        } else {
          MaxSize = SectionEnd - Function.getAddress();
        }
      }

      if (MaxSize < Function.getSize()) {
        if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: symbol seen in the middle of the function "
                 << Function << ". Skipping.\n";
        }
        Function.setSimple(false);
        continue;
      }
      Function.setMaxSize(MaxSize);
    }

    StringRef SectionContents;
    check_error(Section.getContents(SectionContents),
                "cannot get section contents");

    assert(SectionContents.size() == Section.getSize() &&
           "section size mismatch");

    // Function offset from the section start.
    auto FunctionOffset = Function.getAddress() - Section.getAddress();

    // Offset of the function in the file.
    Function.setFileOffset(
        SectionContents.data() - InputFile->getData().data() + FunctionOffset);

    ArrayRef<uint8_t> FunctionData(
        reinterpret_cast<const uint8_t *>
          (SectionContents.data()) + FunctionOffset,
        Function.getSize());

    if (!Function.disassemble(FunctionData))
      continue;

    if (opts::PrintAll || opts::PrintDisasm)
      Function.print(outs(), "after disassembly", true);

    if (!Function.isSimple())
      continue;

    // Fill in CFI information for this function
    if (EHFrame->ParseError.empty()) {
      if (!CFIRdWrt->fillCFIInfoFor(Function)) {
        if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: unable to fill CFI for function "
                 << Function << '\n';
        }
        Function.setSimple(false);
        continue;
      }
    }

    // Parse LSDA.
    if (Function.getLSDAAddress() != 0)
      Function.parseLSDA(LSDAData, LSDAAddress);

    if (!Function.buildCFG())
      continue;

    if (opts::PrintAll || opts::PrintCFG)
      Function.print(outs(), "after building cfg", true);

    if (opts::DumpDotAll)
      Function.dumpGraphForPass("build-cfg");

    if (opts::PrintLoopInfo) {
      Function.calculateLoopInfo();
      Function.printLoopInfo(outs());
    }

    TotalScore += Function.getFunctionScore();

  } // Iterate over all functions

  // Mark all functions with internal addresses serving as interprocedural
  // reference as not simple.
  // TODO: #9301815
  for (auto Addr : BC->InterproceduralReferences) {
    auto *ContainingFunction = getBinaryFunctionContainingAddress(Addr);
    if (ContainingFunction && ContainingFunction->getAddress() != Addr) {
      if (opts::Verbosity >= 1) {
        errs() << "BOLT-WARNING: Function " << ContainingFunction
               << " has internal BBs that are target of a reference located in "
               << "another function. Skipping the function.\n";
      }
      ContainingFunction->setSimple(false);
    }
  }

  uint64_t NumSimpleFunctions{0};
  uint64_t NumStaleProfileFunctions{0};
  std::vector<BinaryFunction *> ProfiledFunctions;
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;
    if (!Function.isSimple())
      continue;
    ++NumSimpleFunctions;
    if (Function.getExecutionCount() == BinaryFunction::COUNT_NO_PROFILE)
      continue;
    if (Function.hasValidProfile())
      ProfiledFunctions.push_back(&Function);
    else
      ++NumStaleProfileFunctions;
  }

  outs() << "BOLT-INFO: "
         << ProfiledFunctions.size() + NumStaleProfileFunctions
         << " functions out of " << NumSimpleFunctions << " simple functions ("
         << format("%.1f",
                   (ProfiledFunctions.size() + NumStaleProfileFunctions) /
                   (float) NumSimpleFunctions * 100.0f)
         << "%) have non-empty execution profile.\n";
  if (NumStaleProfileFunctions) {
    outs() << "BOLT-INFO: " << NumStaleProfileFunctions
           << format(" (%.1f%) ", NumStaleProfileFunctions /
                                  (float) NumSimpleFunctions * 100.0f)
           << " function" << (NumStaleProfileFunctions == 1 ? "" : "s")
           << " have invalid (possibly stale) profile.\n";
  }

  if (ProfiledFunctions.size() > 10) {
    if (opts::Verbosity >= 1) {
      outs() << "BOLT-INFO: top called functions are:\n";
      std::sort(ProfiledFunctions.begin(), ProfiledFunctions.end(),
                [](BinaryFunction *A, BinaryFunction *B) {
                  return B->getExecutionCount() < A->getExecutionCount();
                }
                );
      auto SFI = ProfiledFunctions.begin();
      for (int i = 0; i < 100 && SFI != ProfiledFunctions.end(); ++SFI, ++i) {
        outs() << "  " << **SFI << " : "
               << (*SFI)->getExecutionCount() << '\n';
      }
    }
  }
}

void RewriteInstance::runOptimizationPasses() {
  // Run optimization passes.
  //
  BinaryFunctionPassManager::runAllPasses(*BC, BinaryFunctions, LargeFunctions);
}

namespace {

// Helper function to emit the contents of a function via a MCStreamer object.
void emitFunction(MCStreamer &Streamer, BinaryFunction &Function,
                  BinaryContext &BC, bool EmitColdPart) {
  // Define a helper to decode and emit CFI instructions at a given point in a
  // BB
  auto emitCFIInstr = [&Streamer](const MCCFIInstruction &CFIInstr) {
    switch (CFIInstr.getOperation()) {
    default:
      llvm_unreachable("Unexpected instruction");
    case MCCFIInstruction::OpDefCfaOffset:
      Streamer.EmitCFIDefCfaOffset(CFIInstr.getOffset());
      break;
    case MCCFIInstruction::OpAdjustCfaOffset:
      Streamer.EmitCFIAdjustCfaOffset(CFIInstr.getOffset());
      break;
    case MCCFIInstruction::OpDefCfa:
      Streamer.EmitCFIDefCfa(CFIInstr.getRegister(), CFIInstr.getOffset());
      break;
    case MCCFIInstruction::OpDefCfaRegister:
      Streamer.EmitCFIDefCfaRegister(CFIInstr.getRegister());
      break;
    case MCCFIInstruction::OpOffset:
      Streamer.EmitCFIOffset(CFIInstr.getRegister(), CFIInstr.getOffset());
      break;
    case MCCFIInstruction::OpRegister:
      Streamer.EmitCFIRegister(CFIInstr.getRegister(),
                                CFIInstr.getRegister2());
      break;
    case MCCFIInstruction::OpRelOffset:
      Streamer.EmitCFIRelOffset(CFIInstr.getRegister(), CFIInstr.getOffset());
      break;
    case MCCFIInstruction::OpUndefined:
      Streamer.EmitCFIUndefined(CFIInstr.getRegister());
      break;
    case MCCFIInstruction::OpRememberState:
      Streamer.EmitCFIRememberState();
      break;
    case MCCFIInstruction::OpRestoreState:
      Streamer.EmitCFIRestoreState();
      break;
    case MCCFIInstruction::OpRestore:
      Streamer.EmitCFIRestore(CFIInstr.getRegister());
      break;
    case MCCFIInstruction::OpSameValue:
      Streamer.EmitCFISameValue(CFIInstr.getRegister());
      break;
    case MCCFIInstruction::OpGnuArgsSize:
      Streamer.EmitCFIGnuArgsSize(CFIInstr.getOffset());
      break;
    }
  };

  // No need for human readability?
  // FIXME: what difference does it make in reality?
  // Ctx.setUseNamesOnTempLabels(false);

  // Emit function start

  // Each fuction is emmitted into its own section.
  MCSectionELF *FunctionSection =
      EmitColdPart
          ? BC.Ctx->getELFSection(
                Function.getCodeSectionName().str().append(".cold"),
                ELF::SHT_PROGBITS, ELF::SHF_EXECINSTR | ELF::SHF_ALLOC)
          : BC.Ctx->getELFSection(Function.getCodeSectionName(),
                                  ELF::SHT_PROGBITS,
                                  ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);

  MCSection *Section = FunctionSection;

  Section->setHasInstructions(true);
  BC.Ctx->addGenDwarfSection(Section);

  Streamer.SwitchSection(Section);

  Streamer.EmitCodeAlignment(Function.getAlignment());

  // Emit all names the function is known under.
  for (const auto &Name : Function.getNames()) {
    Twine EmitName = EmitColdPart ? Twine(Name).concat(".cold") : Name;
    auto *EmitSymbol = BC.Ctx->getOrCreateSymbol(EmitName);
    Streamer.EmitSymbolAttribute(EmitSymbol, MCSA_ELF_TypeFunction);
    Streamer.EmitLabel(EmitSymbol);
  }

  // Emit CFI start
  if (Function.hasCFI()) {
    Streamer.EmitCFIStartProc(/*IsSimple=*/false);
    if (Function.getPersonalityFunction() != nullptr) {
      Streamer.EmitCFIPersonality(Function.getPersonalityFunction(),
                                  Function.getPersonalityEncoding());
    }
    if (!EmitColdPart && Function.getLSDASymbol()) {
      Streamer.EmitCFILsda(Function.getLSDASymbol(),
                           BC.MOFI->getLSDAEncoding());
    } else {
      Streamer.EmitCFILsda(0, dwarf::DW_EH_PE_omit);
    }
    // Emit CFI instructions relative to the CIE
    for (auto &CFIInstr : Function.cie()) {
      // Ignore these CIE CFI insns because LLVM will already emit this.
      switch (CFIInstr.getOperation()) {
      default:
        break;
      case MCCFIInstruction::OpDefCfa:
        if (CFIInstr.getRegister() == 7 && CFIInstr.getOffset() == 8)
          continue;
        break;
      case MCCFIInstruction::OpOffset:
        if (CFIInstr.getRegister() == 16 && CFIInstr.getOffset() == -8)
          continue;
        break;
      }
      emitCFIInstr(CFIInstr);
    }
  }

  assert(!(*Function.begin()).isCold() &&
         "first basic block should never be cold");

  // Emit UD2 at the beginning if requested by user.
  if (!opts::BreakFunctionNames.empty()) {
    for (auto &Name : opts::BreakFunctionNames) {
      if (Function.hasName(Name)) {
        Streamer.EmitIntValue(0x0B0F, 2); // UD2: 0F 0B
        break;
      }
    }
  }

  // Emit code.
  auto ULT = Function.getDWARFUnitLineTable();
  int64_t CurrentGnuArgsSize = 0;
  for (auto BB : Function.layout()) {
    if (EmitColdPart != BB->isCold())
      continue;
    if (opts::AlignBlocks && BB->getAlignment() > 1)
      Streamer.EmitCodeAlignment(BB->getAlignment());
    Streamer.EmitLabel(BB->getLabel());
    // Remember last .debug_line entry emitted so that we don't repeat them in
    // subsequent instructions, as gdb can figure it out by looking at the
    // previous instruction with available line number info.
    SMLoc LastLocSeen;

    for (const auto &Instr : *BB) {
      // Handle pseudo instructions.
      if (BC.MIA->isEHLabel(Instr)) {
        assert(Instr.getNumOperands() == 1 && Instr.getOperand(0).isExpr() &&
               "bad EH_LABEL instruction");
        auto Label = &(cast<MCSymbolRefExpr>(Instr.getOperand(0).getExpr())
                           ->getSymbol());
        Streamer.EmitLabel(const_cast<MCSymbol *>(Label));
        continue;
      }
      if (BC.MIA->isCFI(Instr)) {
        emitCFIInstr(*Function.getCFIFor(Instr));
        continue;
      }
      if (opts::UpdateDebugSections) {
        auto RowReference = DebugLineTableRowRef::fromSMLoc(Instr.getLoc());
        if (RowReference != DebugLineTableRowRef::NULL_ROW &&
            Instr.getLoc().getPointer() != LastLocSeen.getPointer()) {
          auto Unit = ULT.first;
          auto OriginalLineTable = ULT.second;
          const auto OrigUnitID = Unit->getOffset();
          unsigned NewFilenum = 0;

          // If the CU id from the current instruction location does not
          // match the CU id from the current function, it means that we
          // have come across some inlined code.  We must look up the CU
          // for the instruction's original function and get the line table
          // from that.  We also update the current CU debug info with the
          // filename of the inlined function.
          if (RowReference.DwCompileUnitIndex != OrigUnitID) {
            Unit =
              BC.DwCtx->getCompileUnitForOffset(RowReference.DwCompileUnitIndex);
            OriginalLineTable = BC.DwCtx->getLineTableForUnit(Unit);
            const auto Filenum =
              OriginalLineTable->Rows[RowReference.RowIndex - 1].File;
            NewFilenum =
              BC.addDebugFilenameToUnit(OrigUnitID,
                                        RowReference.DwCompileUnitIndex,
                                        Filenum);
          }

          assert(Unit && OriginalLineTable &&
                 "Invalid CU offset set in instruction debug info.");

          const auto &OriginalRow =
            OriginalLineTable->Rows[RowReference.RowIndex - 1];

          BC.Ctx->setCurrentDwarfLoc(
            NewFilenum == 0 ? OriginalRow.File : NewFilenum,
            OriginalRow.Line,
            OriginalRow.Column,
            (DWARF2_FLAG_IS_STMT * OriginalRow.IsStmt) |
            (DWARF2_FLAG_BASIC_BLOCK * OriginalRow.BasicBlock) |
            (DWARF2_FLAG_PROLOGUE_END * OriginalRow.PrologueEnd) |
            (DWARF2_FLAG_EPILOGUE_BEGIN * OriginalRow.EpilogueBegin),
            OriginalRow.Isa,
            OriginalRow.Discriminator);
          BC.Ctx->setDwarfCompileUnitID(OrigUnitID);
          LastLocSeen = Instr.getLoc();
        }
      }

      // Emit GNU_args_size CFIs as necessary.
      if (Function.usesGnuArgsSize() && BC.MIA->isInvoke(Instr)) {
        auto NewGnuArgsSize = BC.MIA->getGnuArgsSize(Instr);
        if (NewGnuArgsSize >= 0 && NewGnuArgsSize != CurrentGnuArgsSize) {
          CurrentGnuArgsSize = NewGnuArgsSize;
          Streamer.EmitCFIGnuArgsSize(CurrentGnuArgsSize);
        }
      }

      Streamer.EmitInstruction(Instr, *BC.STI);
    }

    MCSymbol *BBEndLabel = BC.Ctx->createTempSymbol();
    BB->setEndLabel(BBEndLabel);
    Streamer.EmitLabel(BBEndLabel);
  }

  // Emit CFI end
  if (Function.hasCFI())
    Streamer.EmitCFIEndProc();

  if (!EmitColdPart && Function.getFunctionEndLabel())
    Streamer.EmitLabel(Function.getFunctionEndLabel());

  // Emit LSDA before anything else?
  if (!EmitColdPart)
    Function.emitLSDA(&Streamer);

  // TODO: is there any use in emiting end of function?
  //       Perhaps once we have a support for C++ exceptions.
  // auto FunctionEndLabel = Ctx.createTempSymbol("func_end");
  // Streamer.EmitLabel(FunctionEndLabel);
  // Streamer.emitELFSize(FunctionSymbol, MCExpr());
}

template <typename T>
std::vector<T> singletonSet(T t) {
  std::vector<T> Vec;
  Vec.push_back(std::move(t));
  return Vec;
}

} // anonymous namespace

void RewriteInstance::emitFunctions() {
  std::error_code EC;

  // This is an object file, which we keep for debugging purposes.
  // Once we decide it's useless, we should create it in memory.
  std::unique_ptr<tool_output_file> TempOut =
    llvm::make_unique<tool_output_file>(opts::OutputFilename + ".bolt.o",
                                        EC, sys::fs::F_None);
  check_error(EC, "cannot create output object file");

  std::unique_ptr<buffer_ostream> BOS =
      make_unique<buffer_ostream>(TempOut->os());
  raw_pwrite_stream *OS = BOS.get();

  // Implicitly MCObjectStreamer takes ownership of MCAsmBackend (MAB)
  // and MCCodeEmitter (MCE). ~MCObjectStreamer() will delete these
  // two instances.
  auto MCE = BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  auto MAB = BC->TheTarget->createMCAsmBackend(*BC->MRI, BC->TripleName, "");
  std::unique_ptr<MCStreamer> Streamer(
    BC->TheTarget->createMCObjectStreamer(*BC->TheTriple,
                                          *BC->Ctx,
                                          *MAB,
                                          *OS,
                                          MCE,
                                          *BC->STI,
                                          /* RelaxAll */ false,
                                          /* DWARFMustBeAtTheEnd */ false));

  Streamer->InitSections(false);

  // Output functions one by one.
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (!Function.isSimple())
      continue;

    if (!opts::shouldProcess(Function))
      continue;

    DEBUG(dbgs() << "BOLT: generating code for function \""
                 << Function << "\" : "
                 << Function.getFunctionNumber() << '\n');

    emitFunction(*Streamer, Function, *BC.get(), /*EmitColdPart=*/false);

    if (Function.isSplit())
      emitFunction(*Streamer, Function, *BC.get(), /*EmitColdPart=*/true);
  }

  if (opts::UpdateDebugSections)
    updateDebugLineInfoForNonSimpleFunctions();

  Streamer->Finish();

  //////////////////////////////////////////////////////////////////////////////
  // Assign addresses to new functions/sections.
  //////////////////////////////////////////////////////////////////////////////

  auto EFMM = new ExecutableFileMemoryManager();
  SectionMM.reset(EFMM);

  if (opts::UpdateDebugSections) {
    // Compute offsets of tables in .debug_line for each compile unit.
    updateLineTableOffsets();
  }

  // Get output object as ObjectFile.
  std::unique_ptr<MemoryBuffer> ObjectMemBuffer =
      MemoryBuffer::getMemBuffer(BOS->str(), "in-memory object file", false);
  ErrorOr<std::unique_ptr<object::ObjectFile>> ObjOrErr =
    object::ObjectFile::createObjectFile(ObjectMemBuffer->getMemBufferRef());
  check_error(ObjOrErr.getError(), "error creating in-memory object");

  // Run ObjectLinkingLayer() with custom memory manager and symbol resolver.
  orc::ObjectLinkingLayer<> OLT;

  auto Resolver = orc::createLambdaResolver(
          [&](const std::string &Name) {
            DEBUG(dbgs() << "BOLT: looking for " << Name << "\n");
            auto I = BC->GlobalSymbols.find(Name);
            if (I == BC->GlobalSymbols.end())
              return RuntimeDyld::SymbolInfo(nullptr);
            return RuntimeDyld::SymbolInfo(I->second,
                                           JITSymbolFlags::None);
          },
          [](const std::string &S) {
            DEBUG(dbgs() << "BOLT: resolving " << S << "\n");
            return nullptr;
          }
      );
  auto ObjectsHandle = OLT.addObjectSet(
        singletonSet(std::move(ObjOrErr.get())),
        SectionMM.get(),
        std::move(Resolver),
        /* ProcessAllSections = */true);

  // FIXME: use notifyObjectLoaded() to remap sections.

  // Map every function/section current address in memory to that in
  // the output binary.
  uint64_t NewTextSectionStartAddress = NextAvailableAddress;
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;
    if (!Function.isSimple())
      continue;

    auto TooLarge = false;
    auto SMII = EFMM->SectionMapInfo.find(Function.getCodeSectionName());
    if (SMII != EFMM->SectionMapInfo.end()) {
      DEBUG(dbgs() << "BOLT: mapping 0x"
                   << Twine::utohexstr(SMII->second.AllocAddress)
                   << " to 0x" << Twine::utohexstr(Function.getAddress())
                   << '\n');
      OLT.mapSectionAddress(ObjectsHandle,
                            SMII->second.SectionID,
                            Function.getAddress());
      Function.setImageAddress(SMII->second.AllocAddress);
      Function.setImageSize(SMII->second.Size);
      if (Function.getImageSize() > Function.getMaxSize()) {
        TooLarge = true;
        FailedAddresses.emplace_back(Function.getAddress());
      }
    } else {
      if (opts::Verbosity >= 2) {
        errs() << "BOLT-WARNING: cannot remap function " << Function << "\n";
      }
      FailedAddresses.emplace_back(Function.getAddress());
    }

    if (!Function.isSplit())
      continue;

    SMII = EFMM->SectionMapInfo.find(
        Function.getCodeSectionName().str().append(".cold"));
    if (SMII != EFMM->SectionMapInfo.end()) {
      // Cold fragments are aligned at 16 bytes.
      NextAvailableAddress = RoundUpToAlignment(NextAvailableAddress, 16);
      DEBUG(dbgs() << "BOLT: mapping 0x"
                   << Twine::utohexstr(SMII->second.AllocAddress)
                   << " to 0x" << Twine::utohexstr(NextAvailableAddress)
                   << " with size " << Twine::utohexstr(SMII->second.Size)
                   << '\n');
      OLT.mapSectionAddress(ObjectsHandle,
                            SMII->second.SectionID,
                            NextAvailableAddress);
      Function.cold().setAddress(NextAvailableAddress);
      Function.cold().setImageAddress(SMII->second.AllocAddress);
      Function.cold().setImageSize(TooLarge ? 0 : SMII->second.Size);
      Function.cold().setFileOffset(getFileOffsetFor(NextAvailableAddress));

      NextAvailableAddress += Function.cold().getImageSize();
    } else {
      if (opts::Verbosity >= 2) {
        errs() << "BOLT-WARNING: cannot remap function " << Function << "\n";
      }
      FailedAddresses.emplace_back(Function.getAddress());
    }
  }

  // Add the new text section aggregating all existing code sections.
  auto NewTextSectionSize = NextAvailableAddress - NewTextSectionStartAddress;
  if (NewTextSectionSize) {
    SectionMM->SectionMapInfo[".bolt.text"] =
        SectionInfo(0,
                    NewTextSectionSize,
                    16,
                    true /*IsCode*/,
                    true /*IsReadOnly*/,
                    NewTextSectionStartAddress,
                    getFileOffsetFor(NewTextSectionStartAddress));
  }

  // Map special sections to their addresses in the output image.
  //
  // TODO: perhaps we should process all the allocated sections here?
  std::vector<std::string> Sections = { ".eh_frame", ".gcc_except_table" };
  for (auto &SectionName : Sections) {
    auto SMII = EFMM->SectionMapInfo.find(SectionName);
    if (SMII != EFMM->SectionMapInfo.end()) {
      SectionInfo &SI = SMII->second;
      NextAvailableAddress = RoundUpToAlignment(NextAvailableAddress,
                                                SI.Alignment);
      DEBUG(dbgs() << "BOLT: mapping 0x"
                   << Twine::utohexstr(SI.AllocAddress)
                   << " to 0x" << Twine::utohexstr(NextAvailableAddress)
                   << '\n');

      OLT.mapSectionAddress(ObjectsHandle,
                            SI.SectionID,
                            NextAvailableAddress);
      SI.FileAddress = NextAvailableAddress;
      SI.FileOffset = getFileOffsetFor(NextAvailableAddress);

      NextAvailableAddress += SI.Size;
    } else {
      if (opts::Verbosity >= 2) {
        errs() << "BOLT-WARNING: cannot remap " << SectionName << '\n';
      }
    }
  }

  if (opts::UpdateDebugSections) {
    MCAsmLayout Layout(
        static_cast<MCObjectStreamer *>(Streamer.get())->getAssembler());

    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;
      for (auto &BB : Function) {
        if (!(BB.getLabel()->isDefined(false) &&
              BB.getEndLabel() && BB.getEndLabel()->isDefined(false))) {
          continue;
        }
        uint64_t BaseAddress = (BB.isCold() ? Function.cold().getAddress()
                                            : Function.getAddress());
        uint64_t BeginAddress =
            BaseAddress + Layout.getSymbolOffset(*BB.getLabel());
        uint64_t EndAddress =
            BaseAddress + Layout.getSymbolOffset(*BB.getEndLabel());
        BB.setOutputAddressRange(std::make_pair(BeginAddress, EndAddress));
      }
    }
  }

  OLT.emitAndFinalize(ObjectsHandle);

  if (opts::KeepTmp)
    TempOut->keep();
}

bool RewriteInstance::checkLargeFunctions() {
  LargeFunctions.clear();
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    // Ignore this function if we failed to map it to the output binary
    if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
      continue;

    if (Function.getImageSize() <= Function.getMaxSize())
      continue;

    LargeFunctions.insert(BFI.first);
  }
  return !LargeFunctions.empty();
}

void RewriteInstance::patchELFPHDRTable() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  auto &OS = Out->os();

  // Write/re-write program headers.
  Phnum = Obj->getHeader()->e_phnum;
  if (PHDRTableOffset) {
    // Writing new pheader table.
    Phnum += 1; // only adding one new segment
    // Segment size includes the size of the PHDR area.
    NewTextSegmentSize = NextAvailableAddress - PHDRTableAddress;
  } else {
    assert(!PHDRTableAddress && "unexpected address for program header table");
    // Update existing table.
    PHDRTableOffset = Obj->getHeader()->e_phoff;
    NewTextSegmentSize = NextAvailableAddress - NewTextSegmentAddress;
  }
  OS.seek(PHDRTableOffset);

  bool ModdedGnuStack = false;
  bool AddedSegment = false;

  // Copy existing program headers with modifications.
  for (auto &Phdr : Obj->program_headers()) {
    auto NewPhdr = Phdr;
    if (PHDRTableAddress && Phdr.p_type == ELF::PT_PHDR) {
      NewPhdr.p_offset = PHDRTableOffset;
      NewPhdr.p_vaddr = PHDRTableAddress;
      NewPhdr.p_paddr = PHDRTableAddress;
      NewPhdr.p_filesz = sizeof(NewPhdr) * Phnum;
      NewPhdr.p_memsz = sizeof(NewPhdr) * Phnum;
    } else if (Phdr.p_type == ELF::PT_GNU_EH_FRAME) {
      auto SMII = SectionMM->SectionMapInfo.find(".eh_frame_hdr");
      if (SMII != SectionMM->SectionMapInfo.end()) {
        auto &EHFrameHdrSecInfo = SMII->second;
        NewPhdr.p_offset = EHFrameHdrSecInfo.FileOffset;
        NewPhdr.p_vaddr = EHFrameHdrSecInfo.FileAddress;
        NewPhdr.p_paddr = EHFrameHdrSecInfo.FileAddress;
        NewPhdr.p_filesz = EHFrameHdrSecInfo.Size;
        NewPhdr.p_memsz = EHFrameHdrSecInfo.Size;
      }
    } else if (opts::UseGnuStack && Phdr.p_type == ELF::PT_GNU_STACK) {
      NewPhdr.p_type = ELF::PT_LOAD;
      NewPhdr.p_offset = NewTextSegmentOffset;
      NewPhdr.p_vaddr = NewTextSegmentAddress;
      NewPhdr.p_paddr = NewTextSegmentAddress;
      NewPhdr.p_filesz = NewTextSegmentSize;
      NewPhdr.p_memsz = NewTextSegmentSize;
      NewPhdr.p_flags = ELF::PF_X | ELF::PF_R;
      NewPhdr.p_align = PageAlign;
      ModdedGnuStack = true;
    } else if (!opts::UseGnuStack && Phdr.p_type == ELF::PT_DYNAMIC) {
      // Insert new pheader
      ELFFile<ELF64LE>::Elf_Phdr NewTextPhdr;
      NewTextPhdr.p_type = ELF::PT_LOAD;
      NewTextPhdr.p_offset = PHDRTableOffset;
      NewTextPhdr.p_vaddr = PHDRTableAddress;
      NewTextPhdr.p_paddr = PHDRTableAddress;
      NewTextPhdr.p_filesz = NewTextSegmentSize;
      NewTextPhdr.p_memsz = NewTextSegmentSize;
      NewTextPhdr.p_flags = ELF::PF_X | ELF::PF_R;
      NewTextPhdr.p_align = PageAlign;
      OS.write(reinterpret_cast<const char *>(&NewTextPhdr),
               sizeof(NewTextPhdr));
      AddedSegment = true;
    }
    OS.write(reinterpret_cast<const char *>(&NewPhdr), sizeof(NewPhdr));
  }

  assert((!opts::UseGnuStack || ModdedGnuStack) &&
         "could not find GNU_STACK program header to modify");

  assert((opts::UseGnuStack || AddedSegment) &&
         "could not add program header for the new segment");
}

void RewriteInstance::rewriteNoteSections() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  auto &OS = Out->os();

  uint64_t NextAvailableOffset = getFileOffsetFor(NextAvailableAddress);
  assert(NextAvailableOffset >= FirstNonAllocatableOffset &&
         "next available offset calculation failure");
  OS.seek(NextAvailableOffset);

  // Copy over non-allocatable section contents and update file offsets.
  for (auto &Section : Obj->sections()) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;
    if (Section.sh_flags & ELF::SHF_ALLOC)
      continue;

    // Insert padding as needed.
    if (Section.sh_addralign > 1) {
      auto Padding = OffsetToAlignment(NextAvailableOffset,
                                       Section.sh_addralign);
      const unsigned char ZeroByte{0};
      for (unsigned I = 0; I < Padding; ++I)
        OS.write(ZeroByte);

      NextAvailableOffset += Padding;

      assert(Section.sh_size % Section.sh_addralign == 0 &&
             "section size does not match section alignment");
    }

    ErrorOr<StringRef> SectionName = Obj->getSectionName(&Section);
    check_error(SectionName.getError(), "cannot get section name");

    // New section size.
    uint64_t Size = 0;

    // Copy over section contents unless it's one of the sections we ovewrite.
    if (!shouldOverwriteSection(*SectionName)) {
      Size = Section.sh_size;
      std::string Data = InputFile->getData().substr(Section.sh_offset, Size);
      auto SectionPatchersIt = SectionPatchers.find(*SectionName);
      if (SectionPatchersIt != SectionPatchers.end()) {
        (*SectionPatchersIt->second).patchBinary(Data);
      }
      OS << Data;
    }

    // Address of extension to the section.
    uint64_t Address{0};

    // Perform section post-processing.

    auto SII = SectionMM->NoteSectionInfo.find(*SectionName);
    if (SII != SectionMM->NoteSectionInfo.end()) {
      auto &SI = SII->second;
      assert(SI.Alignment <= Section.sh_addralign &&
             "alignment exceeds value in file");

      // Write section extension.
      Address = SI.AllocAddress;
      if (Address) {
        DEBUG(dbgs() << "BOLT: " << (Size ? "appending" : "writing")
                     << " contents to section "
                     << *SectionName << '\n');
        OS.write(reinterpret_cast<const char *>(Address), SI.Size);
        Size += SI.Size;
      }

      if (!SI.PendingRelocs.empty()) {
        DEBUG(dbgs() << "BOLT-DEBUG: processing relocs for section "
                     << *SectionName << '\n');
        for (auto &Reloc : SI.PendingRelocs) {
          DEBUG(dbgs() << "BOLT-DEBUG: writing value "
                       << Twine::utohexstr(Reloc.Value)
                       << " of size " << (unsigned)Reloc.Size
                       << " at offset "
                       << Twine::utohexstr(Reloc.Offset) << '\n');
          assert(Reloc.Size == 4 &&
                 "only relocations of size 4 are supported at the moment");
          OS.pwrite(reinterpret_cast<const char*>(&Reloc.Value),
                    Reloc.Size,
                    NextAvailableOffset + Reloc.Offset);
        }
      }
    }

    // Set/modify section info.
    SectionMM->NoteSectionInfo[*SectionName] =
      SectionInfo(Address,
                  Size,
                  Section.sh_addralign,
                  /*IsCode=*/false,
                  /*IsReadOnly=*/false,
                  /*FileAddress=*/0,
                  NextAvailableOffset);

    NextAvailableOffset += Size;
  }
}

// Rewrite section header table inserting new entries as needed. The sections
// header table size itself may affect the offsets of other sections,
// so we are placing it at the end of the binary.
//
// As we rewrite entries we need to track how many sections were inserted
// as it changes the sh_link value.
//
// The following are assumptoins about file modifications:
//    * There are no modifications done to existing allocatable sections.
//    * All new allocatable sections are written emmediately after existing
//      allocatable sections.
//    * There could be modifications done to non-allocatable sections, e.g.
//      size could be increased.
//    * New non-allocatable sections are added to the end of the file.
void RewriteInstance::patchELFSectionHeaderTable() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  using Elf_Shdr = std::remove_pointer<decltype(Obj)>::type::Elf_Shdr;

  auto &OS = Out->os();

  auto SHTOffset = OS.tell();

  // Copy over entries for original allocatable sections with minor
  // modifications (e.g. name).
  for (auto &Section : Obj->sections()) {
    // Always ignore this section.
    if (Section.sh_type == ELF::SHT_NULL) {
      OS.write(reinterpret_cast<const char *>(&Section), sizeof(Section));
      continue;
    }

    // Break at first non-allocatable section.
    if (!(Section.sh_flags & ELF::SHF_ALLOC))
      break;

    ErrorOr<StringRef> SectionName = Obj->getSectionName(&Section);
    check_error(SectionName.getError(), "cannot get section name");

    auto NewSection = Section;
    if (*SectionName == ".bss") {
      // .bss section offset matches that of the next section.
      NewSection.sh_offset = NewTextSegmentOffset;
    }

    auto SMII = SectionMM->SectionMapInfo.find(*SectionName);
    if (SMII != SectionMM->SectionMapInfo.end()) {
      auto &SecInfo = SMII->second;
      SecInfo.ShName = Section.sh_name;
    }

    OS.write(reinterpret_cast<const char *>(&NewSection), sizeof(NewSection));
  }

  // Create entries for new allocatable sections.
  std::vector<Elf_Shdr> SectionsToRewrite;
  for (auto &SMII : SectionMM->SectionMapInfo) {
    SectionInfo &SI = SMII.second;
    // Ignore function sections.
    if (SI.IsCode && SMII.first != ".bolt.text")
      continue;
    if (opts::Verbosity >= 1) {
      outs() << "BOLT-INFO: writing section header for " << SMII.first << '\n';
    }
    Elf_Shdr NewSection;
    NewSection.sh_name = SI.ShName;
    NewSection.sh_type = ELF::SHT_PROGBITS;
    NewSection.sh_addr = SI.FileAddress;
    NewSection.sh_offset = SI.FileOffset;
    NewSection.sh_size = SI.Size;
    NewSection.sh_entsize = 0;
    NewSection.sh_flags = ELF::SHF_ALLOC | ELF::SHF_EXECINSTR;
    NewSection.sh_link = 0;
    NewSection.sh_info = 0;
    NewSection.sh_addralign = SI.Alignment;
    SectionsToRewrite.emplace_back(NewSection);
  }

  // Write section header entries for new allocatable sections in offset order.
  std::stable_sort(SectionsToRewrite.begin(), SectionsToRewrite.end(),
      [] (Elf_Shdr A, Elf_Shdr B) {
        return A.sh_offset < B.sh_offset;
      });
  for (auto &SI : SectionsToRewrite) {
    OS.write(reinterpret_cast<const char *>(&SI),
             sizeof(SI));
  }

  auto NumNewSections = SectionsToRewrite.size();

  // Copy over entries for non-allocatable sections performing necessary
  // adjustements.
  for (auto &Section : Obj->sections()) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;
    if (Section.sh_flags & ELF::SHF_ALLOC)
      continue;

    ErrorOr<StringRef> SectionName = Obj->getSectionName(&Section);
    check_error(SectionName.getError(), "cannot get section name");

    auto SII = SectionMM->NoteSectionInfo.find(*SectionName);
    assert(SII != SectionMM->NoteSectionInfo.end() &&
           "missing section info for non-allocatable section");

    auto NewSection = Section;
    NewSection.sh_offset = SII->second.FileOffset;
    NewSection.sh_size = SII->second.Size;

    // Adjust sh_link for sections that use it.
    if (Section.sh_link)
      NewSection.sh_link = Section.sh_link + NumNewSections;

    // Adjust sh_info for relocation sections.
    if (Section.sh_type == ELF::SHT_REL || Section.sh_type == ELF::SHT_RELA) {
      if (Section.sh_info)
        NewSection.sh_info = Section.sh_info + NumNewSections;
    }

    OS.write(reinterpret_cast<const char *>(&NewSection), sizeof(NewSection));
  }

  // FIXME: Update _end in .dynamic

  // Fix ELF header.
  auto NewEhdr = *Obj->getHeader();
  NewEhdr.e_phoff = PHDRTableOffset;
  NewEhdr.e_phnum = Phnum;
  NewEhdr.e_shoff = SHTOffset;
  NewEhdr.e_shnum = NewEhdr.e_shnum + NumNewSections;
  NewEhdr.e_shstrndx = NewEhdr.e_shstrndx + NumNewSections;
  OS.pwrite(reinterpret_cast<const char *>(&NewEhdr), sizeof(NewEhdr), 0);
}

void RewriteInstance::rewriteFile() {
  // We obtain an asm-specific writer so that we can emit nops in an
  // architecture-specific way at the end of the function.
  auto MCE = BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  auto MAB = BC->TheTarget->createMCAsmBackend(*BC->MRI, BC->TripleName, "");
  std::unique_ptr<MCStreamer> Streamer(
    BC->TheTarget->createMCObjectStreamer(*BC->TheTriple,
                                          *BC->Ctx,
                                          *MAB,
                                          Out->os(),
                                          MCE,
                                          *BC->STI,
                                          /* RelaxAll */ false,
                                          /* DWARFMustBeAtTheEnd */ false));

  auto &Writer = static_cast<MCObjectStreamer *>(Streamer.get())
                     ->getAssembler()
                     .getWriter();

  // Make sure output stream has enough reserved space, otherwise
  // pwrite() will fail.
  auto Offset = Out->os().seek(getFileOffsetFor(NextAvailableAddress));
  assert(Offset == getFileOffsetFor(NextAvailableAddress) &&
         "error resizing output file");

  // Overwrite function in the output file.
  uint64_t CountOverwrittenFunctions = 0;
  uint64_t OverwrittenScore = 0;
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
      continue;

    if (Function.isSplit() && (Function.cold().getImageAddress() == 0 ||
                               Function.cold().getImageSize() == 0))
      continue;

    if (Function.getImageSize() > Function.getMaxSize()) {
      if (opts::Verbosity >= 1) {
        errs() << "BOLT-WARNING: new function size (0x"
               << Twine::utohexstr(Function.getImageSize())
               << ") is larger than maximum allowed size (0x"
               << Twine::utohexstr(Function.getMaxSize())
               << ") for function " << Function << '\n';
      }
      FailedAddresses.emplace_back(Function.getAddress());
      continue;
    }

    OverwrittenScore += Function.getFunctionScore();
    // Overwrite function in the output file.
    if (opts::Verbosity >= 2) {
      outs() << "BOLT: rewriting function \"" << Function << "\"\n";
    }
    Out->os().pwrite(reinterpret_cast<char *>(Function.getImageAddress()),
                     Function.getImageSize(), Function.getFileOffset());

    // Write nops at the end of the function.
    auto Pos = Out->os().tell();
    Out->os().seek(Function.getFileOffset() + Function.getImageSize());
    MAB->writeNopData(Function.getMaxSize() - Function.getImageSize(),
                      &Writer);
    Out->os().seek(Pos);

    if (!Function.isSplit()) {
      ++CountOverwrittenFunctions;
      if (opts::MaxFunctions &&
          CountOverwrittenFunctions == opts::MaxFunctions) {
        outs() << "BOLT: maximum number of functions reached\n";
        break;
      }
      continue;
    }

    // Write cold part
    if (opts::Verbosity >= 2) {
      outs() << "BOLT: rewriting function \"" << Function << "\" (cold part)\n";
    }
    Out->os().pwrite(reinterpret_cast<char*>(Function.cold().getImageAddress()),
                     Function.cold().getImageSize(),
                     Function.cold().getFileOffset());

    // FIXME: write nops after cold part too.

    ++CountOverwrittenFunctions;
    if (opts::MaxFunctions && CountOverwrittenFunctions == opts::MaxFunctions) {
      outs() << "BOLT: maximum number of functions reached\n";
      break;
    }
  }

  // Print function statistics.
  outs() << "BOLT: " << CountOverwrittenFunctions
         << " out of " << BinaryFunctions.size()
         << " functions were overwritten.\n";
  if (TotalScore != 0) {
    double Coverage = OverwrittenScore / (double)TotalScore * 100.0;
    outs() << format("BOLT: Rewritten functions cover %.2lf", Coverage)
           << "% of the execution count of simple functions of this binary.\n";
  }

  // Write all non-code sections.
  for (auto &SMII : SectionMM->SectionMapInfo) {
    SectionInfo &SI = SMII.second;
    if (SI.IsCode)
      continue;
    if (opts::Verbosity >= 1) {
      outs() << "BOLT: writing new section " << SMII.first << '\n';
    }
    Out->os().pwrite(reinterpret_cast<const char *>(SI.AllocAddress),
                     SI.Size,
                     SI.FileOffset);
  }

  // If .eh_frame is present it requires special handling.
  auto SMII = SectionMM->SectionMapInfo.find(".eh_frame");
  if (SMII != SectionMM->SectionMapInfo.end()) {
    auto &EHFrameSecInfo = SMII->second;
    if (opts::Verbosity >= 1) {
      outs() << "BOLT: writing a new .eh_frame_hdr\n";
    }
    if (FrameHdrAlign > 1) {
      auto PaddingSize = OffsetToAlignment(NextAvailableAddress, FrameHdrAlign);
      for (unsigned I = 0; I < PaddingSize; ++I)
        Out->os().write((unsigned char)0);
      NextAvailableAddress += PaddingSize;
    }

    SectionInfo EHFrameHdrSecInfo;
    EHFrameHdrSecInfo.FileAddress = NextAvailableAddress;
    EHFrameHdrSecInfo.FileOffset = getFileOffsetFor(NextAvailableAddress);

    std::sort(FailedAddresses.begin(), FailedAddresses.end());
    CFIRdWrt->rewriteHeaderFor(
        StringRef(reinterpret_cast<const char *>(EHFrameSecInfo.AllocAddress),
                  EHFrameSecInfo.Size),
        EHFrameSecInfo.FileAddress,
        EHFrameHdrSecInfo.FileAddress,
        FailedAddresses);

    EHFrameHdrSecInfo.Size = FrameHdrCopy.size();

    assert(Out->os().tell() == EHFrameHdrSecInfo.FileOffset &&
           "offset mismatch");
    Out->os().write(FrameHdrCopy.data(), EHFrameHdrSecInfo.Size);

    SectionMM->SectionMapInfo[".eh_frame_hdr"] = EHFrameHdrSecInfo;

    NextAvailableAddress += EHFrameHdrSecInfo.Size;
  }

  // Patch program header table.
  patchELFPHDRTable();

  // Copy non-allocatable sections once allocatable part is finished.
  rewriteNoteSections();

  // Update ELF book-keeping info.
  patchELFSectionHeaderTable();

  // TODO: we should find a way to mark the binary as optimized by us.
  Out->keep();
}

bool RewriteInstance::shouldOverwriteSection(StringRef SectionName) {
  if (opts::UpdateDebugSections) {
    for (auto &OverwriteName : DebugSectionsToOverwrite) {
      if (SectionName == OverwriteName)
        return true;
    }
  }

  return false;
}

BinaryFunction *
RewriteInstance::getBinaryFunctionContainingAddress(uint64_t Address) {
  auto FI = BinaryFunctions.upper_bound(Address);
  if (FI == BinaryFunctions.begin())
    return nullptr;
  --FI;
  if (FI->first + FI->second.getSize() <= Address)
    return nullptr;
  return &FI->second;
}
