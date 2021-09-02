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
#include "DataReader.h"
#include "Exceptions.h"
#include "RewriteInstance.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
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
#include "llvm/Support/Debug.h"
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
#define DEBUG_TYPE "flo"

using namespace llvm;
using namespace object;
using namespace flo;

namespace opts {

static cl::opt<std::string>
OutputFilename("o", cl::desc("<output file>"), cl::Required);

static cl::list<std::string>
FunctionNames("funcs",
              cl::CommaSeparated,
              cl::desc("list of functions to optimize"),
              cl::value_desc("func1,func2,func3,..."));

static cl::opt<std::string>
FunctionNamesFile("funcs_file",
                  cl::desc("file with list of functions to optimize"));

static cl::list<std::string>
SkipFunctionNames("skip_funcs",
                  cl::CommaSeparated,
                  cl::desc("list of functions to skip"),
                  cl::value_desc("func1,func2,func3,..."));

static cl::opt<unsigned>
MaxFunctions("max_funcs",
             cl::desc("maximum # of functions to overwrite"),
             cl::Optional);

static cl::opt<bool>
EliminateUnreachable("eliminate-unreachable",
                     cl::desc("eliminate unreachable code"),
                     cl::Optional);

static cl::opt<bool>
SplitFunctions("split-functions",
               cl::desc("split functions into hot and cold distinct regions"),
               cl::Optional);

static cl::opt<BinaryFunction::LayoutType> ReorderBlocks(
    "reorder-blocks",
    cl::desc("change layout of basic blocks in a function"),
    cl::init(BinaryFunction::LT_NONE),
    cl::values(clEnumValN(BinaryFunction::LT_NONE,
                          "none",
                          "do not reorder basic blocks"),
               clEnumValN(BinaryFunction::LT_REVERSE,
                          "reverse",
                          "layout blocks in reverse order"),
               clEnumValN(BinaryFunction::LT_OPTIMIZE,
                          "normal",
                          "perform optimal layout based on profile"),
               clEnumValN(BinaryFunction::LT_OPTIMIZE_BRANCH,
                          "branch-predictor",
                          "perform optimal layout prioritizing branch "
                            "predictions"),
               clEnumValN(BinaryFunction::LT_OPTIMIZE_CACHE,
                          "cache",
                          "perform optimal layout prioritizing I-cache "
                            "behavior"),
               clEnumValEnd));


static cl::opt<bool> AlignBlocks("align-blocks",
                                 cl::desc("try to align BBs inserting nops"),
                                 cl::Optional);

static cl::opt<bool>
DumpEHFrame("dump-eh-frame", cl::desc("dump parsed .eh_frame (debugging)"),
         cl::Hidden);

static cl::opt<bool>
PrintAll("print-all", cl::desc("print functions after each stage"),
         cl::Hidden);

static cl::opt<bool>
PrintCFG("print-cfg", cl::desc("print functions after CFG construction"),
         cl::Hidden);

static cl::opt<bool>
PrintUCE("print-uce",
         cl::desc("print functions after unreachable code elimination"),
         cl::Hidden);

static cl::opt<bool>
PrintDisasm("print-disasm", cl::desc("print function after disassembly"),
            cl::Hidden);

static cl::opt<bool>
PrintEHRanges("print-eh-ranges",
              cl::desc("print function with updated exception ranges"),
              cl::Hidden);

static cl::opt<bool>
PrintReordered("print-reordered",
               cl::desc("print functions after layout optimization"),
               cl::Hidden);


// Check against lists of functions from options if we should
// optimize the function with a given name.
bool shouldProcess(const BinaryFunction &Function) {
  if (opts::MaxFunctions && Function.getFunctionNumber() > opts::MaxFunctions)
    return false;

  if (!FunctionNamesFile.empty()) {
    std::ifstream FuncsFile(FunctionNamesFile, std::ios::in);
    std::string FuncName;
    while (std::getline(FuncsFile, FuncName)) {
      FunctionNames.push_back(FuncName);
    }
    FunctionNamesFile = "";
  }

  bool IsValid = true;
  if (!FunctionNames.empty()) {
    IsValid = false;
    for (auto &Name : FunctionNames) {
      if (Function.getName() == Name) {
        IsValid = true;
        break;
      }
    }
  }
  if (!IsValid)
    return false;

  if (!SkipFunctionNames.empty()) {
    for (auto &Name : SkipFunctionNames) {
      if (Function.getName() == Name) {
        IsValid = false;
        break;
      }
    }
  }

  return IsValid;
}

} // namespace opts


static void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << "FLO: '" << Message << "': " << EC.message() << ".\n";
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

  DEBUG(dbgs() << "FLO: allocating " << (IsCode ? "code" : "data")
               << " section : " << SectionName
               << " with size " << Size << ", alignment " << Alignment
               << " at 0x" << ret << "\n");

  SectionMapInfo[SectionName] = SectionInfo(reinterpret_cast<uint64_t>(ret),
                                            Size,
                                            Alignment,
                                            IsCode);

  return ret;
}

bool ExecutableFileMemoryManager::finalizeMemory(std::string *ErrMsg) {
  DEBUG(dbgs() << "FLO: finalizeMemory()\n");
  return SectionMemoryManager::finalizeMemory(ErrMsg);
}

/// Create BinaryContext for a given architecture \p ArchName and
/// triple \p TripleName.
static std::unique_ptr<BinaryContext> CreateBinaryContext(
    std::string ArchName,
    std::string TripleName, const DataReader &DR) {

  std::string Error;

  std::unique_ptr<Triple> TheTriple = llvm::make_unique<Triple>(TripleName);
  const Target *TheTarget = TargetRegistry::lookupTarget(ArchName,
                                                         *TheTriple,
                                                         Error);
  if (!TheTarget) {
    errs() << "FLO: " << Error;
    return nullptr;
  }

  std::unique_ptr<const MCRegisterInfo> MRI(
      TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "error: no register info for target " << TripleName << "\n";
    return nullptr;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> AsmInfo(
      TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!AsmInfo) {
    errs() << "error: no assembly info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TripleName << "\n";
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
    errs() << "error: no disassembler for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCInstrAnalysis> MIA(
      TheTarget->createMCInstrAnalysis(MII.get()));
  if (!MIA) {
    errs() << "error: failed to create instruction analysis for target"
           << TripleName << "\n";
    return nullptr;
  }

  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  std::unique_ptr<MCInstPrinter> InstructionPrinter(
      TheTarget->createMCInstPrinter(Triple(TripleName), AsmPrinterVariant,
                                     *AsmInfo, *MII, *MRI));
  if (!InstructionPrinter) {
    errs() << "error: no instruction printer for target " << TripleName
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

RewriteInstance::RewriteInstance(ELFObjectFileBase *File,
                                 const DataReader &DR)
    : File(File), BC(CreateBinaryContext("x86-64", "x86_64-unknown-linux", DR)),
      DwCtx(new DWARFContextInMemory(*File)) {}

RewriteInstance::~RewriteInstance() {}

void RewriteInstance::reset() {
  BinaryFunctions.clear();
  FileSymRefs.clear();
  auto &DR = BC->DR;
  BC = CreateBinaryContext("x86-64", "x86_64-unknown-linux", DR);
  DwCtx.reset(new DWARFContextInMemory(*File));
  CFIRdWrt.reset(nullptr);
  SectionMM.reset(nullptr);
  Out.reset(nullptr);
  EHFrame = nullptr;
  FailedAddresses.clear();
  TotalScore = 0;
}

void RewriteInstance::run() {
  if (!BC) {
    errs() << "failed to create a binary context\n";
    return;
  }

  readSymbolTable();
  readSpecialSections();
  disassembleFunctions();
  runOptimizationPasses();
  emitFunctions();

  if (opts::SplitFunctions && splitLargeFunctions()) {
    // Emit again because now some functions have been split
    outs() << "FLO: split-functions: starting pass 2...\n";
    reset();
    readSymbolTable();
    readSpecialSections();
    disassembleFunctions();
    runOptimizationPasses();
    emitFunctions();
  }

  // Copy input file to output
  std::error_code EC;
  Out = llvm::make_unique<tool_output_file>(opts::OutputFilename, EC,
                                            sys::fs::F_None, 0777);
  check_error(EC, "cannot create output executable file");
  Out->os() << File->getData();

  // Rewrite optimized functions back to this output
  rewriteFile();
}

namespace {

// Helper function to map a random memory address to a file offset. Returns 0 if
// this address cannot be mapped back to the file.
uint64_t discoverFileOffset(ELFObjectFileBase *File, uint64_t MemAddr) {
  for (const auto &Section : File->sections()) {
    uint64_t SecAddress = Section.getAddress();
    uint64_t Size = Section.getSize();
    if (MemAddr < SecAddress ||
        SecAddress + Size <= MemAddr)
      continue;

    StringRef SectionContents;
    check_error(Section.getContents(SectionContents),
                "cannot get section contents");
    uint64_t SecFileOffset = SectionContents.data() - File->getData().data();
    uint64_t MemAddrSecOffset = MemAddr - SecAddress;
    return SecFileOffset + MemAddrSecOffset;
  }
  return 0ULL;
}

} // anonymous namespace

void RewriteInstance::readSymbolTable() {
  std::string FileSymbolName;

  FileSymRefs.clear();
  BinaryFunctions.clear();
  BC->GlobalAddresses.clear();

  // For local symbols we want to keep track of associated FILE symbol for
  // disambiguation by name.
  for (const SymbolRef &Symbol : File->symbols()) {
    // Keep undefined symbols for pretty printing?
    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    ErrorOr<StringRef> Name = Symbol.getName();
    check_error(Name.getError(), "cannot get symbol name");

    if (*Name == "__flo_storage") {
      ExtraStorage.Addr = Symbol.getValue();
      ExtraStorage.BumpPtr = ExtraStorage.Addr;
      ExtraStorage.FileOffset = discoverFileOffset(File, ExtraStorage.Addr);
      assert(ExtraStorage.FileOffset != 0 && "Corrupt __flo_storage symbol");

      FileSymRefs[ExtraStorage.Addr] = Symbol;
      continue;
    }
    if (*Name == "__flo_storage_end") {
      ExtraStorage.AddrEnd = Symbol.getValue();
      continue;
    }

    if (Symbol.getType() == SymbolRef::ST_File) {
      // Could be used for local symbol disambiguation.
      FileSymbolName = *Name;
      continue;
    }

    ErrorOr<uint64_t> AddressOrErr = Symbol.getAddress();
    check_error(AddressOrErr.getError(), "cannot get symbol address");
    uint64_t Address = *AddressOrErr;
    if (Address == 0) {
      if (Symbol.getType() == SymbolRef::ST_Function)
        errs() << "FLO-WARNING: function with 0 address seen\n";
      continue;
    }

    FileSymRefs[Address] = Symbol;

    // There's nothing horribly wrong with anonymous symbols, but let's
    // ignore them for now.
    if (Name->empty())
      continue;

    // Disambiguate all local symbols before adding to symbol table.
    // Since we don't know if we'll see a global with the same name,
    // always modify the local name.
    std::string UniqueName;
    if (Symbol.getFlags() & SymbolRef::SF_Global) {
      assert(BC->GlobalSymbols.find(*Name) == BC->GlobalSymbols.end() &&
             "global name not unique");
      UniqueName = *Name;
      /// It's possible we are seeing a globalized local. LLVM might treat it as
      /// local if it has a "private global" prefix, e.g. ".L". Thus we have to
      /// change the prefix to enforce global scope of the symbol.
      if (StringRef(UniqueName)
              .startswith(BC->AsmInfo->getPrivateGlobalPrefix()))
        UniqueName = "PG." + UniqueName;
    } else {
      unsigned LocalCount = 1;
      std::string LocalName = (*Name).str() + "/" + FileSymbolName + "/";

      if ((*Name).startswith(BC->AsmInfo->getPrivateGlobalPrefix())) {
        LocalName = "PG." + LocalName;
      }

      while (BC->GlobalSymbols.find(LocalName + std::to_string(LocalCount)) !=
             BC->GlobalSymbols.end()) {
        ++LocalCount;
      }
      UniqueName = LocalName + std::to_string(LocalCount);
    }

    // Add the name to global symbols map.
    BC->GlobalSymbols[UniqueName] = Address;

    // Add to the reverse map. There could multiple names at the same address.
    BC->GlobalAddresses.emplace(std::make_pair(Address, UniqueName));

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
    if (Section == File->section_end()) {
      // Could be an absolute symbol. Could record for pretty printing.
      continue;
    }

    // Create the function and add to the map.
    BinaryFunctions.emplace(
        Address,
        BinaryFunction(UniqueName, Symbol, *Section, Address,
                       SymbolSize, *BC)
    );
  }
  ExtraStorage.Size = ExtraStorage.AddrEnd - ExtraStorage.Addr;
}

void RewriteInstance::readSpecialSections() {
  // Process special sections.
  StringRef FrameHdrContents;
  for (const auto &Section : File->sections()) {
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
    }
    if (SectionName == ".eh_frame_hdr") {
      FrameHdrAddress = Section.getAddress();
      FrameHdrContents = SectionContents;
      FrameHdrAlign = Section.getAlignment();
    }
  }

  FrameHdrCopy =
      std::vector<char>(FrameHdrContents.begin(), FrameHdrContents.end());
  // Process debug sections.
  EHFrame = DwCtx->getEHFrame();
  if (opts::DumpEHFrame) {
    EHFrame->dump(outs());
  }
  CFIRdWrt.reset(new CFIReaderWriter(*EHFrame, FrameHdrAddress, FrameHdrCopy));
  if (!EHFrame->ParseError.empty()) {
    errs() << "FLO-ERROR: EHFrame reader failed with message \""
           << EHFrame->ParseError << "\"\n";
    exit(1);
  }
}

void RewriteInstance::disassembleFunctions() {
  // Disassemble every function and build it's control flow graph.
  TotalScore = 0;
  for (auto &BFI : BinaryFunctions) {
    BinaryFunction &Function = BFI.second;

    if (!opts::shouldProcess(Function)) {
      DEBUG(dbgs() << "FLO: skipping processing function " << Function.getName()
                   << " per user request.\n");
      continue;
    }

    SectionRef Section = Function.getSection();
    assert(Section.getAddress() <= Function.getAddress() &&
           Section.getAddress() + Section.getSize() 
             >= Function.getAddress() + Function.getSize() &&
          "wrong section for function");
    if (!Section.isText() || Section.isVirtual() || !Section.getSize()) {
      // When could it happen?
      errs() << "FLO: corresponding section is non-executable or empty "
             << "for function " << Function.getName();
      continue;
    }

    // Set the proper maximum size value after the whole symbol table
    // has been processed.
    auto SymRefI = FileSymRefs.upper_bound(Function.getAddress());
    if (SymRefI != FileSymRefs.end()) {
      auto MaxSize = SymRefI->first - Function.getAddress();
      if (MaxSize < Function.getSize()) {
        errs() << "FLO-WARNING: symbol seen in the middle of the function "
               << Function.getName() << ". Skipping.\n";
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
        SectionContents.data() - File->getData().data() + FunctionOffset);

    ArrayRef<uint8_t> FunctionData(
        reinterpret_cast<const uint8_t *>
          (SectionContents.data()) + FunctionOffset,
        Function.getSize());

    if (!Function.disassemble(FunctionData))
      continue;

    if (opts::PrintAll || opts::PrintDisasm)
      Function.print(errs(), "after disassembly");

    if (!Function.isSimple())
      continue;

    // Fill in CFI information for this function
    if (EHFrame->ParseError.empty() && Function.isSimple()) {
      CFIRdWrt->fillCFIInfoFor(Function);
    }

    // Parse LSDA.
    if (Function.getLSDAAddress() != 0)
      Function.parseLSDA(LSDAData, LSDAAddress);

    if (!Function.buildCFG())
      continue;

    if (opts::PrintAll || opts::PrintCFG)
      Function.print(errs(), "after building cfg");

    TotalScore += Function.getFunctionScore();

  } // Iterate over all functions

  // Mark all functions with internal addresses serving as interprocedural
  // branch targets as not simple --  pretty rare but can happen in code
  // written in assembly.
  // TODO: #9301815
  for (auto Addr : BC->InterproceduralBranchTargets) {
    // Check if this address is internal to some function we are reordering
    auto I = BinaryFunctions.upper_bound(Addr);
    if (I == BinaryFunctions.begin())
      continue;
    BinaryFunction &Func = (--I)->second;
    uint64_t Offset = Addr - I->first;
    if (Offset == 0 || Offset >= Func.getSize())
      continue;
    errs() << "FLO-WARNING: Function " << Func.getName()
           << " has internal BBs that are target of a branch located in "
              "another function. We will not process this function.\n";
    Func.setSimple(false);
  }

  uint64_t NumSimpleFunctions{0};
  std::vector<BinaryFunction *> ProfiledFunctions;
  for (auto &BFI : BinaryFunctions) {
    if (!BFI.second.isSimple())
      continue;
    ++NumSimpleFunctions;
    if (BFI.second.getExecutionCount() != BinaryFunction::COUNT_NO_PROFILE)
      ProfiledFunctions.push_back(&BFI.second);
  }

  errs() << "FLO-INFO: " << ProfiledFunctions.size() << " functions out of "
         << NumSimpleFunctions
         << " simple functions ("
         << format("%.1f",
                   ProfiledFunctions.size() /
                   (float) NumSimpleFunctions * 100.0)
         << "%) have non-empty execution profile.\n";

  if (ProfiledFunctions.size() > 10) {
    errs() << "FLO-INFO: top called functions are:\n";
    std::sort(ProfiledFunctions.begin(), ProfiledFunctions.end(),
              [](BinaryFunction *A, BinaryFunction *B) {
                return B->getExecutionCount() < A->getExecutionCount();
              }
    );
    auto SFI = ProfiledFunctions.begin();
    for(int i = 0; i < 100 && SFI != ProfiledFunctions.end(); ++SFI, ++i) {
      errs() << "  " << (*SFI)->getName() << " : "
             << (*SFI)->getExecutionCount() << '\n';
    }
  }
}

void RewriteInstance::runOptimizationPasses() {
  // Run optimization passes.
  //
  // FIXME: use real optimization passes.
  bool NagUser = true;
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (!opts::shouldProcess(Function))
      continue;

    if (!Function.isSimple())
      continue;

    // Detect and eliminate unreachable basic blocks. We could have those
    // filled with nops and they are used for alignment.
    //
    // FIXME: this wouldn't work with C++ exceptions until we implement
    //        support for those as there will be "invisible" edges
    //        in the graph.
    if (opts::EliminateUnreachable && Function.layout_size() > 0) {
      if (NagUser) {
        outs()
            << "FLO-WARNING: Using -eliminate-unreachable is experimental and "
               "unsafe for exceptions\n";
        NagUser = false;
      }

      std::stack<BinaryBasicBlock*> Stack;
      std::map<BinaryBasicBlock *, bool> Reachable;
      BinaryBasicBlock *Entry = *Function.layout_begin();
      Stack.push(Entry);
      Reachable[Entry] = true;
      // Determine reachable BBs from the entry point
      while (!Stack.empty()) {
        auto BB = Stack.top();
        Stack.pop();
        for (auto Succ : BB->successors()) {
          if (Reachable[Succ])
            continue;
          Reachable[Succ] = true;
          Stack.push(Succ);
        }
      }

      auto Count = Function.eraseDeadBBs(Reachable);
      if (Count) {
        DEBUG(dbgs() << "FLO: Removed " << Count
                     << " dead basic block(s) in function "
                     << Function.getName() << '\n');
      }

      if (opts::PrintAll || opts::PrintUCE)
        Function.print(errs(), "after unreachable code elimination");
    }

    if (opts::ReorderBlocks != BinaryFunction::LT_NONE) {
      bool ShouldSplit = ToSplit.find(BFI.first) != ToSplit.end();
      BFI.second.modifyLayout(opts::ReorderBlocks, ShouldSplit);
      if (opts::PrintAll || opts::PrintReordered)
        Function.print(errs(), "after reordering blocks");
    }

    // Post-processing passes.

    // Fix the CFI state.
    if (!Function.fixCFIState()) {
      errs() << "FLO-WARNING: unable to fix CFI state for function "
             << Function.getName() << ". Skipping.\n";
      Function.setSimple(false);
      continue;
    }

    // Update exception handling information.
    Function.updateEHRanges();
    if (opts::PrintAll || opts::PrintEHRanges)
      Function.print(errs(), "after updating EH ranges");

  }
}

namespace {

// Helper function to emit the contents of a function via a MCStreamer object.
void emitFunction(MCStreamer &Streamer, BinaryFunction &Function,
                  BinaryContext &BC, bool EmitColdPart, bool HasExtraStorage) {
  // Define a helper to decode and emit CFI instructions at a given point in a
  // BB
  auto emitCFIInstr = [&Streamer](MCCFIInstruction &CFIInstr) {
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
  Streamer.SwitchSection(Section);

  Streamer.EmitCodeAlignment(Function.getAlignment());

  if (!EmitColdPart) {
    MCSymbol *FunctionSymbol = BC.Ctx->getOrCreateSymbol(Function.getName());
    Streamer.EmitSymbolAttribute(FunctionSymbol, MCSA_ELF_TypeFunction);
    Streamer.EmitLabel(FunctionSymbol);
    Function.setOutputSymbol(FunctionSymbol);
  } else {
    MCSymbol *FunctionSymbol =
      BC.Ctx->getOrCreateSymbol(Twine(Function.getName()).concat(".cold"));
    Streamer.EmitSymbolAttribute(FunctionSymbol, MCSA_ELF_TypeFunction);
    Streamer.EmitLabel(FunctionSymbol);
    Function.cold().setOutputSymbol(FunctionSymbol);
  }

  // Emit CFI start
  if (Function.hasCFI() && HasExtraStorage) {
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

  assert(!Function.begin()->isCold() &&
         "first basic block should never be cold");

  // Emit code.
  for (auto BB : Function.layout()) {
    if (EmitColdPart != BB->isCold())
      continue;
    if (opts::AlignBlocks && BB->getAlignment() > 1)
      Streamer.EmitCodeAlignment(BB->getAlignment());
    Streamer.EmitLabel(BB->getLabel());
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
      if (!BC.MIA->isCFI(Instr)) {
        Streamer.EmitInstruction(Instr, *BC.STI);
        continue;
      }
      if (HasExtraStorage)
        emitCFIInstr(*Function.getCFIFor(Instr));
    }
  }

  // Emit CFI end
  if (Function.hasCFI() && HasExtraStorage)
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
    llvm::make_unique<tool_output_file>(opts::OutputFilename + ".o",
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

  bool NoSpaceWarning = false;
  // Output functions one by one.
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (!Function.isSimple())
      continue;

    if (!opts::shouldProcess(Function))
      continue;

    DEBUG(dbgs() << "FLO: generating code for function \"" << Function.getName()
                 << "\" : " << Function.getFunctionNumber() << '\n');

    if (Function.hasCFI()) {
      if (ExtraStorage.Size == 0)
        NoSpaceWarning = true;
    }

    emitFunction(*Streamer, Function, *BC.get(),
                 /*EmitColdPart=*/false,
                 /*HasExtraStorage=*/ExtraStorage.Size != 0);

    if (Function.isSplit())
      emitFunction(*Streamer, Function, *BC.get(),
                   /*EmitColdPart=*/true,
                   /*HasExtraStorage=*/ExtraStorage.Size != 0);
  }
  if (NoSpaceWarning) {
    errs() << "FLO-WARNING: missing __flo_storage in this binary. No "
           << "extra space left to allocate the new .eh_frame\n";
  }

  Streamer->Finish();

  // Get output object as ObjectFile.
  std::unique_ptr<MemoryBuffer> ObjectMemBuffer =
      MemoryBuffer::getMemBuffer(BOS->str(), "in-memory object file", false);
  ErrorOr<std::unique_ptr<object::ObjectFile>> ObjOrErr =
    object::ObjectFile::createObjectFile(ObjectMemBuffer->getMemBufferRef());
  check_error(ObjOrErr.getError(), "error creating in-memory object");

  auto EFMM = new ExecutableFileMemoryManager();
  SectionMM.reset(EFMM);

  // FIXME: use notifyObjectLoaded() to remap sections.

  DEBUG(dbgs() << "Creating OLT\n");
  // Run ObjectLinkingLayer() with custom memory manager and symbol resolver.
  orc::ObjectLinkingLayer<> OLT;

  auto Resolver = orc::createLambdaResolver(
          [&](const std::string &Name) {
            DEBUG(dbgs() << "FLO: looking for " << Name << "\n");
            auto I = BC->GlobalSymbols.find(Name);
            if (I == BC->GlobalSymbols.end())
              return RuntimeDyld::SymbolInfo(nullptr);
            return RuntimeDyld::SymbolInfo(I->second,
                                           JITSymbolFlags::None);
          },
          [](const std::string &S) {
            DEBUG(dbgs() << "FLO: resolving " << S << "\n");
            return nullptr;
          }
      );
  // FIXME:
  auto ObjectsHandle = OLT.addObjectSet(
        singletonSet(std::move(ObjOrErr.get())),
        SectionMM.get(),
        std::move(Resolver));
  //OLT.takeOwnershipOfBuffers(ObjectsHandle, );

  // Map every function/section current address in memory to that in
  // the output binary.
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;
    if (!Function.isSimple())
      continue;

    auto SMII = EFMM->SectionMapInfo.find(Function.getCodeSectionName());
    if (SMII != EFMM->SectionMapInfo.end()) {
      DEBUG(dbgs() << "FLO: mapping 0x"
                   << Twine::utohexstr(SMII->second.AllocAddress)
                   << " to 0x" << Twine::utohexstr(Function.getAddress())
                   << '\n');
      OLT.mapSectionAddress(ObjectsHandle,
          reinterpret_cast<const void*>(SMII->second.AllocAddress),
          Function.getAddress());
      Function.setImageAddress(SMII->second.AllocAddress);
      Function.setImageSize(SMII->second.Size);
    } else {
      errs() << "FLO: cannot remap function " << Function.getName() << "\n";
      FailedAddresses.emplace_back(Function.getAddress());
    }

    if (!Function.isSplit())
      continue;

    SMII = EFMM->SectionMapInfo.find(
        Function.getCodeSectionName().str().append(".cold"));
    if (SMII != EFMM->SectionMapInfo.end()) {
      // Align at a 16-byte boundary
      ExtraStorage.BumpPtr = RoundUpToAlignment(ExtraStorage.BumpPtr, 16);
      DEBUG(dbgs() << "FLO: mapping 0x"
                   << Twine::utohexstr(SMII->second.AllocAddress)
                   << " to 0x" << Twine::utohexstr(ExtraStorage.BumpPtr)
                   << " with size " << Twine::utohexstr(SMII->second.Size)
                   << '\n');
      OLT.mapSectionAddress(ObjectsHandle,
          reinterpret_cast<const void*>(SMII->second.AllocAddress),
          ExtraStorage.BumpPtr);
      Function.cold().setImageAddress(SMII->second.AllocAddress);
      Function.cold().setImageSize(SMII->second.Size);
      Function.cold().setFileOffset(ExtraStorage.BumpPtr - ExtraStorage.Addr +
                                    ExtraStorage.FileOffset);
      ExtraStorage.BumpPtr += SMII->second.Size;
    } else {
      errs() << "FLO: cannot remap function " << Function.getName() << "\n";
      FailedAddresses.emplace_back(Function.getAddress());
    }
  }

  // Map special sections to their addresses in the output image.
  //
  // TODO: perhaps we should process all the allocated sections here?
  std::vector<std::string> Sections = { ".eh_frame", ".gcc_except_table" };
  for(auto &SectionName : Sections) {
    auto SMII = EFMM->SectionMapInfo.find(SectionName);
    if (SMII != EFMM->SectionMapInfo.end()) {
      SectionInfo &SI = SMII->second;
      ExtraStorage.BumpPtr = RoundUpToAlignment(ExtraStorage.BumpPtr,
                                                SI.Alignment);
      DEBUG(dbgs() << "FLO: mapping 0x"
                   << Twine::utohexstr(SI.AllocAddress)
                   << " to 0x" << Twine::utohexstr(ExtraStorage.BumpPtr)
                   << '\n');

      OLT.mapSectionAddress(ObjectsHandle,
                            reinterpret_cast<const void *>(SI.AllocAddress),
                            ExtraStorage.BumpPtr);

      SI.FileAddress = ExtraStorage.BumpPtr;
      SI.FileOffset = ExtraStorage.BumpPtr - ExtraStorage.Addr +
                      ExtraStorage.FileOffset;

      ExtraStorage.BumpPtr += SI.Size;
    } else {
      errs() << "FLO: cannot remap " << SectionName << '\n';
    }
  }

  if (ExtraStorage.BumpPtr - ExtraStorage.Addr > ExtraStorage.Size) {
    errs() << format(
        "FLO fatal error: __flo_storage in this binary has not enough free "
        "space (required %d bytes, available %d bytes).\n",
        ExtraStorage.BumpPtr - ExtraStorage.Addr, ExtraStorage.Size);
    exit(1);
  }

  OLT.emitAndFinalize(ObjectsHandle);
  TempOut->keep();
}

bool RewriteInstance::splitLargeFunctions() {
  bool Changed = false;
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    // Ignore this function if we failed to map it to the output binary
    if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
      continue;

    if (Function.getImageSize() <= Function.getMaxSize())
      continue;

    ToSplit.insert(BFI.first);
    Changed = true;
  }
  return Changed;
}

namespace {

// Helper to locate EH_FRAME_HDR segment, specialized for 64-bit LE ELF
bool patchEhFrameHdrSegment(const ELFFile<ELF64LE> *Obj, raw_pwrite_stream *OS,
                            uint64_t Offset, uint64_t Addr, uint64_t Size) {
  for (const auto &Phdr : Obj->program_headers()) {
    if (Phdr.p_type != ELF::PT_GNU_EH_FRAME)
      continue;
    uint64_t OffsetLoc = (uintptr_t)&Phdr.p_offset - (uintptr_t)Obj->base();
    uint64_t VAddrLoc = (uintptr_t)&Phdr.p_vaddr - (uintptr_t)Obj->base();
    uint64_t PAddrLoc = (uintptr_t)&Phdr.p_paddr - (uintptr_t)Obj->base();
    uint64_t FileSzLoc = (uintptr_t)&Phdr.p_filesz - (uintptr_t)Obj->base();
    uint64_t MemSzLoc = (uintptr_t)&Phdr.p_memsz - (uintptr_t)Obj->base();
    char Buffer[8];
    // Update Offset
    support::ulittle64_t::ref(Buffer + 0) = Offset;
    OS->pwrite(Buffer, 8, OffsetLoc);
    support::ulittle64_t::ref(Buffer + 0) = Addr;
    OS->pwrite(Buffer, 8, VAddrLoc);
    OS->pwrite(Buffer, 8, PAddrLoc);
    support::ulittle64_t::ref(Buffer + 0) = Size;
    OS->pwrite(Buffer, 8, FileSzLoc);
    OS->pwrite(Buffer, 8, MemSzLoc);
    return true;
  }
  return false;
}

} // anonymous namespace

void RewriteInstance::rewriteFile() {
  // FIXME: is there a less painful way to obtain assembler/writer?
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

  // Print _flo_storage area stats for debug
  DEBUG(dbgs() << format("INFO: __flo_storage address = 0x%x file offset = "
                         "0x%x total size = 0x%x\n",
                         ExtraStorage.Addr, ExtraStorage.FileOffset,
                         ExtraStorage.Size));

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
      errs() << "FLO-WARNING: new function size (0x"
             << Twine::utohexstr(Function.getImageSize())
             << ") is larger than maximum allowed size (0x"
             << Twine::utohexstr(Function.getMaxSize())
             << ") for function " << Function.getName() << '\n';
      FailedAddresses.emplace_back(Function.getAddress());
      continue;
    }

    OverwrittenScore += Function.getFunctionScore();
    // Overwrite function in the output file.
    outs() << "FLO: rewriting function \"" << Function.getName() << "\"\n";
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
        outs() << "FLO: maximum number of functions reached\n";
        break;
      }
      continue;
    }

    // Write cold part
    outs() << "FLO: rewriting function \"" << Function.getName()
           << "\" (cold part)\n";
    Out->os().pwrite(reinterpret_cast<char*>(Function.cold().getImageAddress()),
                     Function.cold().getImageSize(),
                     Function.cold().getFileOffset());

    ++CountOverwrittenFunctions;
    if (opts::MaxFunctions && CountOverwrittenFunctions == opts::MaxFunctions) {
      outs() << "FLO: maximum number of functions reached\n";
      break;
    }
  }

  outs() << "FLO: " << CountOverwrittenFunctions
         << " out of " << BinaryFunctions.size()
         << " functions were overwritten.\n";

  // If .eh_frame is present it requires special handling.
  auto SMII = SectionMM->SectionMapInfo.find(".eh_frame");
  if (SMII != SectionMM->SectionMapInfo.end()) {
    auto &EHFrameSI = SMII->second;
    outs() << "FLO: writing a new .eh_frame_hdr\n";
    if (FrameHdrAlign > 1) {
      ExtraStorage.BumpPtr =
        RoundUpToAlignment(ExtraStorage.BumpPtr, FrameHdrAlign);
    }
    std::sort(FailedAddresses.begin(), FailedAddresses.end());
    CFIRdWrt->rewriteHeaderFor(
        StringRef(reinterpret_cast<const char *>(EHFrameSI.AllocAddress),
                  EHFrameSI.Size),
        EHFrameSI.FileAddress,
        ExtraStorage.BumpPtr,
        FailedAddresses);
    if (ExtraStorage.BumpPtr - ExtraStorage.Addr - ExtraStorage.Size <
        FrameHdrCopy.size()) {
      errs() << "FLO fatal error: __flo_storage in this binary has not enough "
                "free space\n";
      exit(1);
    }

    uint64_t HdrFileOffset =
        ExtraStorage.BumpPtr - ExtraStorage.Addr + ExtraStorage.FileOffset;
    Out->os().pwrite(FrameHdrCopy.data(), FrameHdrCopy.size(), HdrFileOffset);
    outs() << "FLO: patching EH_FRAME program segment to reflect new "
              ".eh_frame_hdr\n";
    if (auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(File)) {
      auto Obj = ELF64LEFile->getELFFile();
      if (!patchEhFrameHdrSegment(Obj, &Out->os(), HdrFileOffset,
                                  ExtraStorage.BumpPtr, FrameHdrCopy.size())) {
        outs() << "FAILED to patch program segment!\n";
      }
    } else {
      outs() << "FLO-ERROR: program segment NOT patched -- I don't know how to "
                "handle this object file!\n";
    }
  }

  // Write all non-code sections.
  for(auto &SMII : SectionMM->SectionMapInfo) {
    SectionInfo &SI = SMII.second;
    if (SI.IsCode)
      continue;
    outs() << "FLO: writing new section " << SMII.first << '\n';
    Out->os().pwrite(reinterpret_cast<const char *>(SI.AllocAddress),
                     SI.Size,
                     SI.FileOffset);

    // Update ELF section header.
  }

  if (TotalScore != 0) {
    double Coverage = OverwrittenScore / (double)TotalScore * 100.0;
    outs() << format("FLO: Rewritten functions cover %.2lf", Coverage)
           << "% of the execution count of simple functions of this binary.\n";
  }

  // TODO: we should find a way to mark the binary as optimized by us.
  Out->keep();
}
