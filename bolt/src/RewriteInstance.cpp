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

#include "RewriteInstance.h"
#include "BinaryBasicBlock.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "BinaryPassManager.h"
#include "BoltAddressTranslation.h"
#include "CacheMetrics.h"
#include "DWARFRewriter.h"
#include "DataAggregator.h"
#include "DataReader.h"
#include "Exceptions.h"
#include "ExecutableFileMemoryManager.h"
#include "MCPlusBuilder.h"
#include "NameResolver.h"
#include "ParallelUtilities.h"
#include "Passes/ReorderFunctions.h"
#include "ProfileReader.h"
#include "ProfileWriter.h"
#include "Relocation.h"
#include "Utils.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <fstream>
#include <stack>
#include <system_error>
#include <thread>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace object;
using namespace bolt;

extern cl::opt<uint32_t> X86AlignBranchBoundary;
extern cl::opt<bool> X86AlignBranchWithin32BBoundaries;

namespace opts {

extern bool HeatmapMode;

extern cl::OptionCategory BoltCategory;
extern cl::OptionCategory BoltDiffCategory;
extern cl::OptionCategory BoltOptCategory;
extern cl::OptionCategory BoltOutputCategory;
extern cl::OptionCategory AggregatorCategory;

extern cl::opt<MacroFusionType> AlignMacroOpFusion;
extern cl::opt<JumpTableSupportLevel> JumpTables;
extern cl::list<std::string> ReorderData;
extern cl::opt<bolt::ReorderFunctions::ReorderType> ReorderFunctions;
extern cl::opt<bool> TimeBuild;

cl::opt<bool>
Instrument("instrument",
  cl::desc("instrument code to generate accurate profile data"),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

static cl::opt<std::string>
RuntimeInstrumentationLib("runtime-instrumentation-lib",
    cl::desc("specify file name of the runtime instrumentation library"),
    cl::ZeroOrMore,
    cl::init("libbolt_rt.a"),
    cl::cat(BoltOptCategory));

static cl::opt<bool>
ForceToDataRelocations("force-data-relocations",
  cl::desc("force relocations to data sections to always be processed"),
  cl::init(false),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

// Note: enabling this is liable to make things break.
static cl::opt<bool>
AllowSectionRelocations("allow-section-relocations",
  cl::desc("allow reordering of data referenced by section relocations "
           "(experimental)"),
  cl::init(false),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

static cl::opt<bool>
PrintCacheMetrics("print-cache-metrics",
  cl::desc("calculate and print various metrics for instruction cache"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

cl::opt<std::string>
OutputFilename("o",
  cl::desc("<output file>"),
  cl::Optional,
  cl::cat(BoltOutputCategory));

cl::opt<bool>
AllowStripped("allow-stripped",
  cl::desc("allow processing of stripped binaries"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<std::string>
BoltProfile("b",
  cl::desc("<bolt profile>"),
  cl::cat(BoltCategory));

static cl::list<std::string>
BreakFunctionNames("break-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to core dump on (debugging)"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
DumpDotAll("dump-dot-all",
  cl::desc("dump function CFGs to graphviz format after each stage"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
DumpEHFrame("dump-eh-frame",
  cl::desc("dump parsed .eh_frame (debugging)"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
FunctionNames("funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to optimize"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<std::string>
FunctionNamesFile("funcs-file",
  cl::desc("file with list of functions to optimize"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
FunctionPadSpec("pad-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to pad with amount of bytes"),
  cl::value_desc("func1:pad1,func2:pad2,func3:pad3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
HotFunctionsAtEnd(
  "hot-functions-at-end",
  cl::desc(
      "if reorder-functions is used, order functions putting hottest last"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
HotText("hot-text",
  cl::desc("hot text symbols support (relocation mode)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::list<std::string>
HotTextMoveSections("hot-text-move-sections",
  cl::desc("list of sections containing functions used for hugifying hot text. "
           "BOLT makes sure these functions are not placed on the same page as "
           "the hot text. (default=\'.stub,.mover\')."),
  cl::value_desc("sec1,sec2,sec3,..."),
  cl::CommaSeparated,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

cl::opt<bool>
HotData("hot-data",
  cl::desc("hot data symbols support (relocation mode)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
UpdateEnd("update-end",
  cl::desc("update the _end symbol to point to the end of all data sections"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
KeepTmp("keep-tmp",
  cl::desc("preserve intermediate .o file"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
MarkFuncs("mark-funcs",
  cl::desc("mark function boundaries with break instruction to make "
           "sure we accidentally don't cross them"),
  cl::ReallyHidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<unsigned>
MaxFunctions("max-funcs",
  cl::desc("maximum number of functions to overwrite"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<unsigned>
MaxDataRelocations("max-data-relocations",
  cl::desc("maximum number of data relocations to process"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
PrintAll("print-all",
  cl::desc("print functions after each stage"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
PrintCFG("print-cfg",
  cl::desc("print functions after CFG construction"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool> PrintDisasm("print-disasm",
  cl::desc("print function after disassembly"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintGlobals("print-globals",
  cl::desc("print global symbols after disassembly"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

extern cl::opt<bool> PrintSections;

static cl::opt<bool>
PrintLoopInfo("print-loops",
  cl::desc("print loop related information"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintSDTMarkers("print-sdt",
  cl::desc("print all SDT markers"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<cl::boolOrDefault>
RelocationMode("relocs",
  cl::desc("use relocations in the binary (default=autodetect)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<std::string>
SaveProfile("w",
  cl::desc("save recorded profile to a file"),
  cl::cat(BoltOutputCategory));

static cl::list<std::string>
SkipFunctionNames("skip-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to skip"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<std::string>
SkipFunctionNamesFile("skip-funcs-file",
  cl::desc("file with list of functions to skip"),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
SplitEH("split-eh",
  cl::desc("split C++ exception handling code"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltOptCategory));

cl::opt<bool>
StrictMode("strict",
  cl::desc("trust the input to be from a well-formed source"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

cl::opt<bool>
TrapOldCode("trap-old-code",
  cl::desc("insert traps in old function bodies (relocation mode)"),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
UpdateDebugSections("update-debug-sections",
  cl::desc("update DWARF debug sections of the executable"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

cl::opt<bool>
EnableBAT("enable-bat",
  cl::desc("write BOLT Address Translation tables"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
UseGnuStack("use-gnu-stack",
  cl::desc("use GNU_STACK program header for new segment (workaround for "
           "issues with strip/objcopy)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

cl::opt<bool>
UseOldText("use-old-text",
  cl::desc("re-use space in old .text if possible (relocation mode)"),
  cl::cat(BoltCategory));

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
  cl::ZeroOrMore,
  cl::cat(BoltCategory),
  cl::sub(*cl::AllSubCommands));

cl::opt<bool>
AggregateOnly("aggregate-only",
  cl::desc("exit after writing aggregated data file"),
  cl::Hidden,
  cl::cat(AggregatorCategory));

cl::opt<bool>
DiffOnly("diff-only",
  cl::desc("stop processing once we have enough to compare two binaries"),
  cl::Hidden,
  cl::cat(BoltDiffCategory));

static cl::opt<bool>
TimeRewrite("time-rewrite",
  cl::desc("print time spent in rewriting passes"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
SequentialDisassembly("sequential-disassembly",
  cl::desc("performs disassembly sequentially"),
  cl::init(false),
  cl::cat(BoltOptCategory));

static cl::opt<bool>
WriteBoltInfoSection("bolt-info",
  cl::desc("write bolt info section in the output binary"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltOutputCategory));

static cl::opt<bool>
X86AlignBranchBoundaryHotOnly("x86-align-branch-boundary-hot-only",
  cl::desc("only apply branch boundary alignment in hot code"),
  cl::init(true),
  cl::cat(BoltOptCategory));

bool isHotTextMover(const BinaryFunction &Function) {
  for (auto &SectionName : opts::HotTextMoveSections) {
    if (Function.getOriginSectionName() == SectionName)
      return true;
  }

  return false;
}

// Check against lists of functions from options if we should
// optimize the function with a given name.
bool shouldProcess(const BinaryFunction &Function) {
  if (opts::MaxFunctions &&
      Function.getFunctionNumber() >= opts::MaxFunctions) {
    if (Function.getFunctionNumber() == opts::MaxFunctions) {
      outs() << "BOLT-INFO: processing ending on " << Function << "\n";
    } else {
      return false;
    }
  }

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
      if (Function.hasNameRegex(Name)) {
        IsValid = true;
        break;
      }
    }
  }
  if (!IsValid)
    return false;

  for (auto &Name : SkipFunctionNames) {
    if (Function.hasNameRegex(Name))
      return false;
  }

  return true;
}

size_t padFunction(const BinaryFunction &Function) {
  static std::map<std::string, size_t> FunctionPadding;

  if (FunctionPadding.empty() && !FunctionPadSpec.empty()) {
    for (auto &Spec : FunctionPadSpec) {
      auto N = Spec.find(':');
      if (N == std::string::npos)
        continue;
      auto Name = Spec.substr(0, N);
      auto Padding = std::stoull(Spec.substr(N+1));
      FunctionPadding[Name] = Padding;
    }
  }

  for (auto &FPI : FunctionPadding) {
    auto Name = FPI.first;
    auto Padding = FPI.second;
    if (Function.hasNameRegex(Name)) {
      return Padding;
    }
  }

  return 0;
}

} // namespace opts

constexpr const char *RewriteInstance::SectionsToOverwrite[];
constexpr const char *RewriteInstance::DebugSectionsToOverwrite[];

const std::string RewriteInstance::OrgSecPrefix = ".bolt.org";

const std::string RewriteInstance::BOLTSecPrefix = ".bolt";

const char RewriteInstance::TimerGroupName[] = "rewrite";
const char RewriteInstance::TimerGroupDesc[] = "Rewrite passes";

namespace llvm {
namespace bolt {

extern const char *BoltRevision;

} // namespace bolt
} // namespace llvm

namespace {

bool refersToReorderedSection(ErrorOr<BinarySection &> Section) {
  auto Itr = std::find_if(opts::ReorderData.begin(),
                          opts::ReorderData.end(),
                          [&](const std::string &SectionName) {
                            return (Section &&
                                    Section->getName() == SectionName);
                          });
  return Itr != opts::ReorderData.end();
}

} // namespace

RewriteInstance::RewriteInstance(ELFObjectFileBase *File, DataReader &DR,
                                 DataAggregator &DA, const int Argc,
                                 const char *const *Argv, StringRef ToolPath)
    : InputFile(File), Argc(Argc), Argv(Argv), ToolPath(ToolPath), DA(DA),
      BC(BinaryContext::createBinaryContext(
          File, DR,
          DWARFContext::create(*File, nullptr,
                               DWARFContext::defaultErrorHandler, "", false))),
      BAT(llvm::make_unique<BoltAddressTranslation>(*BC)),
      SHStrTab(StringTableBuilder::ELF) {
  if (opts::UpdateDebugSections) {
    DebugInfoRewriter = llvm::make_unique<DWARFRewriter>(*BC, SectionPatchers);
  }
}

RewriteInstance::~RewriteInstance() {}

bool RewriteInstance::shouldDisassemble(const BinaryFunction &BF) const {
  // If we have to relocate the code we have to disassemble all functions.
  if (!BF.getBinaryContext().HasRelocations && !opts::shouldProcess(BF)) {
    DEBUG(dbgs() << "BOLT: skipping processing function " << BF
                 << " per user request.\n");
    return false;
  }

  // If we are running in profile conversion mode and there is no profile
  // available for the function, we can skip the disassembly.
  // However, in strict relocation mode we need to account for all
  // functions. Also, when multi-threading is enabled, the profile may not be
  // available yet, and we conservatively disassemble the function.
  if (opts::AggregateOnly && opts::NoThreads && !opts::StrictMode &&
      !BF.hasProfileAvailable())
    return false;

  return true;
}

void RewriteInstance::discoverStorage() {
  NamedRegionTimer T("discoverStorage", "discover storage", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);

  // Stubs are harmful because RuntimeDyld may try to increase the size of
  // sections accounting for stubs when we need those sections to match the
  // same size seen in the input binary, in case this section is a copy
  // of the original one seen in the binary.
  BC->EFMM.reset(new ExecutableFileMemoryManager(*BC, /*AllowStubs*/ false));

  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  if (Obj->getHeader()->e_type != ELF::ET_EXEC) {
    outs() << "BOLT-INFO: shared object or position-independent executable "
              "detected\n";
    BC->HasFixedLoadAddress = false;
  }

  EntryPoint = Obj->getHeader()->e_entry;

  NextAvailableAddress = 0;
  uint64_t NextAvailableOffset = 0;
  auto PHs = cantFail(Obj->program_headers(), "program_headers() failed");
  for (const auto &Phdr : PHs) {
    if (Phdr.p_type == ELF::PT_LOAD) {
      BC->FirstAllocAddress = std::min(BC->FirstAllocAddress,
                                       static_cast<uint64_t>(Phdr.p_vaddr));
      NextAvailableAddress = std::max(NextAvailableAddress,
                                      Phdr.p_vaddr + Phdr.p_memsz);
      NextAvailableOffset = std::max(NextAvailableOffset,
                                     Phdr.p_offset + Phdr.p_filesz);

      BC->EFMM->SegmentMapInfo[Phdr.p_vaddr] = SegmentInfo{Phdr.p_vaddr,
                                                           Phdr.p_memsz,
                                                           Phdr.p_offset,
                                                           Phdr.p_filesz,
                                                           Phdr.p_align};
    }
  }

  for (const auto &Section : InputFile->sections()) {
    StringRef SectionName;
    Section.getName(SectionName);
    StringRef SectionContents;
    Section.getContents(SectionContents);
    if (SectionName == ".text") {
      BC->OldTextSectionAddress = Section.getAddress();
      BC->OldTextSectionSize = Section.getSize();
      BC->OldTextSectionOffset =
        SectionContents.data() - InputFile->getData().data();
    }

    if (!opts::HeatmapMode &&
        !(opts::AggregateOnly && BAT->enabledFor(InputFile)) &&
        (SectionName.startswith(OrgSecPrefix) ||
         SectionName.startswith(BOLTSecPrefix))) {
      errs() << "BOLT-ERROR: input file was processed by BOLT. "
                "Cannot re-optimize.\n";
      exit(1);
    }
  }

  assert(NextAvailableAddress && NextAvailableOffset &&
         "no PT_LOAD pheader seen");

  outs() << "BOLT-INFO: first alloc address is 0x"
         << Twine::utohexstr(BC->FirstAllocAddress) << '\n';

  FirstNonAllocatableOffset = NextAvailableOffset;

  NextAvailableAddress = alignTo(NextAvailableAddress, BC->PageAlign);
  NextAvailableOffset = alignTo(NextAvailableOffset, BC->PageAlign);

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
    if (NextAvailableOffset <= NextAvailableAddress - BC->FirstAllocAddress) {
      NextAvailableOffset = NextAvailableAddress - BC->FirstAllocAddress;
    } else {
      NextAvailableAddress = NextAvailableOffset + BC->FirstAllocAddress;
    }
    assert(NextAvailableOffset == NextAvailableAddress - BC->FirstAllocAddress
           && "PHDR table address calculation error");

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
  NextAvailableAddress = alignTo(NextAvailableAddress, 64);
  NextAvailableOffset = alignTo(NextAvailableOffset, 64);

  NewTextSegmentAddress = NextAvailableAddress;
  NewTextSegmentOffset = NextAvailableOffset;
  BC->LayoutStartAddress = NextAvailableAddress;
}

void RewriteInstance::parseSDTNotes() {
  if (!SDTSection)
    return;

  StringRef Buf = SDTSection->getContents();
  auto DE = DataExtractor(Buf, BC->AsmInfo->isLittleEndian(),
                          BC->AsmInfo->getCodePointerSize());
  uint32_t Offset = 0;

  while (DE.isValidOffset(Offset)) {
    auto NameSz = DE.getU32(&Offset);
    DE.getU32(&Offset); // skip over DescSz
    auto Type = DE.getU32(&Offset);
    Offset = alignTo(Offset, 4);

    if (Type != 3)
      errs() << "BOLT-WARNING: SDT note type \"" << Type
             << "\" is not expected\n";

    if (NameSz == 0)
      errs() << "BOLT-WARNING: SDT note has empty name\n";

    StringRef Name = DE.getCStr(&Offset);

    if (!Name.equals("stapsdt"))
      errs() << "BOLT-WARNING: SDT note name \"" << Name
             << "\" is not expected\n";

    // Parse description
    SDTMarkerInfo Marker;
    Marker.PCOffset = Offset;
    Marker.PC = DE.getU64(&Offset);
    Marker.Base = DE.getU64(&Offset);
    Marker.Semaphore = DE.getU64(&Offset);
    Marker.Provider = DE.getCStr(&Offset);
    Marker.Name = DE.getCStr(&Offset);
    Marker.Args = DE.getCStr(&Offset);
    Offset = alignTo(Offset, 4);
    BC->SDTMarkers[Marker.PC] = Marker;
  }

  if (opts::PrintSDTMarkers)
    printSDTMarkers();
}

void RewriteInstance::printSDTMarkers() {
  outs() << "BOLT-INFO: Number of SDT markers is " << BC->SDTMarkers.size()
         << "\n";
  for (auto It : BC->SDTMarkers) {
    auto &Marker = It.second;
    outs() << "BOLT-INFO: PC: " << utohexstr(Marker.PC)
           << ", Base: " << utohexstr(Marker.Base)
           << ", Semaphore: " << utohexstr(Marker.Semaphore)
           << ", Provider: " << Marker.Provider << ", Name: " << Marker.Name
           << ", Args: " << Marker.Args << "\n";
  }
}

void RewriteInstance::parseBuildID() {
  if (!BuildIDSection)
    return;

  StringRef Buf = BuildIDSection->getContents();

  // Reading notes section (see Portable Formats Specification, Version 1.1,
  // pg 2-5, section "Note Section").
  DataExtractor DE = DataExtractor(Buf, true, 8);
  uint32_t Offset = 0;
  if (!DE.isValidOffset(Offset))
    return;
  uint32_t NameSz = DE.getU32(&Offset);
  if (!DE.isValidOffset(Offset))
    return;
  uint32_t DescSz = DE.getU32(&Offset);
  if (!DE.isValidOffset(Offset))
    return;
  uint32_t Type = DE.getU32(&Offset);

  DEBUG(dbgs() << "NameSz = " << NameSz << "; DescSz = " << DescSz
               << "; Type = " << Type << "\n");

  // Type 3 is a GNU build-id note section
  if (Type != 3)
    return;

  StringRef Name = Buf.slice(Offset, Offset + NameSz);
  Offset = alignTo(Offset + NameSz, 4);
  if (Name.substr(0, 3) != "GNU")
    return;

  BuildID = Buf.slice(Offset, Offset + DescSz);
}

Optional<std::string> RewriteInstance::getPrintableBuildID() const {
  if (BuildID.empty())
    return NoneType();

  std::string Str;
  raw_string_ostream OS(Str);
  auto CharIter = BuildID.bytes_begin();
  while (CharIter != BuildID.bytes_end()) {
    if (*CharIter < 0x10)
      OS << "0";
    OS << Twine::utohexstr(*CharIter);
    ++CharIter;
  }
  return OS.str();
}

void RewriteInstance::patchBuildID() {
  auto &OS = Out->os();

  if (BuildID.empty())
    return;

  size_t IDOffset = BuildIDSection->getContents().rfind(BuildID);
  assert(IDOffset != StringRef::npos && "failed to patch build-id");

  auto FileOffset = getFileOffsetForAddress(BuildIDSection->getAddress());
  if (!FileOffset) {
    errs() << "BOLT-WARNING: Non-allocatable build-id will not be updated.\n";
    return;
  }

  char LastIDByte = BuildID[BuildID.size() - 1];
  LastIDByte ^= 1;
  OS.pwrite(&LastIDByte, 1, FileOffset + IDOffset + BuildID.size() - 1);

  outs() << "BOLT-INFO: patched build-id (flipped last bit)\n";
}

void RewriteInstance::run() {
  if (!BC) {
    errs() << "BOLT-ERROR: failed to create a binary context\n";
    return;
  }

  outs() << "BOLT-INFO: Target architecture: "
         << Triple::getArchTypeName(
                (llvm::Triple::ArchType)InputFile->getArch())
         << "\n";

  discoverStorage();
  readSpecialSections();
  adjustCommandLineOptions();
  discoverFileObjects();

  std::thread PreProcessProfileThread([&]() {
    if (!DA.started())
      return;

    outs() << "BOLT-INFO: spawning thread to pre-process profile\n";
    preprocessProfileData();
  });

  if (opts::NoThreads)
    PreProcessProfileThread.join();

  readDebugInfo();

  // Skip disassembling if we have a translation table and we are running an
  // aggregation job.
  if (!opts::AggregateOnly || !BAT->enabledFor(InputFile)) {
    disassembleFunctions();
  }

  if (PreProcessProfileThread.joinable())
    PreProcessProfileThread.join();

  processProfileData();

  if (opts::AggregateOnly)
    return;

  postProcessFunctions();

  if (opts::DiffOnly)
    return;

  runOptimizationPasses();

  emitAndLink();

  updateMetadata();

  if (opts::WriteBoltInfoSection)
    addBoltInfoSection();

  // Copy allocatable part of the input.
  std::error_code EC;
  Out = llvm::make_unique<ToolOutputFile>(opts::OutputFilename, EC,
                                          sys::fs::F_None, 0777);
  check_error(EC, "cannot create output executable file");
  Out->os() << InputFile->getData().substr(0, FirstNonAllocatableOffset);

  // Rewrite allocatable contents and copy non-allocatable parts with mods.
  rewriteFile();
}

void RewriteInstance::discoverFileObjects() {
  NamedRegionTimer T("discoverFileObjects", "discover file objects",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);
  NameResolver NR;

  FileSymRefs.clear();
  BC->getBinaryFunctions().clear();
  BC->clearBinaryData();

  // For local symbols we want to keep track of associated FILE symbol name for
  // disambiguation by combined name.
  StringRef  FileSymbolName;
  bool SeenFileName = false;
  struct SymbolRefHash {
    std::size_t operator()(SymbolRef const &S) const {
      return std::hash<decltype(DataRefImpl::p)>{}(S.getRawDataRefImpl().p);
    }
  };
  std::unordered_map<SymbolRef, StringRef, SymbolRefHash> SymbolToFileName;
  for (const auto &Symbol : InputFile->symbols()) {
    auto NameOrError = Symbol.getName();
    if (NameOrError && NameOrError->startswith("__asan_init")) {
      errs() << "BOLT-ERROR: input file was compiled or linked with sanitizer "
                "support. Cannot optimize.\n";
      exit(1);
    }
    if (NameOrError && NameOrError->startswith("__llvm_coverage_mapping")) {
      errs() << "BOLT-ERROR: input file was compiled or linked with coverage "
                "support. Cannot optimize.\n";
      exit(1);
    }

    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    if (cantFail(Symbol.getType()) == SymbolRef::ST_File) {
      auto Name =
          cantFail(std::move(NameOrError), "cannot get symbol name for file");
      // Ignore Clang LTO artificial FILE symbol as it is not always generated,
      // and this uncertainty is causing havoc in function name matching.
      if (Name == "ld-temp.o")
        continue;
      FileSymbolName = Name;
      SeenFileName = true;
      continue;
    }
    if (!FileSymbolName.empty() &&
        !(Symbol.getFlags() & SymbolRef::SF_Global)) {
      SymbolToFileName[Symbol] = FileSymbolName;
    }
  }

  // Sort symbols in the file by value. Ignore symbols from non-allocatable
  // sections.
  auto isSymbolInMemory = [this](const SymbolRef &Sym) {
    if (cantFail(Sym.getType()) == SymbolRef::ST_File)
      return false;
    if (Sym.getFlags() & SymbolRef::SF_Absolute)
      return true;
    if (Sym.getFlags() & SymbolRef::SF_Undefined)
      return false;
    BinarySection Section(*BC, *cantFail(Sym.getSection()));
    return Section.isAllocatable();
  };
  std::vector<SymbolRef> SortedFileSymbols;
  std::copy_if(InputFile->symbol_begin(), InputFile->symbol_end(),
               std::back_inserter(SortedFileSymbols),
               isSymbolInMemory);

  std::stable_sort(SortedFileSymbols.begin(), SortedFileSymbols.end(),
                   [](const SymbolRef &A, const SymbolRef &B) {
                     // FUNC symbols have the highest precedence, while SECTIONs
                     // have the lowest.
                     auto AddressA = cantFail(A.getAddress());
                     auto AddressB = cantFail(B.getAddress());
                     if (AddressA != AddressB)
                      return AddressA < AddressB;

                     auto AType = cantFail(A.getType());
                     auto BType = cantFail(B.getType());
                     if (AType == SymbolRef::ST_Function &&
                         BType != SymbolRef::ST_Function)
                       return true;
                     if (BType == SymbolRef::ST_Debug &&
                         AType != SymbolRef::ST_Debug)
                       return true;

                     return false;
                   });

  // For aarch64, the ABI defines mapping symbols so we identify data in the
  // code section (see IHI0056B). $d identifies data contents.
  auto LastSymbol = SortedFileSymbols.end() - 1;
  if (BC->isAArch64()) {
    LastSymbol = std::stable_partition(
        SortedFileSymbols.begin(), SortedFileSymbols.end(),
        [](const SymbolRef &Symbol) {
          StringRef Name = cantFail(Symbol.getName());
          return !(cantFail(Symbol.getType()) == SymbolRef::ST_Unknown &&
                   (Name == "$d" || Name == "$x"));
        });
    --LastSymbol;
  }

  auto getNextAddress = [&](std::vector<SymbolRef>::const_iterator Itr) {
    const auto SymbolSection = cantFail(Itr->getSection());
    const auto SymbolAddress = cantFail(Itr->getAddress());
    const auto SymbolEndAddress = SymbolAddress + ELFSymbolRef(*Itr).getSize();

    // absolute sym
    if (SymbolSection == InputFile->section_end())
      return SymbolEndAddress;

    while (Itr != LastSymbol &&
           cantFail(std::next(Itr)->getSection()) == SymbolSection &&
           cantFail(std::next(Itr)->getAddress()) == SymbolAddress) {
      ++Itr;
    }

    if (Itr != LastSymbol &&
        cantFail(std::next(Itr)->getSection()) == SymbolSection)
      return cantFail(std::next(Itr)->getAddress());

    const auto SymbolSectionEndAddress =
      SymbolSection->getAddress() + SymbolSection->getSize();
    if ((ELFSectionRef(*SymbolSection).getFlags() & ELF::SHF_TLS) ||
        SymbolEndAddress > SymbolSectionEndAddress)
      return SymbolEndAddress;

    return SymbolSectionEndAddress;
  };

  BinaryFunction *PreviousFunction = nullptr;
  unsigned AnonymousId = 0;

  const auto MarkersBegin = std::next(LastSymbol);
  for (auto ISym = SortedFileSymbols.begin(); ISym != MarkersBegin; ++ISym) {
    const auto &Symbol = *ISym;
    // Keep undefined symbols for pretty printing?
    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    const auto SymbolType = cantFail(Symbol.getType());

    if (SymbolType == SymbolRef::ST_File)
      continue;

    StringRef SymName = cantFail(Symbol.getName(), "cannot get symbol name");
    uint64_t Address =
        cantFail(Symbol.getAddress(), "cannot get symbol address");
    if (Address == 0) {
      if (opts::Verbosity >= 1 && SymbolType == SymbolRef::ST_Function)
        errs() << "BOLT-WARNING: function with 0 address seen\n";
      continue;
    }

    FileSymRefs[Address] = Symbol;

    /// It is possible we are seeing a globalized local. LLVM might treat it as
    /// a local if it has a "private global" prefix, e.g. ".L". Thus we have to
    /// change the prefix to enforce global scope of the symbol.
    std::string Name = SymName.startswith(BC->AsmInfo->getPrivateGlobalPrefix())
                           ? "PG" + std::string(SymName)
                           : std::string(SymName);

    // Disambiguate all local symbols before adding to symbol table.
    // Since we don't know if we will see a global with the same name,
    // always modify the local name.
    //
    // NOTE: the naming convention for local symbols should match
    //       the one we use for profile data.
    std::string UniqueName;
    std::string AlternativeName;
    if (Name.empty()) {
      // Symbols that will be registered by disassemblePLT()
      if ((PLTSection && PLTSection->getAddress() == Address) ||
          (PLTGOTSection && PLTGOTSection->getAddress() == Address)) {
        continue;
      }
      UniqueName = "ANONYMOUS." + std::to_string(AnonymousId++);
    } else if (Symbol.getFlags() & SymbolRef::SF_Global) {
      assert(!BC->getBinaryDataByName(Name) && "global name not unique");
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
      std::string AltPrefix;
      auto SFI = SymbolToFileName.find(Symbol);
      if (SymbolType == SymbolRef::ST_Function &&
          SFI != SymbolToFileName.end()) {
        AltPrefix = Name + "/" + std::string(SFI->second);
      }

      UniqueName = NR.uniquify(Name);
      if (!AltPrefix.empty())
        AlternativeName = NR.uniquify(AltPrefix);
    }

    uint64_t SymbolSize = ELFSymbolRef(Symbol).getSize();
    uint64_t TentativeSize = SymbolSize ? SymbolSize
                                        : getNextAddress(ISym) - Address;
    uint64_t SymbolAlignment = Symbol.getAlignment();
    unsigned SymbolFlags = Symbol.getFlags();

    auto registerName = [&](uint64_t FinalSize) {
      // Register names even if it's not a function, e.g. for an entry point.
      BC->registerNameAtAddress(UniqueName, Address, FinalSize,
                                SymbolAlignment, SymbolFlags);
      if (!AlternativeName.empty())
        BC->registerNameAtAddress(AlternativeName, Address, FinalSize,
                                  SymbolAlignment, SymbolFlags);
    };

    section_iterator Section =
        cantFail(Symbol.getSection(), "cannot get symbol section");
    if (Section == InputFile->section_end()) {
      // Could be an absolute symbol. Could record for pretty printing.
      DEBUG(if (opts::Verbosity > 1) {
          dbgs() << "BOLT-INFO: absolute sym " << UniqueName << "\n";
        });
      registerName(TentativeSize);
      continue;
    }

    DEBUG(dbgs() << "BOLT-DEBUG: considering symbol " << UniqueName
                 << " for function\n");

    if (!Section->isText()) {
      assert(SymbolType != SymbolRef::ST_Function &&
             "unexpected function inside non-code section");
      DEBUG(dbgs() << "BOLT-DEBUG: rejecting as symbol is not in code\n");
      registerName(TentativeSize);
      continue;
    }

    // Assembly functions could be ST_NONE with 0 size. Check that the
    // corresponding section is a code section and they are not inside any
    // other known function to consider them.
    //
    // Sometimes assembly functions are not marked as functions and neither are
    // their local labels. The only way to tell them apart is to look at
    // symbol scope - global vs local.
    if (PreviousFunction && SymbolType != SymbolRef::ST_Function) {
      if (PreviousFunction->containsAddress(Address)) {
        if (PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
          DEBUG(dbgs() << "BOLT-DEBUG: symbol is a function local symbol\n");
        } else if (Address == PreviousFunction->getAddress() && !SymbolSize) {
          DEBUG(dbgs() << "BOLT-DEBUG: ignoring symbol as a marker\n");
        } else if (opts::Verbosity > 1) {
          errs() << "BOLT-WARNING: symbol " << UniqueName
                 << " seen in the middle of function "
                 << *PreviousFunction << ". Could be a new entry.\n";
        }
        registerName(SymbolSize);
        continue;
      } else if (PreviousFunction->getSize() == 0 &&
                 PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
        DEBUG(dbgs() << "BOLT-DEBUG: symbol is a function local symbol\n");
        registerName(SymbolSize);
        continue;
      }
    }

    if (PreviousFunction &&
        PreviousFunction->containsAddress(Address) &&
        PreviousFunction->getAddress() != Address) {
      if (PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
        if (opts::Verbosity >= 1) {
          outs() << "BOLT-DEBUG: possibly another entry for function "
                 << *PreviousFunction << " : " << UniqueName << '\n';
        }
      } else {
        outs() << "BOLT-INFO: using " << UniqueName << " as another entry to "
               << "function " << *PreviousFunction << '\n';

        PreviousFunction->
          addEntryPointAtOffset(Address - PreviousFunction->getAddress());

        if (!BC->HasRelocations)
          PreviousFunction->setSimple(false);

        // Remove the symbol from FileSymRefs so that we can skip it from
        // in the future.
        auto SI = FileSymRefs.find(Address);
        assert(SI != FileSymRefs.end() && "symbol expected to be present");
        assert(SI->second == Symbol && "wrong symbol found");
        FileSymRefs.erase(SI);
      }
      registerName(SymbolSize);
      continue;
    }

    // Checkout for conflicts with function data from FDEs.
    bool IsSimple = true;
    auto FDEI = CFIRdWrt->getFDEs().lower_bound(Address);
    if (FDEI != CFIRdWrt->getFDEs().end()) {
      const auto &FDE = *FDEI->second;
      if (FDEI->first != Address) {
        // There's no matching starting address in FDE. Make sure the previous
        // FDE does not contain this address.
        if (FDEI != CFIRdWrt->getFDEs().begin()) {
          --FDEI;
          auto &PrevFDE = *FDEI->second;
          auto PrevStart = PrevFDE.getInitialLocation();
          auto PrevLength = PrevFDE.getAddressRange();
          if (Address > PrevStart && Address < PrevStart + PrevLength) {
            errs() << "BOLT-ERROR: function " << UniqueName
                   << " is in conflict with FDE ["
                   << Twine::utohexstr(PrevStart) << ", "
                   << Twine::utohexstr(PrevStart + PrevLength)
                   << "). Skipping.\n";
            IsSimple = false;
          }
        }
      } else if (FDE.getAddressRange() != SymbolSize) {
        if (SymbolSize) {
          // Function addresses match but sizes differ.
          errs() << "BOLT-WARNING: sizes differ for function " << UniqueName
                 << ". FDE : " << FDE.getAddressRange()
                 << "; symbol table : " << SymbolSize << ". Using max size.\n";
        }
        SymbolSize = std::max(SymbolSize, FDE.getAddressRange());
        if (BC->getBinaryDataAtAddress(Address)) {
          BC->setBinaryDataSize(Address, SymbolSize);
        } else {
          DEBUG(dbgs() << "BOLT-DEBUG: No BD @ 0x"
                       << Twine::utohexstr(Address) << "\n");
        }
      }
    }

    BinaryFunction *BF{nullptr};
    // Since function may not have yet obtained its real size, do a search
    // using the list of registered functions instead of calling
    // getBinaryFunctionAtAddress().
    auto BFI = BC->getBinaryFunctions().find(Address);
    if (BFI != BC->getBinaryFunctions().end()) {
      BF = &BFI->second;
      // Duplicate the function name. Make sure everything matches before we add
      // an alternative name.
      if (SymbolSize != BF->getSize()) {
        if (opts::Verbosity >= 1) {
          if (SymbolSize && BF->getSize()) {
            errs() << "BOLT-WARNING: size mismatch for duplicate entries "
                   << *BF << " and " << UniqueName << '\n';
          }
          outs() << "BOLT-INFO: adjusting size of function " << *BF
                 << " old " << BF->getSize() << " new " << SymbolSize << "\n";
        }
        BF->setSize(std::max(SymbolSize, BF->getSize()));
        BC->setBinaryDataSize(Address, BF->getSize());
      }
      BF->addAlternativeName(UniqueName);
    } else {
      auto Section = BC->getSectionForAddress(Address);
      assert(Section && "section for functions must be registered");

      // Skip symbols from zero-sized sections.
      if (!Section->getSize())
        continue;

      BF = BC->createBinaryFunction(UniqueName, *Section, Address,
                                    SymbolSize, IsSimple);
    }
    if (!AlternativeName.empty())
      BF->addAlternativeName(AlternativeName);

    registerName(SymbolSize);
    PreviousFunction = BF;
  }

  // Process PLT section.
  if (BC->TheTriple->getArch() == Triple::x86_64)
    disassemblePLT();

  // See if we missed any functions marked by FDE.
  for (const auto &FDEI : CFIRdWrt->getFDEs()) {
    const auto Address = FDEI.first;
    const auto *FDE = FDEI.second;
    const auto *BF = BC->getBinaryFunctionAtAddress(Address);
    if (BF)
      continue;

    BF = BC->getBinaryFunctionContainingAddress(Address);
    if (BF) {
      errs() << "BOLT-WARNING: FDE [0x" << Twine::utohexstr(Address) << ", 0x"
             << Twine::utohexstr(Address + FDE->getAddressRange())
             << ") conflicts with function " << *BF << '\n';
      continue;
    }

    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: FDE [0x" << Twine::utohexstr(Address)
             << ", 0x" << Twine::utohexstr(Address + FDE->getAddressRange())
             << ") has no corresponding symbol table entry\n";
    }
    auto Section = BC->getSectionForAddress(Address);
    assert(Section && "cannot get section for address from FDE");
    std::string FunctionName =
      "__BOLT_FDE_FUNCat" + Twine::utohexstr(Address).str();
    BC->createBinaryFunction(FunctionName, *Section, Address,
                             FDE->getAddressRange(), true);
  }

  if (!SeenFileName && BC->DR.hasLocalsWithFileName() && !opts::AllowStripped) {
    errs() << "BOLT-ERROR: input binary does not have local file symbols "
              "but profile data includes function names with embedded file "
              "names. It appears that the input binary was stripped while a "
              "profiled binary was not. If you know what you are doing and "
              "wish to proceed, use -allow-stripped option.\n";
    exit(1);
  }

  // Now that all the functions were created - adjust their boundaries.
  adjustFunctionBoundaries();

  // Annotate functions with code/data markers in AArch64
  for (auto ISym = MarkersBegin; ISym != SortedFileSymbols.end(); ++ISym) {
    const auto &Symbol = *ISym;
    uint64_t Address =
        cantFail(Symbol.getAddress(), "cannot get symbol address");
    auto SymbolSize = ELFSymbolRef(Symbol).getSize();
    auto *BF = BC->getBinaryFunctionContainingAddress(Address, true, true);
    if (!BF) {
      // Stray marker
      continue;
    }
    const auto EntryOffset = Address - BF->getAddress();
    if (BF->isCodeMarker(Symbol, SymbolSize)) {
      BF->markCodeAtOffset(EntryOffset);
      continue;
    }
    if (BF->isDataMarker(Symbol, SymbolSize)) {
      BF->markDataAtOffset(EntryOffset);
      BC->AddressToConstantIslandMap[Address] = BF;
      continue;
    }
    llvm_unreachable("Unknown marker");
  }

  if (!BC->HasRelocations)
    return;

  // Read all relocations now that we have binary functions mapped.
  std::vector<SectionRef> RelocationSections;
  for (const auto &Section : InputFile->sections()) {
    if (Section.getRelocatedSection() != InputFile->section_end())
      RelocationSections.push_back(Section);
  }

  // Sort relocation sections so that we process text section relocations last.
  std::stable_sort(RelocationSections.begin(), RelocationSections.end(),
                   [](const SectionRef &A, const SectionRef &B) {
                     if (!A.getRelocatedSection()->isText() &&
                         B.getRelocatedSection()->isText())
                       return true;
                     return false;
                   });

  for (const auto &Section : RelocationSections)
    readRelocations(Section, NR);
}

void RewriteInstance::disassemblePLT() {
  // Used to analyze both the .plt section (most common) and the less common
  // .plt.got created by the BFD linker.
  auto analyzeOnePLTSection = [&](BinarySection &Section,
                                  BinarySection *RelocsSection,
                                  uint64_t RelocType, uint64_t EntrySize) {
    const auto PLTAddress = Section.getAddress();
    StringRef PLTContents = Section.getContents();
    ArrayRef<uint8_t> PLTData(
        reinterpret_cast<const uint8_t *>(PLTContents.data()),
        Section.getSize());

    for (uint64_t Offset = 0; Offset < Section.getSize();
         Offset += EntrySize) {
      uint64_t InstrSize;
      MCInst Instruction;
      const uint64_t InstrAddr = PLTAddress + Offset;
      if (!BC->DisAsm->getInstruction(Instruction, InstrSize,
                                      PLTData.slice(Offset), InstrAddr, nulls(),
                                      nulls())) {
        errs() << "BOLT-ERROR: unable to disassemble instruction in PLT "
                  "section "
               << Section.getName() << " at offset 0x"
               << Twine::utohexstr(Offset) << '\n';
        exit(1);
      }

      if (!BC->MIB->isIndirectBranch(Instruction))
        continue;

      uint64_t TargetAddress;
      if (!BC->MIB->evaluateMemOperandTarget(Instruction, TargetAddress,
                                             InstrAddr, InstrSize)) {
        errs() << "BOLT-ERROR: error evaluating PLT instruction at offset 0x"
               << Twine::utohexstr(InstrAddr) << '\n';
        exit(1);
      }

      // To get the name we have to read a relocation against the address.
      if (RelocsSection) {
        for (const auto &Rel : RelocsSection->getSectionRef().relocations()) {
          if (Rel.getType() != RelocType)
            continue;
          if (Rel.getOffset() == TargetAddress) {
            const auto SymbolIter = Rel.getSymbol();
            assert(SymbolIter != InputFile->symbol_end() &&
                   "non-null symbol expected");
            const auto SymbolName = cantFail((*SymbolIter).getName());
            std::string Name = SymbolName.str() + "@PLT";
            const auto PtrSize = BC->AsmInfo->getCodePointerSize();
            auto *BF = BC->createBinaryFunction(Name, Section, InstrAddr, 0,
                                                /*IsSimple=*/false, EntrySize,
                                                PLTAlignment);
            auto TargetSymbol =
                BC->registerNameAtAddress(SymbolName.str() + "@GOT",
                                          TargetAddress, PtrSize, PLTAlignment);
            BF->setPLTSymbol(TargetSymbol);
            break;
          }
        }
      }
    }
  };

  if (PLTSection) {
    // Pseudo function for the start of PLT. The table could have a matching
    // FDE that we want to match to pseudo function.
    BC->createBinaryFunction("__BOLT_PLT_PSEUDO", *PLTSection,
                             PLTSection->getAddress(), 0, false, PLTSize,
                             PLTAlignment);
    analyzeOnePLTSection(*PLTSection,
                         RelaPLTSection ? &*RelaPLTSection : nullptr,
                         ELF::R_X86_64_JUMP_SLOT, PLTSize);
  }

  if (PLTGOTSection) {
    analyzeOnePLTSection(*PLTGOTSection,
                         RelaDynSection ? &*RelaDynSection : nullptr,
                         ELF::R_X86_64_GLOB_DAT,
                         /*Size=*/8);
    // If we did not register any function at PLTGOT start, we may be missing
    // relocs. Add a function at the start to mark this section.
    if (BC->getBinaryFunctions().find(PLTGOTSection->getAddress()) ==
        BC->getBinaryFunctions().end()) {
      BC->createBinaryFunction("__BOLT_PLTGOT_PSEUDO", *PLTGOTSection,
                               PLTGOTSection->getAddress(), 0, false,
                               /*Size*/ 8, PLTAlignment);
    }
  }
}

void RewriteInstance::adjustFunctionBoundaries() {
  for (auto BFI = BC->getBinaryFunctions().begin(),
            BFE = BC->getBinaryFunctions().end();
       BFI != BFE; ++BFI) {
    auto &Function = BFI->second;
    const BinaryFunction *NextFunction{nullptr};
    if (std::next(BFI) != BFE)
      NextFunction = &std::next(BFI)->second;

    // Check if it's a fragment of a function.
    auto FragName = Function.hasNameRegex(".*\\.cold\\..*");
    if (!FragName)
      FragName = Function.hasNameRegex(".*\\.cold");
    if (FragName) {
      static bool PrintedWarning = false;
      if (BC->HasRelocations && !PrintedWarning) {
        errs() << "BOLT-WARNING: split function detected on input : "
               << *FragName << ". The support is limited in relocation mode.\n";
        PrintedWarning = true;
      }
      Function.IsFragment = true;
    }

    // Check if there's a symbol or a function with a larger address in the
    // same section. If there is - it determines the maximum size for the
    // current function. Otherwise, it is the size of a containing section
    // the defines it.
    //
    // NOTE: ignore some symbols that could be tolerated inside the body
    //       of a function.
    auto NextSymRefI = FileSymRefs.upper_bound(Function.getAddress());
    while (NextSymRefI != FileSymRefs.end()) {
      auto &Symbol = NextSymRefI->second;
      const auto SymbolAddress = NextSymRefI->first;
      const auto SymbolSize = ELFSymbolRef(Symbol).getSize();

      if (NextFunction && SymbolAddress >= NextFunction->getAddress())
        break;

      if (!Function.isSymbolValidInScope(Symbol, SymbolSize))
        break;

      // This is potentially another entry point into the function.
      auto EntryOffset = NextSymRefI->first - Function.getAddress();
      DEBUG(dbgs() << "BOLT-DEBUG: adding entry point to function " << Function
                   << " at offset 0x" << Twine::utohexstr(EntryOffset) << '\n');
      Function.addEntryPointAtOffset(EntryOffset);
      // In non-relocation mode there's potentially an external undetectable
      // reference to the entry point and hence we cannot move this entry
      // point. Optimizing without moving could be difficult.
      if (!BC->HasRelocations)
        Function.setSimple(false);

      ++NextSymRefI;
    }

    // Function runs at most till the end of the containing section.
    uint64_t NextObjectAddress = Function.getSection().getEndAddress();
    // Or till the next object marked by a symbol.
    if (NextSymRefI != FileSymRefs.end()) {
      NextObjectAddress = std::min(NextSymRefI->first, NextObjectAddress);
    }
    // Or till the next function not marked by a symbol.
    if (NextFunction) {
      NextObjectAddress =
          std::min(NextFunction->getAddress(), NextObjectAddress);
    }

    const auto MaxSize = NextObjectAddress - Function.getAddress();
    if (MaxSize < Function.getSize()) {
      errs() << "BOLT-ERROR: symbol seen in the middle of the function "
             << Function << ". Skipping.\n";
      Function.setSimple(false);
      Function.setMaxSize(Function.getSize());
      continue;
    }
    Function.setMaxSize(MaxSize);
    if (!Function.getSize() && Function.isSimple()) {
      // Some assembly functions have their size set to 0, use the max
      // size as their real size.
      if (opts::Verbosity >= 1) {
        outs() << "BOLT-INFO: setting size of function " << Function << " to "
               << Function.getMaxSize() << " (was 0)\n";
      }
      Function.setSize(Function.getMaxSize());
    }
  }
}

void RewriteInstance::relocateEHFrameSection() {
  assert(EHFrameSection && "non-empty .eh_frame section expected");

  DWARFDebugFrame EHFrame(true, EHFrameSection->getAddress());
  DWARFDataExtractor DE(EHFrameSection->getContents(),
                        BC->AsmInfo->isLittleEndian(),
                        BC->AsmInfo->getCodePointerSize());
  auto createReloc = [&](uint64_t Value, uint64_t Offset, uint64_t DwarfType) {
    if (DwarfType == dwarf::DW_EH_PE_omit)
      return;

    if (!(DwarfType & dwarf::DW_EH_PE_pcrel) &&
        !(DwarfType & dwarf::DW_EH_PE_textrel) &&
        !(DwarfType & dwarf::DW_EH_PE_funcrel) &&
        !(DwarfType & dwarf::DW_EH_PE_datarel)) {
      return;
    }

    if (!(DwarfType & dwarf::DW_EH_PE_sdata4))
      return;

    uint64_t RelType;
    switch (DwarfType & 0x0f) {
    default:
      llvm_unreachable("unsupported DWARF encoding type");
    case dwarf::DW_EH_PE_sdata4:
    case dwarf::DW_EH_PE_udata4:
      RelType = ELF::R_X86_64_PC32;
      Offset -= 4;
      break;
    case dwarf::DW_EH_PE_sdata8:
    case dwarf::DW_EH_PE_udata8:
      RelType = ELF::R_X86_64_PC64;
      Offset -= 8;
      break;
    }

    auto *BD = BC->getBinaryDataContainingAddress(Value);
    auto *Symbol = BD ? BD->getSymbol() : nullptr;
    auto Addend = BD ? Value - BD->getAddress() : 0;
    if (!Symbol) {
      DEBUG(dbgs() << "BOLT-DEBUG: creating symbol for DWARF reference at 0x"
                   << Twine::utohexstr(Value) << '\n');
      Symbol = BC->getOrCreateGlobalSymbol(Value, "FUNCat");
    }

    DEBUG(dbgs() << "BOLT-DEBUG: adding DWARF reference against symbol "
                 << Symbol->getName() << '\n');

    EHFrameSection->addRelocation(Offset, Symbol, RelType, Addend);
  };

  EHFrame.parse(DE, createReloc);
}

ArrayRef<uint8_t> RewriteInstance::getLSDAData() {
  return ArrayRef<uint8_t>(LSDASection->getData(),
                           LSDASection->getContents().size());
}

uint64_t RewriteInstance::getLSDAAddress() {
  return LSDASection->getAddress();
}

void RewriteInstance::readSpecialSections() {
  NamedRegionTimer T("readSpecialSections", "read special sections",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);

  bool HasTextRelocations = false;
  bool HasDebugInfo = false;

  // Process special sections.
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
      if (isDebugSection(SectionName))
        HasDebugInfo = true;
    }
  }

  if (HasDebugInfo && !opts::UpdateDebugSections && !opts::AggregateOnly) {
    errs() << "BOLT-WARNING: debug info will be stripped from the binary. "
              "Use -update-debug-sections to keep it.\n";
  }

  HasTextRelocations = (bool)BC->getUniqueSectionByName(".rela.text");
  LSDASection = BC->getUniqueSectionByName(".gcc_except_table");
  EHFrameSection = BC->getUniqueSectionByName(".eh_frame");
  PLTSection = BC->getUniqueSectionByName(".plt");
  GOTPLTSection = BC->getUniqueSectionByName(".got.plt");
  PLTGOTSection = BC->getUniqueSectionByName(".plt.got");
  RelaPLTSection = BC->getUniqueSectionByName(".rela.plt");
  RelaDynSection = BC->getUniqueSectionByName(".rela.dyn");
  BuildIDSection = BC->getUniqueSectionByName(".note.gnu.build-id");
  SDTSection = BC->getUniqueSectionByName(".note.stapsdt");

  if (auto BATSec =
          BC->getUniqueSectionByName(BoltAddressTranslation::SECTION_NAME)) {
    // Do not read BAT when plotting a heatmap
    if (!opts::HeatmapMode) {
      if (std::error_code EC = BAT->parse(BATSec->getContents())) {
        errs() << "BOLT-ERROR: failed to parse BOLT address translation "
          "table.\n";
        exit(1);
      }
    }
  }

  if (opts::PrintSections) {
    outs() << "BOLT-INFO: Sections from original binary:\n";
    BC->printSections(outs());
  }

  if (opts::RelocationMode == cl::BOU_TRUE && !HasTextRelocations) {
    errs() << "BOLT-ERROR: relocations against code are missing from the input "
              "file. Cannot proceed in relocations mode (-relocs).\n";
    exit(1);
  }

  BC->HasRelocations = HasTextRelocations &&
                       (opts::RelocationMode != cl::BOU_FALSE);

  // Force non-relocation mode for heatmap generation
  if (opts::HeatmapMode) {
    BC->HasRelocations = false;
  }

  if (BC->HasRelocations) {
    outs() << "BOLT-INFO: enabling " << (opts::StrictMode ? "strict " : "")
           << "relocation mode\n";
  }

  // Process debug sections.
  EHFrame = BC->DwCtx->getEHFrame();
  if (opts::DumpEHFrame) {
    outs() << "BOLT-INFO: Dumping original binary .eh_frame\n";
    EHFrame->dump(outs(), &*BC->MRI, NoneType());
  }
  CFIRdWrt.reset(new CFIReaderWriter(*EHFrame));

  // Parse build-id
  parseBuildID();

  if (DA.started()) {
    if (auto FileBuildID = getPrintableBuildID()) {
      outs() << "BOLT-INFO: binary build-id is:     " << *FileBuildID << "\n";
      DA.processFileBuildID(*FileBuildID);
    } else {
      errs() << "BOLT-WARNING: build-id will not be checked because we could "
                "not read one from input binary\n";
    }
  }

  parseSDTNotes();
}

void RewriteInstance::adjustCommandLineOptions() {
  if (BC->isAArch64() && !BC->HasRelocations) {
    errs() << "BOLT-WARNING: non-relocation mode for AArch64 is not fully "
              "supported\n";
  }

  if (opts::Instrument && !BC->HasRelocations) {
    errs() << "BOLT-ERROR: instrumentation requires relocations\n";
    exit(1);
  }

  if (opts::AlignMacroOpFusion != MFT_NONE && !BC->isX86()) {
    outs() << "BOLT-INFO: disabling -align-macro-fusion on non-x86 platform\n";
    opts::AlignMacroOpFusion = MFT_NONE;
  }

  if ((X86AlignBranchWithin32BBoundaries || X86AlignBranchBoundary != 0) &&
      BC->isX86()) {
    if (!BC->HasRelocations) {
      errs() << "BOLT-ERROR: cannot apply mitigations for Intel JCC erratum in "
                "non-relocation mode\n";
      exit(1);
    }
    outs() << "BOLT-WARNING: using mitigation for Intel JCC erratum, layout "
              "may take several minutes\n";
    opts::AlignMacroOpFusion = MFT_NONE;
  }

  if (opts::AlignMacroOpFusion != MFT_NONE &&
      !BC->HasRelocations) {
    outs() << "BOLT-INFO: disabling -align-macro-fusion in non-relocation "
              "mode\n";
    opts::AlignMacroOpFusion = MFT_NONE;
  }

  if (opts::SplitEH && !BC->HasRelocations) {
    errs() << "BOLT-WARNING: disabling -split-eh in non-relocation mode\n";
    opts::SplitEH = false;
  }

  if (opts::StrictMode && !BC->HasRelocations) {
    errs() << "BOLT-WARNING: disabling strict mode (-strict) in non-relocation "
              "mode\n";
    opts::StrictMode = false;
  }

  if (BC->HasRelocations && opts::AggregateOnly &&
      !opts::StrictMode.getNumOccurrences()) {
    outs() << "BOLT-INFO: enabling strict relocation mode for aggregation "
              "purposes\n";
    opts::StrictMode = true;
  }

  if (BC->isX86() && BC->HasRelocations &&
      opts::AlignMacroOpFusion == MFT_HOT &&
      !DA.started() && BC->DR.getAllFuncsData().empty() &&
      opts::BoltProfile.empty()) {
    outs() << "BOLT-INFO: enabling -align-macro-fusion=all since no profile "
              "was specified\n";
    opts::AlignMacroOpFusion = MFT_ALL;
  }

  if (!BC->HasRelocations &&
      opts::ReorderFunctions != ReorderFunctions::RT_NONE) {
    errs() << "BOLT-ERROR: function reordering only works when "
           << "relocations are enabled\n";
    exit(1);
  }

  if (opts::ReorderFunctions != ReorderFunctions::RT_NONE &&
      !opts::HotText.getNumOccurrences()) {
    opts::HotText = true;
  } else if (opts::HotText && !BC->HasRelocations) {
    errs() << "BOLT-WARNING: hot text is disabled in non-relocation mode\n";
    opts::HotText = false;
  }

  if (opts::HotText && opts::HotTextMoveSections.getNumOccurrences() == 0) {
    opts::HotTextMoveSections.addValue(".stub");
    opts::HotTextMoveSections.addValue(".mover");
    opts::HotTextMoveSections.addValue(".never_hugify");
  }
}

namespace {
template <typename ELFT>
int64_t getRelocationAddend(const ELFObjectFile<ELFT> *Obj,
                            const RelocationRef &RelRef) {
  int64_t Addend = 0;
  const ELFFile<ELFT> &EF = *Obj->getELFFile();
  DataRefImpl Rel = RelRef.getRawDataRefImpl();
  const auto *RelocationSection = cantFail(EF.getSection(Rel.d.a));
  switch (RelocationSection->sh_type) {
  default: llvm_unreachable("unexpected relocation section type");
  case ELF::SHT_REL:
    break;
  case ELF::SHT_RELA: {
    const auto *RelA = Obj->getRela(Rel);
    Addend = RelA->r_addend;
    break;
  }
  }

  return Addend;
}

int64_t getRelocationAddend(const ELFObjectFileBase *Obj,
                         const RelocationRef &Rel) {
  if (auto *ELF32LE = dyn_cast<ELF32LEObjectFile>(Obj))
    return getRelocationAddend(ELF32LE, Rel);
  if (auto *ELF64LE = dyn_cast<ELF64LEObjectFile>(Obj))
    return getRelocationAddend(ELF64LE, Rel);
  if (auto *ELF32BE = dyn_cast<ELF32BEObjectFile>(Obj))
    return getRelocationAddend(ELF32BE, Rel);
  auto *ELF64BE = cast<ELF64BEObjectFile>(Obj);
  return getRelocationAddend(ELF64BE, Rel);
}
} // anonymous namespace

bool RewriteInstance::analyzeRelocation(const RelocationRef &Rel,
                                        uint64_t RType,
                                        std::string &SymbolName,
                                        bool &IsSectionRelocation,
                                        uint64_t &SymbolAddress,
                                        int64_t &Addend,
                                        uint64_t &ExtractedValue) const {
  if (!Relocation::isSupported(RType))
    return false;

  const bool IsAArch64 = BC->isAArch64();

  const auto RelSize = Relocation::getSizeForType(RType);

  auto Value = BC->getUnsignedValueAtAddress(Rel.getOffset(), RelSize);
  assert(Value && "failed to extract relocated value");
  ExtractedValue = *Value;
  if (IsAArch64) {
    ExtractedValue = Relocation::extractValue(RType,
                                              ExtractedValue,
                                              Rel.getOffset());
  }

  Addend = getRelocationAddend(InputFile, Rel);

  const auto IsPCRelative = Relocation::isPCRelative(RType);
  const auto PCRelOffset = IsPCRelative && !IsAArch64 ? Rel.getOffset() : 0;
  bool SkipVerification = false;
  auto SymbolIter = Rel.getSymbol();
  if (SymbolIter == InputFile->symbol_end()) {
    SymbolAddress = ExtractedValue - Addend + PCRelOffset;
    auto *RelSymbol = BC->getOrCreateGlobalSymbol(SymbolAddress, "RELSYMat");
    SymbolName = RelSymbol->getName();
    IsSectionRelocation = false;
  } else {
    const auto &Symbol = *SymbolIter;
    SymbolName = cantFail(Symbol.getName());
    SymbolAddress = cantFail(Symbol.getAddress());
    SkipVerification = (cantFail(Symbol.getType()) == SymbolRef::ST_Other);
    // Section symbols are marked as ST_Debug.
    IsSectionRelocation = (cantFail(Symbol.getType()) == SymbolRef::ST_Debug);
  }
  // For PIE or dynamic libs, the linker may choose not to put the relocation
  // result at the address if it is a X86_64_64 one because it will emit a
  // dynamic relocation (X86_RELATIVE) for the dynamic linker and loader to
  // resolve it at run time. The static relocation result goes as the addend
  // of the dynamic relocation in this case. We can't verify these cases.
  // FIXME: perhaps we can try to find if it really emitted a corresponding
  // RELATIVE relocation at this offset with the correct value as the addend.
  if (!BC->HasFixedLoadAddress && RelSize == 8)
    SkipVerification = true;

  if (IsSectionRelocation && !IsAArch64) {
    auto Section = BC->getSectionForAddress(SymbolAddress);
    assert(Section && "section expected for section relocation");
    SymbolName = "section " + std::string(Section->getName());
    // Convert section symbol relocations to regular relocations inside
    // non-section symbols.
    if (Section->containsAddress(ExtractedValue) && !IsPCRelative) {
      SymbolAddress = ExtractedValue;
      Addend = 0;
    } else {
      Addend = ExtractedValue - (SymbolAddress - PCRelOffset);
    }
  }

  // If no symbol has been found or if it is a relocation requiring the
  // creation of a GOT entry, do not link against the symbol but against
  // whatever address was extracted from the instruction itself. We are
  // not creating a GOT entry as this was already processed by the linker.
  // For GOT relocs, do not subtract addend as the addend does not refer
  // to this instruction's target, but it refers to the target in the GOT
  // entry.
  if (Relocation::isGOT(RType)) {
    Addend = 0;
    SymbolAddress = ExtractedValue + PCRelOffset;
  } else if (!SymbolAddress) {
    assert(!IsSectionRelocation);
    if (ExtractedValue || Addend == 0 || IsPCRelative) {
      SymbolAddress = ExtractedValue - Addend + PCRelOffset;
    } else {
      // This is weird case.  The extracted value is zero but the addend is
      // non-zero and the relocation is not pc-rel.  Using the previous logic,
      // the SymbolAddress would end up as a huge number.  Seen in
      // exceptions_pic.test.
      DEBUG(dbgs() << "BOLT-DEBUG: relocation @ 0x"
                   << Twine::utohexstr(Rel.getOffset())
                   << " value does not match addend for "
                   << "relocation to undefined symbol.\n");
      return true;
    }
  }

  auto verifyExtractedValue = [&]() {
    if (SkipVerification)
      return true;

    if (IsAArch64)
      return true;

    if (SymbolName == "__hot_start" || SymbolName == "__hot_end")
      return true;

    if (Relocation::isTLS(RType))
      return true;

    return truncateToSize(ExtractedValue, RelSize) ==
           truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize);
  };

  assert(verifyExtractedValue() && "mismatched extracted relocation value");

  return true;
}

void RewriteInstance::readRelocations(const SectionRef &Section,
                                      NameResolver &NR) {
  StringRef SectionName;
  Section.getName(SectionName);
  DEBUG(dbgs() << "BOLT-DEBUG: reading relocations for section "
               << SectionName << ":\n");
  if (BinarySection(*BC, Section).isAllocatable()) {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring runtime relocations\n");
    return;
  }
  auto SecIter = Section.getRelocatedSection();
  assert(SecIter != InputFile->section_end() && "relocated section expected");
  auto RelocatedSection = *SecIter;

  StringRef RelocatedSectionName;
  RelocatedSection.getName(RelocatedSectionName);
  DEBUG(dbgs() << "BOLT-DEBUG: relocated section is "
               << RelocatedSectionName << '\n');

  if (!BinarySection(*BC, RelocatedSection).isAllocatable()) {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocations against "
                 << "non-allocatable section\n");
    return;
  }
  const bool SkipRelocs = StringSwitch<bool>(RelocatedSectionName)
    .Cases(".plt", ".rela.plt", ".got.plt", ".eh_frame", ".gcc_except_table",
           true)
    .Default(false);
  if (SkipRelocs) {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocations against known section\n");
    return;
  }

  const bool IsAArch64 = BC->isAArch64();
  const bool IsFromCode = RelocatedSection.isText();

  auto printRelocationInfo = [&](const RelocationRef &Rel,
                                 StringRef SymbolName,
                                 uint64_t SymbolAddress,
                                 uint64_t Addend,
                                 uint64_t ExtractedValue) {
    SmallString<16> TypeName;
    Rel.getTypeName(TypeName);
    const auto Address = SymbolAddress + Addend;
    auto Section = BC->getSectionForAddress(SymbolAddress);
    dbgs() << "Relocation: offset = 0x"
           << Twine::utohexstr(Rel.getOffset())
           << "; type = " << TypeName
           << "; value = 0x" << Twine::utohexstr(ExtractedValue)
           << "; symbol = " << SymbolName
           << " (" << (Section ? Section->getName() : "") << ")"
           << "; symbol address = 0x" << Twine::utohexstr(SymbolAddress)
           << "; addend = 0x" << Twine::utohexstr(Addend)
           << "; address = 0x" << Twine::utohexstr(Address)
           << "; in = ";
    if (auto *Func = BC->getBinaryFunctionContainingAddress(Rel.getOffset(),
                                                            false,
                                                            IsAArch64)) {
      dbgs() << Func->getPrintName() << "\n";
    } else {
      dbgs() << BC->getSectionForAddress(Rel.getOffset())->getName() << "\n";
    }
  };

  for (const auto &Rel : Section.relocations()) {
    SmallString<16> TypeName;
    Rel.getTypeName(TypeName);
    auto RType = Rel.getType();

    // Adjust the relocation type as the linker might have skewed it.
    if (BC->isX86() && (RType & ELF::R_X86_64_converted_reloc_bit)) {
      if (opts::Verbosity >= 1) {
        dbgs() << "BOLT-WARNING: ignoring R_X86_64_converted_reloc_bit\n";
      }
      RType &= ~ELF::R_X86_64_converted_reloc_bit;
    }

    // No special handling required for TLS relocations.
    if (Relocation::isTLS(RType))
      continue;

    std::string SymbolName;
    uint64_t SymbolAddress;
    int64_t Addend;
    uint64_t ExtractedValue;
    bool IsSectionRelocation;
    if (!analyzeRelocation(Rel,
                           RType,
                           SymbolName,
                           IsSectionRelocation,
                           SymbolAddress,
                           Addend,
                           ExtractedValue)) {
      DEBUG(dbgs() << "BOLT-DEBUG: skipping relocation @ offset = 0x"
                   << Twine::utohexstr(Rel.getOffset())
                   << "; type name = " << TypeName
                   << '\n');
      continue;
    }

    const auto Address = SymbolAddress + Addend;

    DEBUG(dbgs() << "BOLT-DEBUG: ";
          printRelocationInfo(Rel,
                              SymbolName,
                              SymbolAddress,
                              Addend,
                              ExtractedValue));

    BinaryFunction *ContainingBF = nullptr;
    if (IsFromCode) {
      ContainingBF =
        BC->getBinaryFunctionContainingAddress(Rel.getOffset(),
                                               /*CheckPastEnd*/ false,
                                               /*UseMaxSize*/ IsAArch64);
      assert(ContainingBF && "cannot find function for address in code");
    }

    // PC-relative relocations from data to code are tricky since the original
    // information is typically lost after linking even with '--emit-relocs'.
    // They are normally used by PIC-style jump tables and reference both
    // the jump table and jump destination by computing the difference
    // between the two. If we blindly apply the relocation it will appear
    // that it references an arbitrary location in the code, possibly even
    // in a different function from that containing the jump table.
    if (!IsAArch64 && Relocation::isPCRelative(RType)) {
      // Just register the fact that we have PC-relative relocation at a given
      // address. The actual referenced label/address cannot be determined
      // from linker data alone.
      if (!IsFromCode) {
        BC->addPCRelativeDataRelocation(Rel.getOffset());
      }
      DEBUG(dbgs() << "BOLT-DEBUG: not creating PC-relative relocation at 0x"
                   << Twine::utohexstr(Rel.getOffset()) << " for " << SymbolName
                   << "\n");
      continue;
    }

    auto ForceRelocation = [&](StringRef SymbolName) {
      if (opts::HotText && (SymbolName == "__hot_start" ||
                            SymbolName == "__hot_end"))
        return true;

      if (opts::HotData && (SymbolName == "__hot_data_start" ||
                            SymbolName == "__hot_data_end"))
        return true;

      if (SymbolName == "_end")
        return true;

      return false;
    }(SymbolName);

    if (BC->isAArch64() && RType == ELF::R_AARCH64_ADR_GOT_PAGE)
      ForceRelocation = true;

    auto RefSection = BC->getSectionForAddress(SymbolAddress);
    if (!RefSection && !ForceRelocation) {
      DEBUG(dbgs() << "BOLT-DEBUG: cannot determine referenced section.\n");
      continue;
    }

    const bool IsToCode = RefSection && RefSection->isText();

    // Occasionally we may see a reference past the last byte of the function
    // typically as a result of __builtin_unreachable(). Check it here.
    auto *ReferencedBF = BC->getBinaryFunctionContainingAddress(
        Address, /*CheckPastEnd*/ true, /*UseMaxSize*/ IsAArch64);

    if (!IsSectionRelocation) {
      if (auto *BF = BC->getBinaryFunctionContainingAddress(SymbolAddress)) {
        if (BF != ReferencedBF) {
          // It's possible we are referencing a function without referencing any
          // code, e.g. when taking a bitmask action on a function address.
          errs() << "BOLT-WARNING: non-standard function reference (e.g. "
                    "bitmask) detected against function " << *BF;
          if (IsFromCode) {
            errs() << " from function " << *ContainingBF << '\n';
          } else {
            errs() << " from data section at 0x"
                   << Twine::utohexstr(Rel.getOffset()) << '\n';
          }
          DEBUG(printRelocationInfo(Rel,
                                    SymbolName,
                                    SymbolAddress,
                                    Addend,
                                    ExtractedValue)
          );
          ReferencedBF = BF;
        }
      }
    } else if (ReferencedBF) {
      assert(RefSection && "section expected for section relocation");
      if (ReferencedBF->getSection() != *RefSection) {
        DEBUG(dbgs() << "BOLT-DEBUG: ignoring false function reference\n");
        ReferencedBF = nullptr;
      }
    }

    // Workaround for a member function pointer de-virtualization bug. We check
    // if a non-pc-relative relocation in the code is pointing to (fptr - 1).
    if (IsToCode && ContainingBF && !Relocation::isPCRelative(RType) &&
        (!ReferencedBF || (ReferencedBF->getAddress() != Address))) {
      if (const auto *RogueBF = BC->getBinaryFunctionAtAddress(Address + 1)) {
        // Do an extra check that the function was referenced previously.
        // It's a linear search, but it should rarely happen.
        bool Found{false};
        for (const auto &RelKV : ContainingBF->Relocations) {
          const auto &Rel = RelKV.second;
          if (Rel.Symbol == RogueBF->getSymbol() &&
              !Relocation::isPCRelative(Rel.Type)) {
            Found = true;
            break;
          }
        }

        if (Found) {
          errs() << "BOLT-WARNING: detected possible compiler "
                    "de-virtualization bug: -1 addend used with "
                    "non-pc-relative relocation against function "
                 << *RogueBF << " in function " << *ContainingBF << '\n';
          continue;
        }
      }
    }

    MCSymbol *ReferencedSymbol = nullptr;
    if (ForceRelocation) {
      auto Name = Relocation::isGOT(RType) ? "Zero" : SymbolName;
      ReferencedSymbol = BC->registerNameAtAddress(Name, 0, 0, 0);
      SymbolAddress = 0;
      if (Relocation::isGOT(RType))
        Addend = Address;
      DEBUG(dbgs() << "BOLT-DEBUG: forcing relocation against symbol "
                   << SymbolName << " with addend " << Addend << '\n');
    } else if (ReferencedBF) {
      ReferencedSymbol = ReferencedBF->getSymbol();
      uint64_t RefFunctionOffset = 0;

      // Adjust the point of reference to a code location inside a function.
      if (ReferencedBF->containsAddress(Address, /*UseMaxSize = */true)) {
        RefFunctionOffset = Address - ReferencedBF->getAddress();
        if (RefFunctionOffset) {
          ReferencedSymbol =
            ReferencedBF->getOrCreateLocalLabel(Address,
                                                /*CreatePastEnd =*/ true);
          ReferencedBF->registerReferencedOffset(RefFunctionOffset);
          if (opts::Verbosity > 1 && !RelocatedSection.isReadOnly()) {
            dbgs() << "BOLT-WARNING: writable reference into the middle of "
                   << "the function " << *ReferencedBF
                   << " detected at address 0x"
                   << Twine::utohexstr(Rel.getOffset()) << '\n';
          }
        }
        SymbolAddress = Address;
        Addend = 0;
      }
      DEBUG(
        dbgs() << "  referenced function " << *ReferencedBF;
        if (Address != ReferencedBF->getAddress())
          dbgs() << " at offset 0x" << Twine::utohexstr(RefFunctionOffset);
        dbgs() << '\n'
      );
    } else {
      if (IsToCode && SymbolAddress) {
        // This can happen e.g. with PIC-style jump tables.
        DEBUG(dbgs() << "BOLT-DEBUG: no corresponding function for "
                        "relocation against code\n");
      }

      // In AArch64 there are zero reasons to keep a reference to the
      // "original" symbol plus addend. The original symbol is probably just a
      // section symbol. If we are here, this means we are probably accessing
      // data, so it is imperative to keep the original address.
      if (IsAArch64) {
        SymbolName = ("SYMBOLat0x" + Twine::utohexstr(Address)).str();
        SymbolAddress = Address;
        Addend = 0;
      }

      // If we are allowing section relocations, we assign relocations
      // that are pointing to the end of a symbol to that symbol rather
      // than the following symbol.
      const auto IncludeEnd =
        opts::AllowSectionRelocations && IsSectionRelocation;

      if (auto *BD = BC->getBinaryDataContainingAddress(SymbolAddress,
                                                        IncludeEnd)) {
        assert(!IncludeEnd ||
               (BD == BC->getBinaryDataContainingAddress(SymbolAddress) ||
                !BC->getBinaryDataContainingAddress(SymbolAddress) ||
                (IsSectionRelocation && BD->getEndAddress() ==
                 BC->getBinaryDataContainingAddress(SymbolAddress)->
                    getAddress())));

        // Note: this assertion is trying to check sanity of BinaryData objects
        // but AArch64 has inferred and incomplete object locations coming from
        // GOT/TLS or any other non-trivial relocation (that requires creation
        // of sections and whose symbol address is not really what should be
        // encoded in the instruction). So we essentially disabled this check
        // for AArch64 and live with bogus names for objects.
        assert((IsAArch64 ||
                IsSectionRelocation ||
                BD->nameStartsWith(SymbolName) ||
                BD->nameStartsWith("PG" + SymbolName) ||
                (BD->nameStartsWith("ANONYMOUS") &&
                 (BD->getSectionName().startswith(".plt") ||
                  BD->getSectionName().endswith(".plt")))) &&
               "BOLT symbol names of all non-section relocations must match "
               "up with symbol names referenced in the relocation");

        if (!opts::AllowSectionRelocations && IsSectionRelocation) {
          BC->markAmbiguousRelocations(*BD, Address);
        }

        ReferencedSymbol = BD->getSymbol();
        Addend += (SymbolAddress - BD->getAddress());
        SymbolAddress = BD->getAddress();
        assert(Address == SymbolAddress + Addend);
      } else {
        // These are mostly local data symbols but undefined symbols
        // in relocation sections can get through here too, from .plt.
        assert((IsAArch64 ||
                IsSectionRelocation ||
                BC->getSectionNameForAddress(SymbolAddress)->startswith(".plt"))
               && "known symbols should not resolve to anonymous locals");

        if (IsSectionRelocation) {
          ReferencedSymbol = BC->getOrCreateGlobalSymbol(SymbolAddress,
                                                         "SYMBOLat");
        } else {
          auto Symbol = *Rel.getSymbol();
          const uint64_t SymbolSize =
              IsAArch64 ? 0 : ELFSymbolRef(Symbol).getSize();
          const uint64_t SymbolAlignment =
              IsAArch64 ? 1 : Symbol.getAlignment();
          const auto SymbolFlags = Symbol.getFlags();
          std::string Name;
          if (SymbolFlags & SymbolRef::SF_Global) {
            Name = SymbolName;
          } else {
            if (StringRef(SymbolName).startswith(
                  BC->AsmInfo->getPrivateGlobalPrefix())) {
              Name = NR.uniquify("PG" + SymbolName);
            } else {
              Name = NR.uniquify(SymbolName);
            }
          }
          ReferencedSymbol = BC->registerNameAtAddress(Name,
                                                       SymbolAddress,
                                                       SymbolSize,
                                                       SymbolAlignment,
                                                       SymbolFlags);
        }

        if (!opts::AllowSectionRelocations && IsSectionRelocation) {
          auto *BD = BC->getBinaryDataByName(ReferencedSymbol->getName());
          BC->markAmbiguousRelocations(*BD, Address);
        }
      }
    }

    auto checkMaxDataRelocations = [&]() {
      ++NumDataRelocations;
      if (opts::MaxDataRelocations &&
          NumDataRelocations + 1 == opts::MaxDataRelocations) {
          dbgs() << "BOLT-DEBUG: processing ending on data relocation "
                 << NumDataRelocations << ": ";
          printRelocationInfo(Rel,
                              ReferencedSymbol->getName(),
                              SymbolAddress,
                              Addend,
                              ExtractedValue);
      }

      return (!opts::MaxDataRelocations ||
              NumDataRelocations < opts::MaxDataRelocations);
    };

    if ((RefSection && refersToReorderedSection(RefSection)) ||
        (opts::ForceToDataRelocations && checkMaxDataRelocations()))
      ForceRelocation = true;

    if (IsFromCode) {
      ContainingBF->addRelocation(Rel.getOffset(),
                                  ReferencedSymbol,
                                  RType,
                                  Addend,
                                  ExtractedValue);
    } else if (IsToCode || ForceRelocation) {
      BC->addRelocation(Rel.getOffset(), ReferencedSymbol, RType, Addend,
                        ExtractedValue);
    } else {
      DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocation from data to data\n");
    }
  }
}

void RewriteInstance::readDebugInfo() {
  NamedRegionTimer T("readDebugInfo", "read debug info", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);
  if (!opts::UpdateDebugSections)
    return;

  BC->preprocessDebugInfo();
}

void RewriteInstance::preprocessProfileData() {
  NamedRegionTimer T("preprocessprofile", "pre-process profile data",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);
  if (BAT->enabledFor(InputFile)) {
    outs() << "BOLT-INFO: profile collection done on a binary already "
              "processed by BOLT\n";
    DA.setBAT(&*BAT);
  }
  DA.parseProfile(*BC.get());
}

void RewriteInstance::processProfileData() {
  NamedRegionTimer T("processprofile", "process profile data", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);
  auto &BinaryFunctions = BC->getBinaryFunctions();
  if (DA.started()) {
    DA.processProfile(*BC.get());

    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;
      Function.convertBranchData();
    }

    if (opts::AggregateOnly) {
      if (std::error_code EC = DA.writeAggregatedFile()) {
        check_error(EC, "cannot create output data file");
      }
    }
  } else {
    if (!opts::BoltProfile.empty()) {
      ProfileReader PR;
      auto EC = PR.readProfile(opts::BoltProfile, BinaryFunctions);
      check_error(EC, "cannot read profile");

      return;
    }

    // Preliminary match profile data to functions.
    if (!BC->DR.getAllFuncsData().empty()) {
      if (BC->DR.collectedInBoltedBinary()) {
        outs() << "BOLT-INFO: profile collection done on a binary already "
                  "processed by BOLT\n";
      }
      for (auto &BFI : BinaryFunctions) {
        auto &Function = BFI.second;
        if (auto *MemData = BC->DR.getFuncMemData(Function.getNames())) {
          Function.MemData = MemData;
          MemData->Used = true;
        }
        if (auto *FuncData = BC->DR.getFuncBranchData(Function.getNames())) {
          Function.BranchData = FuncData;
          Function.ExecutionCount = FuncData->ExecutionCount;
          FuncData->Used = true;
        }
      }
    }

    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;
      Function.readProfile();
    }
  }

  if (!opts::SaveProfile.empty()) {
    ProfileWriter PW(opts::SaveProfile);
    PW.writeProfile(*this);
  }
}

void RewriteInstance::disassembleFunctions() {
  NamedRegionTimer T("disassembleFunctions", "disassemble functions",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);
  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;

    if (!shouldDisassemble(Function)) {
      Function.setSimple(false);
      continue;
    }

    auto FunctionData = Function.getData();
    if (!FunctionData) {
      // When could it happen?
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
      reinterpret_cast<const uint8_t*>(InputFile->getData().data());
    Function.setFileOffset(FunctionData->begin() - FileBegin);

    Function.disassemble();

    if (!Function.isSimple() && BC->HasRelocations) {
      BC->exitWithBugReport("function cannot be properly disassembled. "
                            "Unable to continue in relocation mode.",
                            Function);
    }

    if (opts::PrintAll || opts::PrintDisasm)
      Function.print(outs(), "after disassembly", true);

    // Post-process inter-procedural references ASAP as it may affect
    // functions we are about to disassemble next.
    BC->processInterproceduralReferences();
  }

  BC->populateJumpTables();

  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;

    if (!shouldDisassemble(Function))
      continue;

    Function.postProcessEntryPoints();
    Function.postProcessJumpTables();
  }

  if (!BC->HasRelocations)
    BC->adjustCodePadding();

  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;

    if (!shouldDisassemble(Function))
      continue;

    if (!Function.isSimple()) {
      assert((!BC->HasRelocations || Function.getSize() == 0) &&
             "unexpected non-simple function in relocation mode");
      continue;
    }

    // Fill in CFI information for this function
    if (!Function.trapsOnEntry()) {
      if (!CFIRdWrt->fillCFIInfoFor(Function)) {
        if (BC->HasRelocations) {
          BC->exitWithBugReport("unable to fill CFI.", Function);
        } else {
          errs() << "BOLT-WARNING: unable to fill CFI for function "
                 << Function << ". Skipping.\n";
          Function.setSimple(false);
          continue;
        }
      }
    }

    // Parse LSDA.
    if (Function.getLSDAAddress() != 0)
      Function.parseLSDA(getLSDAData(), getLSDAAddress());

  } // Iterate over all functions

  // Run buildCFG in parallel for all functions
  {
    NamedRegionTimer T("buildCFG", "buildCFG", "buildfuncs",
                       "Build Binary Functions", opts::TimeBuild);

    // Create annotation indices to allow lock-free execution
    BC->MIB->getOrCreateAnnotationIndex("Offset");
    BC->MIB->getOrCreateAnnotationIndex("JTIndexReg");

    ParallelUtilities::WorkFuncWithAllocTy WorkFun =
        [&](BinaryFunction &BF, MCPlusBuilder::AllocatorIdTy AllocId) {
          if (!BF.buildCFG(AllocId))
            return;

          if (opts::PrintAll)
            BF.print(outs(), "while building cfg", true);
        };

    ParallelUtilities::PredicateTy SkipPredicate =
        [&](const BinaryFunction &BF) {
          return !shouldDisassemble(BF) || !BF.isSimple();
        };

    ParallelUtilities::runOnEachFunctionWithUniqueAllocId(
        *BC, ParallelUtilities::SchedulingPolicy::SP_INST_LINEAR, WorkFun,
        SkipPredicate, "disassembleFunctions-buildCFG",
        /*ForceSequential*/ opts::SequentialDisassembly || opts::PrintAll);
  }

  BC->postProcessSymbolTable();
}

void RewriteInstance::postProcessFunctions() {
  BC->TotalScore = 0;
  BC->SumExecutionCount = 0;
  for (auto &BFI : BC->getBinaryFunctions()) {
    BinaryFunction &Function = BFI.second;

    if (Function.empty())
      continue;

    Function.postProcessCFG();

    if (opts::PrintAll || opts::PrintCFG)
      Function.print(outs(), "after building cfg", true);

    if (opts::DumpDotAll)
      Function.dumpGraphForPass("build-cfg");

    if (opts::PrintLoopInfo) {
      Function.calculateLoopInfo();
      Function.printLoopInfo(outs());
    }

    BC->TotalScore += Function.getFunctionScore();
    BC->SumExecutionCount += Function.getKnownExecutionCount();
  }

  if (opts::PrintGlobals) {
    outs() << "BOLT-INFO: Global symbols:\n";
    BC->printGlobalSymbols(outs());
  }
}

void RewriteInstance::runOptimizationPasses() {
  NamedRegionTimer T("runOptimizationPasses", "run optimization passes",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);
  if (opts::Instrument) {
    Instrumenter = llvm::make_unique<Instrumentation>();
    Instrumenter->runOnFunctions(*BC);
  }
  BinaryFunctionPassManager::runAllPasses(*BC);
}

// Helper function to emit the contents of a function via a MCStreamer object.
bool RewriteInstance::emitFunction(MCStreamer &Streamer,
                                   BinaryFunction &Function,
                                   bool EmitColdPart) {
  if (Function.size() == 0)
    return false;

  if (Function.getState() == BinaryFunction::State::Empty)
    return false;

  auto *Section =
      BC->getCodeSection(EmitColdPart ? Function.getColdCodeSectionName()
                                      : Function.getCodeSectionName());
  Streamer.SwitchSection(Section);
  Section->setHasInstructions(true);
  BC->Ctx->addGenDwarfSection(Section);

  if (BC->HasRelocations) {
    Streamer.EmitCodeAlignment(BinaryFunction::MinAlign);
    auto MaxAlignBytes = EmitColdPart
      ? Function.getMaxColdAlignmentBytes()
      : Function.getMaxAlignmentBytes();
    if (MaxAlignBytes > 0)
      Streamer.EmitCodeAlignment(Function.getAlignment(), MaxAlignBytes);
  } else {
    Streamer.EmitCodeAlignment(Function.getAlignment());
  }

  MCContext &Context = Streamer.getContext();
  const MCAsmInfo *MAI = Context.getAsmInfo();

  // Emit all symbols associated with the main function entry.
  if (!EmitColdPart) {
    for (auto *Symbol : Function.getSymbols()) {
      Streamer.EmitSymbolAttribute(Symbol, MCSA_ELF_TypeFunction);
      Streamer.EmitLabel(Symbol);
    }
  } else {
    auto *Symbol = Function.getColdSymbol();
    Streamer.EmitSymbolAttribute(Symbol, MCSA_ELF_TypeFunction);
    Streamer.EmitLabel(Symbol);
  }

  // Emit CFI start
  if (Function.hasCFI()) {
    Streamer.EmitCFIStartProc(/*IsSimple=*/false);
    if (Function.getPersonalityFunction() != nullptr) {
      Streamer.EmitCFIPersonality(Function.getPersonalityFunction(),
                                  Function.getPersonalityEncoding());
    }
    auto *LSDASymbol = EmitColdPart ? Function.getColdLSDASymbol()
                                    : Function.getLSDASymbol();
    if (LSDASymbol) {
      Streamer.EmitCFILsda(LSDASymbol, BC->MOFI->getLSDAEncoding());
    } else {
      Streamer.EmitCFILsda(0, dwarf::DW_EH_PE_omit);
    }
    // Emit CFI instructions relative to the CIE
    for (const auto &CFIInstr : Function.cie()) {
      // Only write CIE CFI insns that LLVM will not already emit
      const std::vector<MCCFIInstruction> &FrameInstrs =
          MAI->getInitialFrameState();
      if (std::find(FrameInstrs.begin(), FrameInstrs.end(), CFIInstr) ==
          FrameInstrs.end())
        Streamer.EmitCFIInstruction(CFIInstr);
    }
  }

  assert((Function.empty() || !(*Function.begin()).isCold()) &&
         "first basic block should never be cold");

  // Emit UD2 at the beginning if requested by user.
  if (!opts::BreakFunctionNames.empty()) {
    for (auto &Name : opts::BreakFunctionNames) {
      if (Function.hasNameRegex(Name)) {
        Streamer.EmitIntValue(0x0B0F, 2); // UD2: 0F 0B
        break;
      }
    }
  }

  // Emit code.
  Function.emitBody(Streamer, EmitColdPart, /*EmitCodeOnly=*/false);

  // Emit padding if requested.
  if (auto Padding = opts::padFunction(Function)) {
    DEBUG(dbgs() << "BOLT-DEBUG: padding function " << Function << " with "
                 << Padding << " bytes\n");
    Streamer.emitFill(Padding, MAI->getTextAlignFillValue());
  }

  if (opts::MarkFuncs) {
    Streamer.EmitIntValue(MAI->getTrapFillValue(), 1);
  }

  // Emit CFI end
  if (Function.hasCFI())
    Streamer.EmitCFIEndProc();

  Streamer.EmitLabel(EmitColdPart ? Function.getFunctionColdEndLabel()
                                  : Function.getFunctionEndLabel());

  // Exception handling info for the function.
  Function.emitLSDA(&Streamer, EmitColdPart);

  if (!EmitColdPart && opts::JumpTables > JTS_NONE)
    Function.emitJumpTables(&Streamer);

  return true;
}

namespace {

template <typename T>
std::vector<T> singletonSet(T t) {
  std::vector<T> Vec;
  Vec.push_back(std::move(t));
  return Vec;
}

} // anonymous namespace

void RewriteInstance::emitAndLink() {
  NamedRegionTimer T("emitAndLink", "emit and link", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);
  std::error_code EC;

  // This is an object file, which we keep for debugging purposes.
  // Once we decide it's useless, we should create it in memory.
  std::unique_ptr<ToolOutputFile> TempOut =
    llvm::make_unique<ToolOutputFile>(opts::OutputFilename + ".bolt.o",
                                      EC, sys::fs::F_None);
  check_error(EC, "cannot create output object file");

  std::unique_ptr<buffer_ostream> BOS =
      make_unique<buffer_ostream>(TempOut->os());
  raw_pwrite_stream *OS = BOS.get();

  // Implicitly MCObjectStreamer takes ownership of MCAsmBackend (MAB)
  // and MCCodeEmitter (MCE). ~MCObjectStreamer() will delete these
  // two instances.
  auto MCE = BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  auto MAB =
      BC->TheTarget->createMCAsmBackend(*BC->STI, *BC->MRI, MCTargetOptions());
  std::unique_ptr<MCStreamer> Streamer(BC->TheTarget->createMCObjectStreamer(
      *BC->TheTriple, *BC->Ctx, std::unique_ptr<MCAsmBackend>(MAB), *OS,
      std::unique_ptr<MCCodeEmitter>(MCE), *BC->STI,
      /* RelaxAll */ false,
      /* IncrementalLinkerCompatible */ false,
      /* DWARFMustBeAtTheEnd */ false));

  Streamer->InitSections(false);

  BC->getTextSection()->setAlignment(BC->PageAlign);

  emitFunctions(Streamer.get());
  if (opts::Instrument) {
    readELFDynamic();
    assert(StartFunction && FiniFunction &&
           "_start and DT_FINI functions must be set");
    Instrumenter->emit(*BC, *Streamer.get(), *StartFunction, *FiniFunction);
  }

  if (!BC->HasRelocations && opts::UpdateDebugSections)
    DebugInfoRewriter->updateDebugLineInfoForNonSimpleFunctions();

  emitDataSections(Streamer.get());

  // Relocate .eh_frame to .eh_frame_old.
  if (EHFrameSection) {
    relocateEHFrameSection();
    EHFrameSection->emitAsData(*Streamer, ".eh_frame_old");
  }

  // Update _end if needed.
  if (opts::UpdateEnd) {
    Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("_end"));
  }

  Streamer->Finish();

  //////////////////////////////////////////////////////////////////////////////
  // Assign addresses to new sections.
  //////////////////////////////////////////////////////////////////////////////

  if (opts::UpdateDebugSections) {
    // Compute offsets of tables in .debug_line for each compile unit.
    DebugInfoRewriter->updateLineTableOffsets();
  }

  // Get output object as ObjectFile.
  std::unique_ptr<MemoryBuffer> ObjectMemBuffer =
      MemoryBuffer::getMemBuffer(BOS->str(), "in-memory object file", false);
  std::unique_ptr<object::ObjectFile> Obj = cantFail(
      object::ObjectFile::createObjectFile(ObjectMemBuffer->getMemBufferRef()),
      "error creating in-memory object");

  auto Resolver = orc::createLegacyLookupResolver(
      [&](const std::string &Name) -> JITSymbol {
        DEBUG(dbgs() << "BOLT: looking for " << Name << "\n");
        if (BC->EFMM->ObjectsLoaded) {
          auto Result = OLT->findSymbol(Name, false);
          if (cantFail(Result.getAddress()) == 0) {
            // Resolve to a PLT entry if possible
            if (auto *I = BC->getBinaryDataByName(Name + "@PLT"))
              return JITSymbol(I->getAddress(), JITSymbolFlags());

            errs() << "BOLT-ERROR: symbol not found required by runtime "
                      "library: "
                   << Name << "\n";
            exit(1);
          }
          return Result;
        }
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
  // Key for our main object created out of the input binary
  auto K = ES->allocateVModule();
  OLT.reset(new decltype(OLT)::element_type(
      *ES,
      [this, &Resolver](orc::VModuleKey Key) {
        orc::RTDyldObjectLinkingLayer::Resources R;
        R.MemMgr = BC->EFMM;
        R.Resolver = Resolver;
        // Get memory manager
        return R;
      },
      // Loaded notifier
      [&](orc::VModuleKey Key, const object::ObjectFile &Obj,
          const RuntimeDyld::LoadedObjectInfo &) {
        // Assign addresses to all sections. If key corresponds to the object
        // created by ourselves, call our regular mapping function. If we are
        // loading additional objects as part of runtime libraries for
        // instrumentation, treat them as extra sections.
        if (Key == K) {
          mapFileSections(Key);
        } else {
          mapExtraSections(Key);
        }
      },
      // Finalized notifier
      [&](orc::VModuleKey Key) {
        // Update output addresses based on the new section map and
        // layout. Only do this for the object created by ourselves.
        if (Key == K)
          updateOutputValues(FinalLayout);
      }));

  OLT->setProcessAllSections(true);
  cantFail(OLT->addObject(K, std::move(ObjectMemBuffer)));
  cantFail(OLT->emitAndFinalize(K));

  // Link instrumentation runtime library
  if (opts::Instrument) {
    linkRuntime();
    Instrumenter->emitTablesAsELFNote(*BC);
  }

  // Once the code is emitted, we can rename function sections to actual
  // output sections and de-register sections used for emission.
  if (!BC->HasRelocations) {
    for (auto &BFI : BC->getBinaryFunctions()) {
      auto &Function = BFI.second;
      if (auto Section = Function.getCodeSection())
        BC->deregisterSection(*Section);
      Function.CodeSectionName = Function.getOriginSectionName();
      if (Function.isSplit()) {
        if (auto ColdSection = Function.getColdCodeSection())
          BC->deregisterSection(*ColdSection);
        Function.ColdCodeSectionName = ".bolt.text";
      }
    }
  }

  if (opts::PrintCacheMetrics) {
    outs() << "BOLT-INFO: cache metrics after emitting functions:\n";
    CacheMetrics::printAll(BC->getSortedFunctions());
  }

  if (opts::KeepTmp)
    TempOut->keep();
}

void RewriteInstance::linkRuntime() {
  OLT->setProcessAllSections(false);
  std::string Dir = llvm::sys::path::parent_path(ToolPath);
  SmallString<128> P(Dir);
  P = llvm::sys::path::parent_path(Dir);
  llvm::sys::path::append(P, "lib", opts::RuntimeInstrumentationLib);
  std::string LibPath = P.str();
  if (!llvm::sys::fs::exists(LibPath)) {
    errs() << "BOLT-ERROR: instrumentation library not found: " << LibPath
           << "\n";
    exit(1);
  }
  ErrorOr<std::unique_ptr<MemoryBuffer>> MaybeBuf =
      MemoryBuffer::getFile(LibPath, -1, false);
  check_error(MaybeBuf.getError(), LibPath);
  std::unique_ptr<MemoryBuffer> B = std::move(MaybeBuf.get());
  file_magic Magic = identify_magic(B->getBuffer());

  if (Magic == file_magic::archive) {
    Error Err = Error::success();
    object::Archive Archive(B.get()->getMemBufferRef(), Err);
    for (auto &C : Archive.children(Err)) {
      auto ChildKey = ES->allocateVModule();
      auto ChildBuf =
          MemoryBuffer::getMemBuffer(cantFail(C.getMemoryBufferRef()));
      cantFail(OLT->addObject(ChildKey, std::move(ChildBuf)));
      cantFail(OLT->emitAndFinalize(ChildKey));
    }
    check_error(std::move(Err), B->getBufferIdentifier());
  } else if (Magic == file_magic::elf_relocatable ||
             Magic == file_magic::elf_shared_object) {
    auto K2 = ES->allocateVModule();
    cantFail(OLT->addObject(K2, std::move(B)));
    cantFail(OLT->emitAndFinalize(K2));
  } else {
    errs() << "BOLT-ERROR: unrecognized instrumentation library format: "
           << LibPath << "\n";
    exit(1);
  }
  InstrumentationRuntimeFiniAddress =
      cantFail(OLT->findSymbol("__bolt_instr_fini", false).getAddress());
  if (!InstrumentationRuntimeFiniAddress) {
    errs() << "BOLT-ERROR: instrumentation library does not define "
              "__bolt_instr_fini: "
           << LibPath << "\n";
    exit(1);
  }
  InstrumentationRuntimeStartAddress =
      cantFail(OLT->findSymbol("__bolt_instr_start", false).getAddress());
  if (!InstrumentationRuntimeStartAddress) {
    errs() << "BOLT-ERROR: instrumentation library does not define "
              "__bolt_instr_start: "
           << LibPath << "\n";
    exit(1);
  }
  outs() << "BOLT-INFO: output linked against instrumentation runtime "
            "library, lib entry point is 0x"
         << Twine::utohexstr(InstrumentationRuntimeFiniAddress) << "\n";
  outs() << "BOLT-INFO: clear procedure is 0x"
         << Twine::utohexstr(
                cantFail(OLT->findSymbol("__bolt_instr_clear_counters", false)
                             .getAddress()))
         << "\n";
}

void RewriteInstance::updateMetadata() {
  updateSDTMarkers();

  if (!opts::UpdateDebugSections)
    return;

  NamedRegionTimer T("updateDebugInfo", "update debug info", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);
  DebugInfoRewriter->updateDebugInfo();
}

void RewriteInstance::updateSDTMarkers() {
  NamedRegionTimer T("updateSDTMarkers", "update SDT markers", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);

  SectionPatchers[".note.stapsdt"] = llvm::make_unique<SimpleBinaryPatcher>();
  auto *SDTNotePatcher = static_cast<SimpleBinaryPatcher *>(
      SectionPatchers[".note.stapsdt"].get());
  for (auto &SDTInfoKV : BC->SDTMarkers) {
    const auto OriginalAddress = SDTInfoKV.first;
    auto &SDTInfo = SDTInfoKV.second;
    const auto *F = BC->getBinaryFunctionContainingAddress(OriginalAddress);
    if (!F)
      continue;
    const auto NewAddress = F->translateInputToOutputAddress(OriginalAddress);
    SDTNotePatcher->addLE64Patch(SDTInfo.PCOffset, NewAddress);
  }
}

void RewriteInstance::emitFunctions(MCStreamer *Streamer) {
  auto emit = [&](const std::vector<BinaryFunction *> &Functions) {
    const auto HasProfile = BC->NumProfiledFuncs > 0;
    const uint32_t OriginalBranchBoundaryAlign = X86AlignBranchBoundary;
    for (auto *Function : Functions) {
      if (!BC->HasRelocations &&
          (!Function->isSimple() || !opts::shouldProcess(*Function)))
        continue;

      DEBUG(dbgs() << "BOLT: generating code for function \""
                   << *Function << "\" : "
                   << Function->getFunctionNumber() << '\n');

      bool Emitted{false};
      // Turn off Intel JCC Erratum mitigation for cold code if requested
      if (HasProfile && opts::X86AlignBranchBoundaryHotOnly &&
          !Function->hasValidProfile())
        X86AlignBranchBoundary = 0;

      Emitted |= emitFunction(*Streamer, *Function, /*EmitColdPart=*/false);

      if (Function->isSplit()) {
        if (opts::X86AlignBranchBoundaryHotOnly)
          X86AlignBranchBoundary = 0;
        Emitted |= emitFunction(*Streamer, *Function, /*EmitColdPart=*/true);
      }
      X86AlignBranchBoundary = OriginalBranchBoundaryAlign;

      if (Emitted)
        Function->setEmitted(/*KeepCFG=*/opts::PrintCacheMetrics);
    }
  };

  // Mark the start of hot text.
  if (opts::HotText) {
    Streamer->SwitchSection(BC->getTextSection());
    Streamer->EmitLabel(BC->getHotTextStartSymbol());
  }

  // Emit functions in sorted order.
  std::vector<BinaryFunction *> SortedFunctions = BC->getSortedFunctions();
  emit(SortedFunctions);

  // Emit functions added by BOLT.
  emit(BC->getInjectedBinaryFunctions());

  // Mark the end of hot text.
  if (opts::HotText) {
    Streamer->SwitchSection(BC->getTextSection());
    Streamer->EmitLabel(BC->getHotTextEndSymbol());
  }
}

void RewriteInstance::mapFileSections(orc::VModuleKey Key) {
  mapCodeSections(Key);
  mapDataSections(Key);
}

std::vector<BinarySection *>
RewriteInstance::getCodeSections() {
  std::vector<BinarySection *> CodeSections;
  for (auto &Section : BC->textSections()) {
    if (Section.hasValidSectionID())
      CodeSections.emplace_back(&Section);
  };

  auto compareSections = [&](const BinarySection *A, const BinarySection *B) {
    // Place movers before anything else.
    if (A->getName() == BC->getHotTextMoverSectionName())
      return true;
    if (B->getName() == BC->getHotTextMoverSectionName())
      return false;

    // Depending on the option, put main text at the beginning or at the end.
    if (opts::HotFunctionsAtEnd) {
      return B->getName() == BC->getMainCodeSectionName();
    } else {
      return A->getName() == BC->getMainCodeSectionName();
    }
  };

  // Determine the order of sections.
  std::stable_sort(CodeSections.begin(), CodeSections.end(), compareSections);

  return CodeSections;
}

void RewriteInstance::mapCodeSections(orc::VModuleKey Key) {
  auto TextSection = BC->getUniqueSectionByName(BC->getMainCodeSectionName());
  assert(TextSection && ".text section not found in output");

  if (BC->HasRelocations) {
    assert(TextSection->hasValidSectionID() && ".text section should be valid");

    // Populate the list of sections to be allocated.
    auto CodeSections = getCodeSections();
    DEBUG(dbgs() << "Code sections in the order of output:\n";
      for (const auto *Section : CodeSections) {
        dbgs() << Section->getName() << '\n';
      });

    uint64_t PaddingSize{0}; // size of padding required at the end

    // Allocate sections starting at a given Address.
    auto allocateAt = [&](uint64_t Address) {
      for (auto *Section : CodeSections) {
        Address = alignTo(Address, Section->getAlignment());
        Section->setOutputAddress(Address);
        Address += Section->getOutputSize();
      }

      // Make sure we allocate enough space for huge pages.
      if (opts::HotText) {
        auto HotTextEnd = TextSection->getOutputAddress() +
                          TextSection->getOutputSize();
        HotTextEnd = alignTo(HotTextEnd, BC->PageAlign);
        if (HotTextEnd > Address) {
          PaddingSize = HotTextEnd - Address;
          Address = HotTextEnd;
        }
      }
      return Address;
    };

    // Check if we can fit code in the original .text
    bool AllocationDone{false};
    if (opts::UseOldText) {
      const auto CodeSize = allocateAt(BC->OldTextSectionAddress) -
                            BC->OldTextSectionAddress;

      if (CodeSize <= BC->OldTextSectionSize) {
        outs() << "BOLT-INFO: using original .text for new code with 0x"
               << Twine::utohexstr(BC->PageAlign) << " alignment\n";
        AllocationDone = true;
      } else {
        errs() << "BOLT-WARNING: original .text too small to fit the new code"
               << " using 0x" << Twine::utohexstr(BC->PageAlign)
               << " page alignment. " << CodeSize
               << " bytes needed, have " << BC->OldTextSectionSize
               << " bytes available.\n";
        opts::UseOldText = false;
      }
    }

    if (!AllocationDone) {
      NextAvailableAddress = allocateAt(NextAvailableAddress);
    }

    // Do the mapping for ORC layer based on the allocation.
    for (auto *Section : CodeSections) {
      DEBUG(dbgs() << "BOLT: mapping " << Section->getName()
                   << " at 0x" << Twine::utohexstr(Section->getAllocAddress())
                   << " to 0x" << Twine::utohexstr(Section->getOutputAddress())
                   << '\n');
      OLT->mapSectionAddress(Key, Section->getSectionID(),
                             Section->getOutputAddress());
      Section->setFileOffset(
          getFileOffsetForAddress(Section->getOutputAddress()));
    }

    // Check if we need to insert a padding section for hot text.
    if (PaddingSize && !opts::UseOldText) {
      outs() << "BOLT-INFO: padding code to 0x"
             << Twine::utohexstr(NextAvailableAddress)
             << " to accommodate hot text\n";
    }

    return;
  }

  // Processing in non-relocation mode.
  auto NewTextSectionStartAddress = NextAvailableAddress;

  // Prepare .text section for injected functions
  if (TextSection->hasValidSectionID()) {
    uint64_t NewTextSectionOffset = 0;
    auto Padding = OffsetToAlignment(NewTextSectionStartAddress,
                                     BC->PageAlign);
    NextAvailableAddress += Padding;
    NewTextSectionStartAddress = NextAvailableAddress;
    NewTextSectionOffset = getFileOffsetForAddress(NextAvailableAddress);
    NextAvailableAddress += Padding + TextSection->getOutputSize();
    TextSection->setOutputAddress(NewTextSectionStartAddress);
    TextSection->setFileOffset(NewTextSectionOffset);

    DEBUG(dbgs() << "BOLT: mapping .text 0x"
                 << Twine::utohexstr(TextSection->getAllocAddress())
                 << " to 0x" << Twine::utohexstr(NewTextSectionStartAddress)
                 << '\n');
    OLT->mapSectionAddress(Key, TextSection->getSectionID(),
                           NewTextSectionStartAddress);
  }

  for (auto &BFI : BC->getBinaryFunctions()) {
    auto &Function = BFI.second;
    if (!Function.isSimple() || !opts::shouldProcess(Function))
      continue;

    auto TooLarge = false;
    auto FuncSection = Function.getCodeSection();
    assert(FuncSection && "cannot find section for function");
    FuncSection->setOutputAddress(Function.getAddress());
    DEBUG(dbgs() << "BOLT: mapping 0x"
                 << Twine::utohexstr(FuncSection->getAllocAddress())
                 << " to 0x" << Twine::utohexstr(Function.getAddress())
                 << '\n');
    OLT->mapSectionAddress(Key, FuncSection->getSectionID(),
                           Function.getAddress());
    Function.setImageAddress(FuncSection->getAllocAddress());
    Function.setImageSize(FuncSection->getOutputSize());
    if (Function.getImageSize() > Function.getMaxSize()) {
      TooLarge = true;
      FailedAddresses.emplace_back(Function.getAddress());
    }

    // Map jump tables if updating in-place.
    if (opts::JumpTables == JTS_BASIC) {
      for (auto &JTI : Function.JumpTables) {
        auto *JT = JTI.second;
        auto &Section = JT->getOutputSection();
        Section.setOutputAddress(JT->getAddress());
        DEBUG(dbgs() << "BOLT-DEBUG: mapping " << Section.getName()
                     << " to 0x" << Twine::utohexstr(JT->getAddress())
                     << '\n');
        OLT->mapSectionAddress(Key, Section.getSectionID(),
                               JT->getAddress());
      }
    }

    if (!Function.isSplit())
      continue;

    auto ColdSection = Function.getColdCodeSection();
    assert(ColdSection && "cannot find section for cold part");
    // Cold fragments are aligned at 16 bytes.
    NextAvailableAddress = alignTo(NextAvailableAddress, 16);
    auto &ColdPart = Function.cold();
    if (TooLarge) {
      // The corresponding FDE will refer to address 0.
      ColdPart.setAddress(0);
      ColdPart.setImageAddress(0);
      ColdPart.setImageSize(0);
      ColdPart.setFileOffset(0);
    } else {
      ColdPart.setAddress(NextAvailableAddress);
      ColdPart.setImageAddress(ColdSection->getAllocAddress());
      ColdPart.setImageSize(ColdSection->getOutputSize());
      ColdPart.setFileOffset(getFileOffsetForAddress(NextAvailableAddress));
      ColdSection->setOutputAddress(ColdPart.getAddress());
    }

    DEBUG(dbgs() << "BOLT: mapping cold fragment 0x"
                 << Twine::utohexstr(ColdPart.getImageAddress())
                 << " to 0x"
                 << Twine::utohexstr(ColdPart.getAddress())
                 << " with size "
                 << Twine::utohexstr(ColdPart.getImageSize()) << '\n');
    OLT->mapSectionAddress(Key, ColdSection->getSectionID(),
                           ColdPart.getAddress());

    NextAvailableAddress += ColdPart.getImageSize();
  }

  // Add the new text section aggregating all existing code sections.
  // This is pseudo-section that serves a purpose of creating a corresponding
  // entry in section header table.
  auto NewTextSectionSize = NextAvailableAddress - NewTextSectionStartAddress;
  if (NewTextSectionSize) {
    const auto Flags = BinarySection::getFlags(/*IsReadOnly=*/true,
                                               /*IsText=*/true,
                                               /*IsAllocatable=*/true);
    auto &Section = BC->registerOrUpdateSection(BOLTSecPrefix + ".text",
                                                ELF::SHT_PROGBITS,
                                                Flags,
                                                /*Data=*/nullptr,
                                                NewTextSectionSize,
                                                16);
    Section.setOutputAddress(NewTextSectionStartAddress);
    Section.setFileOffset(
      getFileOffsetForAddress(NewTextSectionStartAddress));
  }
}

void RewriteInstance::mapDataSections(orc::VModuleKey Key) {
  // Map special sections to their addresses in the output image.
  // These are the sections that we generate via MCStreamer.
  // The order is important.
  std::vector<std::string> Sections = {
      ".eh_frame", ".eh_frame_old", ".gcc_except_table",
      ".rodata",   ".rodata.cold",  ".bolt.instr.counters"};
  for (auto &SectionName : Sections) {
    auto Section = BC->getUniqueSectionByName(SectionName);
    if (!Section || !Section->isAllocatable() || !Section->isFinalized())
      continue;
    NextAvailableAddress = alignTo(NextAvailableAddress,
                                   Section->getAlignment());
    DEBUG(dbgs() << "BOLT: mapping section " << SectionName << " (0x"
                 << Twine::utohexstr(Section->getAllocAddress())
                 << ") to 0x" << Twine::utohexstr(NextAvailableAddress)
                 << ":0x" << Twine::utohexstr(NextAvailableAddress +
                                              Section->getOutputSize())
                 << '\n');

    OLT->mapSectionAddress(Key, Section->getSectionID(), NextAvailableAddress);
    Section->setOutputAddress(NextAvailableAddress);
    Section->setFileOffset(getFileOffsetForAddress(NextAvailableAddress));

    NextAvailableAddress += Section->getOutputSize();
  }

  // Handling for sections with relocations.
  for (const auto &Section : BC->sections()) {
    if (!Section.hasRelocations() || !Section.hasSectionRef())
      continue;

    StringRef SectionName = Section.getName();
    auto OrgSection =
      BC->getUniqueSectionByName(OrgSecPrefix + std::string(SectionName));
    if (!OrgSection ||
        !OrgSection->isAllocatable() ||
        !OrgSection->isFinalized())
      continue;

    if (OrgSection->getOutputAddress()) {
      DEBUG(dbgs() << "BOLT-DEBUG: section " << SectionName
                   << " is already mapped at 0x"
                   << Twine::utohexstr(OrgSection->getOutputAddress()) << '\n');
      continue;
    }
    DEBUG(dbgs() << "BOLT: mapping original section " << SectionName << " (0x"
                 << Twine::utohexstr(OrgSection->getAllocAddress())
                 << ") to 0x" << Twine::utohexstr(Section.getAddress())
                 << '\n');

    OLT->mapSectionAddress(Key, OrgSection->getSectionID(),
                           Section.getAddress());

    OrgSection->setOutputAddress(Section.getAddress());
    OrgSection->setFileOffset(Section.getContents().data() -
                              InputFile->getData().data());
  }
}

void RewriteInstance::mapExtraSections(orc::VModuleKey Key) {
  assert(BC->HasRelocations && "Unsupported in non-relocation mode");

  for (auto &Section : BC->allocatableSections()) {
    if (Section.getOutputAddress() || !Section.hasValidSectionID())
      continue;
    NextAvailableAddress =
        alignTo(NextAvailableAddress, Section.getAlignment());
    Section.setOutputAddress(NextAvailableAddress);
    NextAvailableAddress += Section.getOutputSize();

    DEBUG(dbgs() << "BOLT: (extra) mapping " << Section.getName()
          << " at 0x" << Twine::utohexstr(Section.getAllocAddress())
          << " to 0x" << Twine::utohexstr(Section.getOutputAddress())
          << '\n');

    OLT->mapSectionAddress(Key, Section.getSectionID(),
                           Section.getOutputAddress());
    Section.setFileOffset(
      getFileOffsetForAddress(Section.getOutputAddress()));
  }
}

void RewriteInstance::updateOutputValues(const MCAsmLayout &Layout) {
  for (auto &BFI : BC->getBinaryFunctions()) {
    auto &Function = BFI.second;
    Function.updateOutputValues(Layout);
  }

  for (auto *InjectedFunction : BC->getInjectedBinaryFunctions()) {
    InjectedFunction->updateOutputValues(Layout);
  }
}

void RewriteInstance::emitDataSections(MCStreamer *Streamer) {
  for (const auto &Section : BC->sections()) {
    if (!Section.hasRelocations() || !Section.hasSectionRef())
      continue;

    StringRef SectionName = Section.getName();
    assert(SectionName != ".eh_frame" && "should not emit .eh_frame as data");
    std::string EmitName = Section.isReordered()
      ? std::string(Section.getOutputName())
      : OrgSecPrefix + std::string(SectionName);
    Section.emitAsData(*Streamer, EmitName);
  }
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
  (void)ModdedGnuStack;
  bool AddedSegment = false;
  (void)AddedSegment;

  // Copy existing program headers with modifications.
  for (auto &Phdr : cantFail(Obj->program_headers())) {
    auto NewPhdr = Phdr;
    if (PHDRTableAddress && Phdr.p_type == ELF::PT_PHDR) {
      NewPhdr.p_offset = PHDRTableOffset;
      NewPhdr.p_vaddr = PHDRTableAddress;
      NewPhdr.p_paddr = PHDRTableAddress;
      NewPhdr.p_filesz = sizeof(NewPhdr) * Phnum;
      NewPhdr.p_memsz = sizeof(NewPhdr) * Phnum;
    } else if (Phdr.p_type == ELF::PT_GNU_EH_FRAME) {
      auto EHFrameHdrSec = BC->getUniqueSectionByName(".eh_frame_hdr");
      if (EHFrameHdrSec &&
          EHFrameHdrSec->isAllocatable() &&
          EHFrameHdrSec->isFinalized()) {
        NewPhdr.p_offset = EHFrameHdrSec->getFileOffset();
        NewPhdr.p_vaddr = EHFrameHdrSec->getOutputAddress();
        NewPhdr.p_paddr = EHFrameHdrSec->getOutputAddress();
        NewPhdr.p_filesz = EHFrameHdrSec->getOutputSize();
        NewPhdr.p_memsz = EHFrameHdrSec->getOutputSize();
      }
    } else if (opts::UseGnuStack && Phdr.p_type == ELF::PT_GNU_STACK) {
      NewPhdr.p_type = ELF::PT_LOAD;
      NewPhdr.p_offset = NewTextSegmentOffset;
      NewPhdr.p_vaddr = NewTextSegmentAddress;
      NewPhdr.p_paddr = NewTextSegmentAddress;
      NewPhdr.p_filesz = NewTextSegmentSize;
      NewPhdr.p_memsz = NewTextSegmentSize;
      NewPhdr.p_flags = ELF::PF_X | ELF::PF_R;
      // FIXME: Currently instrumentation is experimental and the runtime data
      // is emitted with code, thus everything needs to be writable
      if (opts::Instrument)
        NewPhdr.p_flags |= ELF::PF_W;
      NewPhdr.p_align = BC->PageAlign;
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
      // FIXME: experimental instrumentation hack described above
      NewTextPhdr.p_flags = ELF::PF_X | ELF::PF_R;
      if (opts::Instrument)
        NewTextPhdr.p_flags |= ELF::PF_W;
      NewTextPhdr.p_align = BC->PageAlign;
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

namespace {

/// Write padding to \p OS such that its current \p Offset becomes aligned
/// at \p Alignment. Return new (aligned) offset.
uint64_t appendPadding(raw_pwrite_stream &OS,
                       uint64_t Offset,
                       uint64_t Alignment) {
  if (!Alignment)
    return Offset;

  const auto PaddingSize = OffsetToAlignment(Offset, Alignment);
  for (unsigned I = 0; I < PaddingSize; ++I)
    OS.write((unsigned char)0);
  return Offset + PaddingSize;
}

}

void RewriteInstance::rewriteNoteSections() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  auto &OS = Out->os();

  uint64_t NextAvailableOffset = getFileOffsetForAddress(NextAvailableAddress);
  assert(NextAvailableOffset >= FirstNonAllocatableOffset &&
         "next available offset calculation failure");
  OS.seek(NextAvailableOffset);

  // Copy over non-allocatable section contents and update file offsets.
  for (auto &Section : cantFail(Obj->sections())) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;
    if (Section.sh_flags & ELF::SHF_ALLOC)
      continue;

    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");

    if (shouldStrip(Section, SectionName))
      continue;

    // Insert padding as needed.
    NextAvailableOffset =
      appendPadding(OS, NextAvailableOffset, Section.sh_addralign);

    // New section size.
    uint64_t Size = 0;

    // Copy over section contents unless it's one of the sections we overwrite.
    if (!willOverwriteSection(SectionName)) {
      Size = Section.sh_size;
      std::string Data = InputFile->getData().substr(Section.sh_offset, Size);
      auto SectionPatchersIt = SectionPatchers.find(SectionName);
      if (SectionPatchersIt != SectionPatchers.end()) {
        (*SectionPatchersIt->second).patchBinary(Data);
      }
      OS << Data;

      // Add padding as the section extension might rely on the alignment.
      Size = appendPadding(OS, Size, Section.sh_addralign);
    }

    // Perform section post-processing.
    auto BSec = BC->getUniqueSectionByName(SectionName);
    uint8_t *SectionData = nullptr;
    if (BSec && !BSec->isAllocatable()) {
      assert(BSec->getAlignment() <= Section.sh_addralign &&
             "alignment exceeds value in file");

      if (BSec->getAllocAddress()) {
        SectionData = BSec->getOutputData();
        DEBUG(dbgs() << "BOLT-DEBUG: " << (Size ? "appending" : "writing")
                     << " contents to section "
                     << SectionName << '\n');
        OS.write(reinterpret_cast<char *>(SectionData),
                 BSec->getOutputSize());
        Size += BSec->getOutputSize();
      }

      BSec->setFileOffset(NextAvailableOffset);
      BSec->flushPendingRelocations(OS);
    }

    // Set/modify section info.
    auto &NewSection =
      BC->registerOrUpdateNoteSection(SectionName,
                                      SectionData,
                                      Size,
                                      Section.sh_addralign,
                                      BSec ? BSec->isReadOnly() : false,
                                      BSec ? BSec->getELFType()
                                           : ELF::SHT_PROGBITS);
    NewSection.setOutputAddress(0);
    NewSection.setFileOffset(NextAvailableOffset);

    NextAvailableOffset += Size;
  }

  // Write new note sections.
  for (auto &Section : BC->nonAllocatableSections()) {
    if (Section.getFileOffset() || !Section.getAllocAddress())
      continue;

    assert(!Section.hasPendingRelocations() && "cannot have pending relocs");

    NextAvailableOffset = appendPadding(OS, NextAvailableOffset,
                                        Section.getAlignment());
    Section.setFileOffset(NextAvailableOffset);

    DEBUG(dbgs() << "BOLT-DEBUG: writing out new section "
                 << Section.getName() << " of size " << Section.getOutputSize()
                 << " at offset 0x" << Twine::utohexstr(Section.getFileOffset())
                 << '\n');

    OS.write(Section.getOutputContents().data(), Section.getOutputSize());
    NextAvailableOffset += Section.getOutputSize();
  }
}

template <typename ELFT>
void RewriteInstance::finalizeSectionStringTable(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();

  // Pre-populate section header string table.
  for (auto &Section : cantFail(Obj->sections())) {
    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");
    SHStrTab.add(SectionName);
    auto OutputSectionName = getOutputSectionName(Obj, Section);
    if (OutputSectionName != SectionName) {
      AllSHStrTabStrings.emplace_back(SHStrTabPool.intern(OutputSectionName));
      SHStrTab.add(*AllSHStrTabStrings.back());
    }
  }
  for (const auto &Section : BC->sections()) {
    SHStrTab.add(Section.getName());
  }
  SHStrTab.finalize();

  const auto SHStrTabSize = SHStrTab.getSize();
  uint8_t *DataCopy = new uint8_t[SHStrTabSize];
  memset(DataCopy, 0, SHStrTabSize);
  SHStrTab.write(DataCopy);
  BC->registerOrUpdateNoteSection(".shstrtab",
                                  DataCopy,
                                  SHStrTabSize,
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true,
                                  ELF::SHT_STRTAB);
}

void RewriteInstance::addBoltInfoSection() {
  std::string DescStr;
  raw_string_ostream DescOS(DescStr);

  DescOS << "BOLT revision: " << BoltRevision << ", "
         << "command line:";
  for (auto I = 0; I < Argc; ++I) {
    DescOS << " " << Argv[I];
  }
  DescOS.flush();

  // Encode as GNU GOLD VERSION so it is easily printable by 'readelf -n'
  const auto BoltInfo =
      BinarySection::encodeELFNote("GNU", DescStr, 4 /*NT_GNU_GOLD_VERSION*/);
  BC->registerOrUpdateNoteSection(".note.bolt_info", copyByteArray(BoltInfo),
                                  BoltInfo.size(),
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true, ELF::SHT_NOTE);
}

void RewriteInstance::addBATSection() {
  BC->registerOrUpdateNoteSection(BoltAddressTranslation::SECTION_NAME, nullptr,
                                  0,
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true, ELF::SHT_NOTE);
}

void RewriteInstance::encodeBATSection() {
  std::string DescStr;
  raw_string_ostream DescOS(DescStr);

  BAT->write(DescOS);
  DescOS.flush();

  const auto BoltInfo =
      BinarySection::encodeELFNote("BOLT", DescStr, BinarySection::NT_BOLT_BAT);
  BC->registerOrUpdateNoteSection(BoltAddressTranslation::SECTION_NAME,
                                  copyByteArray(BoltInfo), BoltInfo.size(),
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true, ELF::SHT_NOTE);
}

template<typename ELFObjType, typename ELFShdrTy>
std::string RewriteInstance::getOutputSectionName(const ELFObjType *Obj,
                                                  const ELFShdrTy &Section) {
  if (Section.sh_type == ELF::SHT_NULL)
    return "";

  StringRef SectionName =
      cantFail(Obj->getSectionName(&Section), "cannot get section name");

  if ((Section.sh_flags & ELF::SHF_ALLOC) && willOverwriteSection(SectionName))
    return OrgSecPrefix + SectionName.str();

  return SectionName;
}

template <typename ELFShdrTy>
bool RewriteInstance::shouldStrip(const ELFShdrTy &Section,
                                  StringRef SectionName) {
  // Strip non-allocatable relocation sections.
  if (!(Section.sh_flags & ELF::SHF_ALLOC) && Section.sh_type == ELF::SHT_RELA)
    return true;

  // Strip debug sections if not updating them.
  if (isDebugSection(SectionName) && !opts::UpdateDebugSections)
    return true;

  return false;
}

template <typename ELFT, typename ELFShdrTy>
std::vector<ELFShdrTy> RewriteInstance::getOutputSections(
    ELFObjectFile<ELFT> *File, std::vector<uint32_t> &NewSectionIndex) {
  auto *Obj = File->getELFFile();
  auto Sections = cantFail(Obj->sections());

  // Keep track of section header entries together with their name.
  std::vector<std::pair<std::string, ELFShdrTy>> OutputSections;
  auto addSection = [&](const std::string &Name, const ELFShdrTy &Section) {
    auto NewSection = Section;
    NewSection.sh_name = SHStrTab.getOffset(Name);
    OutputSections.emplace_back(std::make_pair(Name, std::move(NewSection)));
  };

  // Copy over entries for original allocatable sections using modified name.
  for (auto &Section : Sections) {
    // Always ignore this section.
    if (Section.sh_type == ELF::SHT_NULL) {
      OutputSections.emplace_back(std::make_pair("", Section));
      continue;
    }

    if (!(Section.sh_flags & ELF::SHF_ALLOC))
      continue;

    addSection(getOutputSectionName(Obj, Section), Section);
  }

  for (const auto &Section : BC->allocatableSections()) {
    if (!Section.isFinalized())
      continue;

    if (Section.getName().startswith(OrgSecPrefix) || Section.isAnonymous()) {
      if (opts::Verbosity)
        outs() << "BOLT-INFO: not writing section header for section "
               << Section.getName() << '\n';
      continue;
    }

    if (opts::Verbosity >= 1)
      outs() << "BOLT-INFO: writing section header for "
             << Section.getName() << '\n';
    ELFShdrTy NewSection;
    NewSection.sh_type = ELF::SHT_PROGBITS;
    NewSection.sh_addr = Section.getOutputAddress();
    NewSection.sh_offset = Section.getFileOffset();
    NewSection.sh_size = Section.getOutputSize();
    NewSection.sh_entsize = 0;
    NewSection.sh_flags = Section.getELFFlags();
    NewSection.sh_link = 0;
    NewSection.sh_info = 0;
    NewSection.sh_addralign = Section.getAlignment();
    addSection(Section.getName(), NewSection);
  }

  // Sort all allocatable sections by their offset.
  std::stable_sort(OutputSections.begin(), OutputSections.end(),
      [] (const std::pair<std::string, ELFShdrTy> &A,
          const std::pair<std::string, ELFShdrTy> &B) {
        return A.second.sh_offset < B.second.sh_offset;
      });

  // Fix section sizes to prevent overlapping.
  for (uint32_t Index = 1; Index < OutputSections.size(); ++Index) {
    auto &PrevSection = OutputSections[Index - 1].second;
    auto &Section = OutputSections[Index].second;

    // Skip TBSS section size adjustment.
    if (PrevSection.sh_type == ELF::SHT_NOBITS &&
        (PrevSection.sh_flags & ELF::SHF_TLS))
      continue;

    if (PrevSection.sh_addr + PrevSection.sh_size > Section.sh_addr) {
      if (opts::Verbosity > 1) {
        outs() << "BOLT-INFO: adjusting size for section "
               << OutputSections[Index - 1].first << '\n';
      }
      PrevSection.sh_size = Section.sh_addr > PrevSection.sh_addr ?
        Section.sh_addr - PrevSection.sh_addr : 0;
    }
  }

  uint64_t LastFileOffset = 0;

  // Copy over entries for non-allocatable sections performing necessary
  // adjustments.
  for (auto &Section : Sections) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;
    if (Section.sh_flags & ELF::SHF_ALLOC)
      continue;

    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");

    if (shouldStrip(Section, SectionName))
      continue;

    auto BSec = BC->getUniqueSectionByName(SectionName);
    assert(BSec && "missing section info for non-allocatable section");

    auto NewSection = Section;
    NewSection.sh_offset = BSec->getFileOffset();
    NewSection.sh_size = BSec->getOutputSize();

    if (NewSection.sh_type == ELF::SHT_SYMTAB) {
      NewSection.sh_info = NumLocalSymbols;
    }

    addSection(SectionName, NewSection);

    LastFileOffset = BSec->getFileOffset();
  }

  // Create entries for new non-allocatable sections.
  for (auto &Section : BC->nonAllocatableSections()) {
    if (Section.getFileOffset() <= LastFileOffset)
      continue;

    if (opts::Verbosity >= 1) {
      outs() << "BOLT-INFO: writing section header for "
             << Section.getName() << '\n';
    }
    ELFShdrTy NewSection;
    NewSection.sh_type = Section.getELFType();
    NewSection.sh_addr = 0;
    NewSection.sh_offset = Section.getFileOffset();
    NewSection.sh_size = Section.getOutputSize();
    NewSection.sh_entsize = 0;
    NewSection.sh_flags = Section.getELFFlags();
    NewSection.sh_link = 0;
    NewSection.sh_info = 0;
    NewSection.sh_addralign = Section.getAlignment();

    addSection(Section.getName(), NewSection);
  }

  // Assign indices to sections.
  std::unordered_map<std::string, uint64_t> NameToIndex;
  for (uint32_t Index = 1; Index < OutputSections.size(); ++Index) {
    const auto &SectionName = OutputSections[Index].first;
    NameToIndex[SectionName] = Index;
    if (auto Section = BC->getUniqueSectionByName(SectionName))
      Section->setIndex(Index);
  }

  // Update section index mapping
  NewSectionIndex.clear();
  NewSectionIndex.resize(Sections.size(), 0);
  for (auto &Section : Sections) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;

    auto OrgIndex = std::distance(Sections.begin(), &Section);
    auto SectionName = getOutputSectionName(Obj, Section);

    // Some sections are stripped
    if (!NameToIndex.count(SectionName))
      continue;

    NewSectionIndex[OrgIndex] = NameToIndex[SectionName];
  }

  std::vector<ELFShdrTy> SectionsOnly(OutputSections.size());
  std::transform(OutputSections.begin(), OutputSections.end(),
                 SectionsOnly.begin(),
                 [](std::pair<std::string, ELFShdrTy> &SectionInfo) {
                   return SectionInfo.second;
                 });

  return SectionsOnly;
}

// Rewrite section header table inserting new entries as needed. The sections
// header table size itself may affect the offsets of other sections,
// so we are placing it at the end of the binary.
//
// As we rewrite entries we need to track how many sections were inserted
// as it changes the sh_link value. We map old indices to new ones for
// existing sections.
template <typename ELFT>
void RewriteInstance::patchELFSectionHeaderTable(ELFObjectFile<ELFT> *File) {
  using Elf_Shdr = typename ELFObjectFile<ELFT>::Elf_Shdr;
  auto &OS = Out->os();
  auto *Obj = File->getELFFile();

  std::vector<uint32_t> NewSectionIndex;
  auto OutputSections = getOutputSections(File, NewSectionIndex);
  DEBUG(
    dbgs() << "BOLT-DEBUG: old to new section index mapping:\n";
    for (uint64_t I = 0; I < NewSectionIndex.size(); ++I) {
      dbgs() << "  " << I << " -> " << NewSectionIndex[I] << '\n';
    }
  );

  // Align starting address for section header table.
  auto SHTOffset = OS.tell();
  SHTOffset = appendPadding(OS, SHTOffset, sizeof(Elf_Shdr));

  // Write all section header entries while patching section references.
  for (auto &Section : OutputSections) {
    Section.sh_link = NewSectionIndex[Section.sh_link];
    if (Section.sh_type == ELF::SHT_REL || Section.sh_type == ELF::SHT_RELA) {
      if (Section.sh_info)
        Section.sh_info = NewSectionIndex[Section.sh_info];
    }
    OS.write(reinterpret_cast<const char *>(&Section), sizeof(Section));
  }

  // Fix ELF header.
  auto NewEhdr = *Obj->getHeader();

  if (BC->HasRelocations) {
    NewEhdr.e_entry = opts::Instrument ? InstrumentationRuntimeStartAddress
                                       : getNewFunctionAddress(NewEhdr.e_entry);
    assert(NewEhdr.e_entry && "cannot find new address for entry point");
  }
  NewEhdr.e_phoff = PHDRTableOffset;
  NewEhdr.e_phnum = Phnum;
  NewEhdr.e_shoff = SHTOffset;
  NewEhdr.e_shnum = OutputSections.size();
  NewEhdr.e_shstrndx = NewSectionIndex[NewEhdr.e_shstrndx];
  OS.pwrite(reinterpret_cast<const char *>(&NewEhdr), sizeof(NewEhdr), 0);
}

template <typename ELFT>
void RewriteInstance::patchELFSymTabs(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();
  // Set pointer at the end of the output file, so we can pwrite old symbol
  // tables if we need to.
  uint64_t NextAvailableOffset = getFileOffsetForAddress(NextAvailableAddress);
  assert(NextAvailableOffset >= FirstNonAllocatableOffset &&
         "next available offset calculation failure");
  Out->os().seek(NextAvailableOffset);

  using Elf_Shdr = typename ELFObjectFile<ELFT>::Elf_Shdr;
  using Elf_Sym  = typename ELFObjectFile<ELFT>::Elf_Sym;

  // Compute a preview of how section indices will change after rewriting, so
  // we can properly update the symbol table based on new section indices.
  std::vector<uint32_t> NewSectionIndex;
  getOutputSections(File, NewSectionIndex);

  auto updateSymbolTable =
    [&](bool PatchExisting,
        const Elf_Shdr *Section,
        std::function<void(size_t, const Elf_Sym &)> Write,
        std::function<size_t(StringRef)> AddToStrTab) {
    auto StringSection = cantFail(Obj->getStringTableForSymtab(*Section));
    unsigned IsHotTextUpdated = 0;
    unsigned IsHotDataUpdated = 0;

    std::vector<Elf_Sym> Symbols;

    std::map<const BinaryFunction *, uint64_t> IslandSizes;
    auto getConstantIslandSize = [&IslandSizes](const BinaryFunction *BF) {
      auto Itr = IslandSizes.find(BF);
      if (Itr != IslandSizes.end())
        return Itr->second;
      return IslandSizes[BF] = BF->estimateConstantIslandSize();
    };

    // Add symbols of injected functions
    for (BinaryFunction *Function : BC->getInjectedBinaryFunctions()) {
      Elf_Sym NewSymbol;
      NewSymbol.st_shndx = Function->getCodeSection()->getIndex();
      NewSymbol.st_value = Function->getOutputAddress();
      NewSymbol.st_name = AddToStrTab(Function->getOneName());
      NewSymbol.st_size = Function->getOutputSize();
      NewSymbol.st_other = 0;
      NewSymbol.setBindingAndType(ELF::STB_LOCAL, ELF::STT_FUNC);
      Symbols.emplace_back(NewSymbol);

      if (Function->isSplit()) {
        auto NewColdSym = NewSymbol;
        NewColdSym.setType(ELF::STT_NOTYPE);
        SmallVector<char, 256> Buf;
        NewColdSym.st_name = AddToStrTab(
            Twine(Function->getPrintName()).concat(".cold.0").toStringRef(Buf));
        NewColdSym.st_value = Function->cold().getAddress();
        NewColdSym.st_size = Function->cold().getImageSize();
        Symbols.emplace_back(NewColdSym);
      }
    }

    for (const Elf_Sym &Symbol : cantFail(Obj->symbols(Section))) {
      auto NewSymbol = Symbol;

      const auto *Function = BC->getBinaryFunctionAtAddress(Symbol.st_value,
                                                            /*Shallow=*/true);

      // Some section symbols may be mistakenly associated with the first
      // function emitted in the section. Dismiss if it is a section symbol.
      if (Function &&
          !Function->getPLTSymbol() &&
          NewSymbol.getType() != ELF::STT_SECTION) {
        NewSymbol.st_value = Function->getOutputAddress();
        NewSymbol.st_size = Function->getOutputSize();
        NewSymbol.st_shndx = Function->getCodeSection()->getIndex();
        if (!PatchExisting && Function->isSplit()) {
          auto NewColdSym = NewSymbol;
          SmallVector<char, 256> Buf;
          NewColdSym.st_name =
              AddToStrTab(Twine(cantFail(Symbol.getName(StringSection)))
                              .concat(".cold.0")
                              .toStringRef(Buf));
          NewColdSym.st_shndx = Function->getColdCodeSection()->getIndex();
          NewColdSym.st_value = Function->cold().getAddress();
          NewColdSym.st_size = Function->cold().getImageSize();
          NewColdSym.setBindingAndType(ELF::STB_LOCAL, ELF::STT_FUNC);
          Symbols.emplace_back(NewColdSym);
        }
        if (!PatchExisting && Function->hasConstantIsland()) {
          auto DataMark = Function->getOutputDataAddress();
          auto CISize = getConstantIslandSize(Function);
          auto CodeMark = DataMark + CISize;
          auto DataMarkSym = NewSymbol;
          DataMarkSym.st_name = AddToStrTab("$d");
          DataMarkSym.st_value = DataMark;
          DataMarkSym.st_size = 0;
          DataMarkSym.setType(ELF::STT_NOTYPE);
          DataMarkSym.setBinding(ELF::STB_LOCAL);
          auto CodeMarkSym = DataMarkSym;
          CodeMarkSym.st_name = AddToStrTab("$x");
          CodeMarkSym.st_value = CodeMark;
          Symbols.emplace_back(DataMarkSym);
          Symbols.emplace_back(CodeMarkSym);
        }
        if (!PatchExisting && Function->hasConstantIsland() &&
            Function->isSplit()) {
          auto DataMark = Function->getOutputColdDataAddress();
          auto CISize = getConstantIslandSize(Function);
          auto CodeMark = DataMark + CISize;
          auto DataMarkSym = NewSymbol;
          DataMarkSym.st_name = AddToStrTab("$d");
          DataMarkSym.st_value = DataMark;
          DataMarkSym.st_size = 0;
          DataMarkSym.setType(ELF::STT_NOTYPE);
          DataMarkSym.setBinding(ELF::STB_LOCAL);
          auto CodeMarkSym = DataMarkSym;
          CodeMarkSym.st_name = AddToStrTab("$x");
          CodeMarkSym.st_value = CodeMark;
          Symbols.emplace_back(DataMarkSym);
          Symbols.emplace_back(CodeMarkSym);
        }
      } else {
        uint32_t OldSectionIndex = NewSymbol.st_shndx;
        // Check if the original section where this symbol links to is
        // code that we may have reordered.
        auto ExpectedSec = File->getELFFile()->getSection(Symbol.st_shndx);
        bool IsCodeSym{false};
        if (ExpectedSec) {
          auto Section = *ExpectedSec;
          IsCodeSym = (Section->sh_type == ELF::SHT_PROGBITS &&
                       Section->sh_flags & ELF::SHF_ALLOC &&
                       Section->sh_flags & ELF::SHF_EXECINSTR &&
                       !(Section->sh_flags & ELF::SHF_WRITE));
        } else {
          consumeError(ExpectedSec.takeError());
        }
        // Try to fetch a containing function to check if this symbol is
        // a secondary entry point of it
        if (!Function && IsCodeSym && NewSymbol.getType() == ELF::STT_FUNC) {
          Function =
              BC->getBinaryFunctionContainingAddress(NewSymbol.st_value,
                                                     /*CheckPastEnd=*/false,
                                                     /*UseMaxSize=*/true,
                                                     /*Shallow=*/true);
        }
        auto *BD = !Function ? BC->getBinaryDataAtAddress(NewSymbol.st_value)
                             : nullptr;
        auto Output =
            Function && !Function->getPLTSymbol()
                ? Function->translateInputToOutputAddress(NewSymbol.st_value)
                : 0;

        // Handle secondary entry points for this function
        // (when Function->getAddress() != Symbol.st_value)
        if (Output && NewSymbol.getType() != ELF::STT_SECTION) {
          NewSymbol.st_value = Output;
          // Force secondary entry points to have zero size
          NewSymbol.st_size = 0;
          NewSymbol.st_shndx = Output >= Function->cold().getAddress() &&
                                       Output < Function->cold().getImageSize()
                                   ? Function->getColdCodeSection()->getIndex()
                                   : Function->getCodeSection()->getIndex();
          OldSectionIndex = ELF::SHN_LORESERVE;
        } else if (BD && BD->isMoved() && !BD->isJumpTable()) {
          assert((!BD->getSize() || !NewSymbol.st_size ||
                  NewSymbol.st_size == BD->getSize()) &&
                 "sizes must match");

          auto &OutputSection = BD->getOutputSection();

          assert(OutputSection.getIndex());
          DEBUG(dbgs() << "BOLT-DEBUG: moving " << BD->getName() << " from "
                       << *BC->getSectionNameForAddress(NewSymbol.st_value)
                       << " (" << NewSymbol.st_shndx << ") to "
                       << OutputSection.getName() << " ("
                       << OutputSection.getIndex() << ")\n");
          OldSectionIndex = ELF::SHN_LORESERVE;
          NewSymbol.st_shndx = OutputSection.getIndex();

          // TODO: use getNewValueForSymbol()?
          NewSymbol.st_value = BD->getOutputAddress();
        }

        if (OldSectionIndex < ELF::SHN_LORESERVE) {
          NewSymbol.st_shndx = NewSectionIndex[OldSectionIndex];
        }

        // Detect local syms in the text section that we didn't update
        // and were preserved by the linker to support relocations against
        // .text (t15274167). Remove then from the symtab.
        if (NewSymbol.getType() == ELF::STT_NOTYPE &&
            NewSymbol.getBinding() == ELF::STB_LOCAL &&
            NewSymbol.st_size == 0 &&
            IsCodeSym) {
          // This will cause the symbol to not be emitted if we are
          // creating a new symtab from scratch instead of patching one.
          if (!PatchExisting)
            continue;
          // If patching an existing symtab, patch this value to zero.
          NewSymbol.st_value = 0;
        }
      }

      auto SymbolName = Symbol.getName(StringSection);
      assert(SymbolName && "cannot get symbol name");

      auto updateSymbolValue = [&](const StringRef Name, unsigned &IsUpdated) {
        NewSymbol.st_value = getNewValueForSymbol(Name);
        NewSymbol.st_shndx = ELF::SHN_ABS;
        outs() << "BOLT-INFO: setting " << Name << " to 0x"
               << Twine::utohexstr(NewSymbol.st_value) << '\n';
        ++IsUpdated;
        return true;
      };

      if (opts::HotText && (*SymbolName == "__hot_start" ||
                            *SymbolName == "__hot_end"))
        updateSymbolValue(*SymbolName, IsHotTextUpdated);

      if (opts::HotData && (*SymbolName == "__hot_data_start" ||
                            *SymbolName == "__hot_data_end"))
        updateSymbolValue(*SymbolName, IsHotDataUpdated);

      if (opts::UpdateEnd && *SymbolName == "_end") {
        NewSymbol.st_value = getNewValueForSymbol(*SymbolName);
        NewSymbol.st_shndx = ELF::SHN_ABS;
        outs() << "BOLT-INFO: setting " << *SymbolName << " to 0x"
               << Twine::utohexstr(NewSymbol.st_value) << '\n';
      }

      if (PatchExisting) {
        Write((&Symbol - cantFail(Obj->symbols(Section)).begin()) *
                  sizeof(Elf_Sym),
              NewSymbol);
      } else {
        Symbols.emplace_back(NewSymbol);
      }
    }

    if (PatchExisting)
      return;

    assert((!IsHotTextUpdated || IsHotTextUpdated == 2) &&
           "either none or both __hot_start/__hot_end symbols were expected");
    assert((!IsHotDataUpdated || IsHotDataUpdated == 2) &&
           "either none or both __hot_data_start/__hot_data_end symbols were "
           "expected");

    auto addSymbol = [&](const std::string &Name) {
      Elf_Sym Symbol;
      Symbol.st_value = getNewValueForSymbol(Name);
      Symbol.st_shndx = ELF::SHN_ABS;
      Symbol.st_name = AddToStrTab(Name);
      Symbol.st_size = 0;
      Symbol.st_other = 0;
      Symbol.setBindingAndType(ELF::STB_WEAK, ELF::STT_NOTYPE);

      outs() << "BOLT-INFO: setting " << Name << " to 0x"
             << Twine::utohexstr(Symbol.st_value) << '\n';

      Symbols.emplace_back(Symbol);
    };

    if (opts::HotText && !IsHotTextUpdated) {
      addSymbol("__hot_start");
      addSymbol("__hot_end");
    }

    if (opts::HotData && !IsHotDataUpdated) {
      addSymbol("__hot_data_start");
      addSymbol("__hot_data_end");
    }

    // Put local symbols at the beginning.
    std::stable_sort(Symbols.begin(), Symbols.end(),
                     [](const Elf_Sym &A, const Elf_Sym &B) {
                       if (A.getBinding() == ELF::STB_LOCAL &&
                           B.getBinding() != ELF::STB_LOCAL)
                         return true;
                       return false;
                     });

    for (const auto &Symbol : Symbols) {
      Write(0, Symbol);
    }
  };

  // Update dynamic symbol table.
  const Elf_Shdr *DynSymSection = nullptr;
  for (const auto &Section : cantFail(Obj->sections())) {
    if (Section.sh_type == ELF::SHT_DYNSYM) {
      DynSymSection = &Section;
      break;
    }
  }
  assert(DynSymSection && "no dynamic symbol table found");
  updateSymbolTable(/*patch existing table?*/ true,
                    DynSymSection,
                    [&](size_t Offset, const Elf_Sym &Sym) {
                      Out->os().pwrite(reinterpret_cast<const char *>(&Sym),
                                       sizeof(Elf_Sym),
                                       DynSymSection->sh_offset + Offset);
                    },
                    [](StringRef) -> size_t { return 0; });

  // (re)create regular symbol table.
  const Elf_Shdr *SymTabSection = nullptr;
  for (const auto &Section : cantFail(Obj->sections())) {
    if (Section.sh_type == ELF::SHT_SYMTAB) {
      SymTabSection = &Section;
      break;
    }
  }
  if (!SymTabSection) {
    errs() << "BOLT-WARNING: no symbol table found\n";
    return;
  }

  const Elf_Shdr *StrTabSection =
      cantFail(Obj->getSection(SymTabSection->sh_link));
  std::string NewContents;
  std::string NewStrTab =
      File->getData().substr(StrTabSection->sh_offset, StrTabSection->sh_size);
  auto SecName = cantFail(Obj->getSectionName(SymTabSection));
  auto StrSecName = cantFail(Obj->getSectionName(StrTabSection));

  NumLocalSymbols = 0;
  updateSymbolTable(/*patch existing table?*/ false,
                    SymTabSection,
                    [&](size_t Offset, const Elf_Sym &Sym) {
                      if (Sym.getBinding() == ELF::STB_LOCAL)
                        ++NumLocalSymbols;
                      NewContents.append(reinterpret_cast<const char *>(&Sym),
                                         sizeof(Elf_Sym));
                    },
                    [&](StringRef Str) {
                      size_t Idx = NewStrTab.size();
                      NewStrTab.append(Str.data(), Str.size());
                      NewStrTab.append(1, '\0');
                      return Idx;
                    });

  BC->registerOrUpdateNoteSection(SecName,
                                  copyByteArray(NewContents),
                                  NewContents.size(),
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true,
                                  ELF::SHT_SYMTAB);

  BC->registerOrUpdateNoteSection(StrSecName,
                                  copyByteArray(NewStrTab),
                                  NewStrTab.size(),
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true,
                                  ELF::SHT_STRTAB);
}

template <typename ELFT>
void
RewriteInstance::patchELFAllocatableRelaSections(ELFObjectFile<ELFT> *File) {
  auto &OS = Out->os();

  for (auto &RelaSection : BC->allocatableRelaSections()) {
    for (const auto &Rel : RelaSection.getSectionRef().relocations()) {
      if (Rel.getType() == ELF::R_X86_64_IRELATIVE ||
          Rel.getType() == ELF::R_X86_64_RELATIVE) {
        DataRefImpl DRI = Rel.getRawDataRefImpl();
        const auto *RelA = File->getRela(DRI);
        auto Address = RelA->r_addend;
        auto NewAddress = getNewFunctionAddress(Address);
        if (!NewAddress)
          continue;
        DEBUG(dbgs() << "BOLT-DEBUG: patching (I)RELATIVE "
                     << RelaSection.getName() << " entry 0x"
                     << Twine::utohexstr(Address) << " with 0x"
                     << Twine::utohexstr(NewAddress) << '\n');
        auto NewRelA = *RelA;
        NewRelA.r_addend = NewAddress;
        OS.pwrite(reinterpret_cast<const char *>(&NewRelA), sizeof(NewRelA),
          reinterpret_cast<const char *>(RelA) - File->getData().data());
      }
    }
  }
}

template <typename ELFT>
void RewriteInstance::patchELFGOT(ELFObjectFile<ELFT> *File) {
  auto &OS = Out->os();

  SectionRef GOTSection;
  for (const auto &Section : File->sections()) {
    StringRef SectionName;
    Section.getName(SectionName);
    if (SectionName == ".got") {
      GOTSection = Section;
      break;
    }
  }
  if (!GOTSection.getObject()) {
    errs() << "BOLT-INFO: no .got section found\n";
    return;
  }

  StringRef GOTContents;
  GOTSection.getContents(GOTContents);
  for (const uint64_t *GOTEntry =
        reinterpret_cast<const uint64_t *>(GOTContents.data());
       GOTEntry < reinterpret_cast<const uint64_t *>(GOTContents.data() +
                                                            GOTContents.size());
       ++GOTEntry) {
    if (auto NewAddress = getNewFunctionAddress(*GOTEntry)) {
      DEBUG(dbgs() << "BOLT-DEBUG: patching GOT entry 0x"
                   << Twine::utohexstr(*GOTEntry) << " with 0x"
                   << Twine::utohexstr(NewAddress) << '\n');
      OS.pwrite(reinterpret_cast<const char *>(&NewAddress), sizeof(NewAddress),
        reinterpret_cast<const char *>(GOTEntry) - File->getData().data());
    }
  }
}

template <typename ELFT>
void RewriteInstance::patchELFDynamic(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();
  auto &OS = Out->os();

  using Elf_Phdr = typename ELFFile<ELFT>::Elf_Phdr;
  using Elf_Dyn  = typename ELFFile<ELFT>::Elf_Dyn;

  // Locate DYNAMIC by looking through program headers.
  uint64_t DynamicOffset = 0;
  const Elf_Phdr *DynamicPhdr = 0;
  for (auto &Phdr : cantFail(Obj->program_headers())) {
    if (Phdr.p_type == ELF::PT_DYNAMIC) {
      DynamicOffset = Phdr.p_offset;
      DynamicPhdr = &Phdr;
      assert(Phdr.p_memsz == Phdr.p_filesz && "dynamic sizes should match");
      break;
    }
  }
  assert(DynamicPhdr && "missing dynamic in ELF binary");

  bool ZNowSet = false;

  // Go through all dynamic entries and patch functions addresses with
  // new ones.
  const Elf_Dyn *DTB = cantFail(Obj->dynamic_table_begin(DynamicPhdr),
                                "error accessing dynamic table");
  const Elf_Dyn *DTE = cantFail(Obj->dynamic_table_end(DynamicPhdr),
                                "error accessing dynamic table");
  for (auto *DE = DTB; DE != DTE; ++DE) {
    auto NewDE = *DE;
    bool ShouldPatch = true;
    switch (DE->getTag()) {
    default:
      ShouldPatch = false;
      break;
    case ELF::DT_INIT:
    case ELF::DT_FINI:
      if (BC->HasRelocations) {
        if (auto NewAddress = getNewFunctionAddress(DE->getPtr())) {
          DEBUG(dbgs() << "BOLT-DEBUG: patching dynamic entry of type "
                       << DE->getTag() << '\n');
          NewDE.d_un.d_ptr = NewAddress;
        }
      }
      if (opts::Instrument && DE->getTag() == ELF::DT_FINI)
        NewDE.d_un.d_ptr = InstrumentationRuntimeFiniAddress;
      break;
    case ELF::DT_FLAGS:
      if (BC->RequiresZNow) {
        NewDE.d_un.d_val |= ELF::DF_BIND_NOW;
        ZNowSet = true;
      }
      break;
    case ELF::DT_FLAGS_1:
      if (BC->RequiresZNow) {
        NewDE.d_un.d_val |= ELF::DF_1_NOW;
        ZNowSet = true;
      }
      break;
    }
    if (ShouldPatch) {
      OS.pwrite(reinterpret_cast<const char *>(&NewDE), sizeof(NewDE),
                DynamicOffset + (DE - DTB) * sizeof(*DE));
    }
  }

  if (BC->RequiresZNow && !ZNowSet) {
    errs() << "BOLT-ERROR: output binary requires immediate relocation "
              "processing which depends on DT_FLAGS or DT_FLAGS_1 presence in "
              ".dynamic. Please re-link the binary with -znow.\n";
    exit(1);
  }
}

template <typename ELFT>
void RewriteInstance::readELFDynamic(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();

  using Elf_Phdr = typename ELFFile<ELFT>::Elf_Phdr;
  using Elf_Dyn  = typename ELFFile<ELFT>::Elf_Dyn;

  if (!opts::Instrument || !BC->HasRelocations)
    return;

  // Locate DYNAMIC by looking through program headers.
  const Elf_Phdr *DynamicPhdr = 0;
  for (auto &Phdr : cantFail(Obj->program_headers())) {
    if (Phdr.p_type == ELF::PT_DYNAMIC) {
      DynamicPhdr = &Phdr;
      assert(Phdr.p_memsz == Phdr.p_filesz && "dynamic sizes should match");
      break;
    }
  }
  assert(DynamicPhdr && "missing dynamic in ELF binary");

  // Go through all dynamic entries and patch functions addresses with
  // new ones.
  const Elf_Dyn *DTB = cantFail(Obj->dynamic_table_begin(DynamicPhdr),
                                "error accessing dynamic table");
  const Elf_Dyn *DTE = cantFail(Obj->dynamic_table_end(DynamicPhdr),
                                "error accessing dynamic table");
  bool FiniFound = false;
  for (auto *DE = DTB; DE != DTE; ++DE) {
    if (DE->getTag() != ELF::DT_FINI)
      continue;
    const auto *Function = BC->getBinaryFunctionAtAddress(DE->getPtr());
    if (!Function) {
      errs() << "BOLT-ERROR: failed to locate fini function.\n";
      exit(1);
    }
    FiniFunction = Function;
    FiniFound = true;
  }

  if (!FiniFound) {
    errs()
        << "BOLT-ERROR: input binary lacks DT_INIT/FINI entry in the dynamic "
           "section but instrumentation currently relies on patching "
           "DT_FINI to write the profile.\n";
    exit(1);
  }

  // Read start function
  auto Ehdr = *Obj->getHeader();
  const auto *Function = BC->getBinaryFunctionAtAddress(Ehdr.e_entry);
  if (!Function) {
    errs() << "BOLT-ERROR: failed to locate _start function.\n";
    exit(1);
  }
  StartFunction = Function;
}


uint64_t RewriteInstance::getNewFunctionAddress(uint64_t OldAddress) {
  const auto *Function = BC->getBinaryFunctionAtAddress(OldAddress,
                                                        /*Shallow=*/true);
  if (!Function)
    return 0;
  return Function->getOutputAddress();
}

void RewriteInstance::rewriteFile() {
  auto &OS = Out->os();

  // We obtain an asm-specific writer so that we can emit nops in an
  // architecture-specific way at the end of the function.
  auto MCE = BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  auto MAB =
      BC->TheTarget->createMCAsmBackend(*BC->STI, *BC->MRI, MCTargetOptions());
  std::unique_ptr<MCStreamer> Streamer(BC->TheTarget->createMCObjectStreamer(
      *BC->TheTriple, *BC->Ctx, std::unique_ptr<MCAsmBackend>(MAB), OS,
      std::unique_ptr<MCCodeEmitter>(MCE), *BC->STI,
      /* RelaxAll */ false,
      /*IncrementalLinkerCompatible */ false,
      /* DWARFMustBeAtTheEnd */ false));

  auto &Writer = static_cast<MCObjectStreamer *>(Streamer.get())
                     ->getAssembler()
                     .getWriter();

  // Make sure output stream has enough reserved space, otherwise
  // pwrite() will fail.
  auto Offset = OS.seek(getFileOffsetForAddress(NextAvailableAddress));
  (void)Offset;
  assert(Offset == getFileOffsetForAddress(NextAvailableAddress) &&
         "error resizing output file");

  if (!BC->HasRelocations) {
    // Overwrite functions in the output file.
    uint64_t CountOverwrittenFunctions = 0;
    uint64_t OverwrittenScore = 0;
    for (auto &BFI : BC->getBinaryFunctions()) {
      auto &Function = BFI.second;

      if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
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

      if (Function.isSplit() && (Function.cold().getImageAddress() == 0 ||
                                 Function.cold().getImageSize() == 0))
        continue;

      OverwrittenScore += Function.getFunctionScore();
      // Overwrite function in the output file.
      if (opts::Verbosity >= 2) {
        outs() << "BOLT: rewriting function \"" << Function << "\"\n";
      }
      OS.pwrite(reinterpret_cast<char *>(Function.getImageAddress()),
                Function.getImageSize(),
                Function.getFileOffset());

      // Write nops at the end of the function.
      auto Pos = OS.tell();
      OS.seek(Function.getFileOffset() + Function.getImageSize());
      MAB->writeNopData(Function.getMaxSize() - Function.getImageSize(),
                        &Writer);
      OS.seek(Pos);

      // Write jump tables if updating in-place.
      if (opts::JumpTables == JTS_BASIC) {
        for (auto &JTI : Function.JumpTables) {
          auto *JT = JTI.second;
          auto &Section = JT->getOutputSection();
          Section.setFileOffset(getFileOffsetForAddress(JT->getAddress()));
          assert(Section.getFileOffset() && "no matching offset in file");
          OS.pwrite(reinterpret_cast<const char*>(Section.getOutputData()),
                    Section.getOutputSize(),
                    Section.getFileOffset());
        }
      }

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
        outs() << "BOLT: rewriting function \"" << Function
               << "\" (cold part)\n";
      }
      OS.pwrite(reinterpret_cast<char*>(Function.cold().getImageAddress()),
                Function.cold().getImageSize(),
                Function.cold().getFileOffset());

      // FIXME: write nops after cold part too.

      ++CountOverwrittenFunctions;
      if (opts::MaxFunctions &&
          CountOverwrittenFunctions == opts::MaxFunctions) {
        outs() << "BOLT: maximum number of functions reached\n";
        break;
      }
    }

    // Print function statistics.
    outs() << "BOLT: " << CountOverwrittenFunctions
           << " out of " << BC->getBinaryFunctions().size()
           << " functions were overwritten.\n";
    if (BC->TotalScore != 0) {
      double Coverage = OverwrittenScore / (double) BC->TotalScore * 100.0;
      outs() << format("BOLT-INFO: rewritten functions cover %.2lf", Coverage)
             << "% of the execution count of simple functions of "
                "this binary\n";
    }
  }

  if (BC->HasRelocations && opts::TrapOldCode) {
    auto SavedPos = OS.tell();
    // Overwrite function body to make sure we never execute these instructions.
    for (auto &BFI : BC->getBinaryFunctions()) {
      auto &BF = BFI.second;
      if (!BF.getFileOffset())
        continue;
      OS.seek(BF.getFileOffset());
      for (unsigned I = 0; I < BF.getMaxSize(); ++I)
        OS.write((unsigned char)
            Streamer->getContext().getAsmInfo()->getTrapFillValue());
    }
    OS.seek(SavedPos);
  }

  // Write all non-local sections, i.e. those not emitted with the function.
  for (auto &Section : BC->allocatableSections()) {
    if (!Section.isFinalized() || !Section.getOutputData())
      continue;

    if (opts::Verbosity >= 1) {
      outs() << "BOLT: writing new section " << Section.getName()
             << "\n data at 0x" << Twine::utohexstr(Section.getAllocAddress())
             << "\n of size " << Section.getOutputSize()
             << "\n at offset " << Section.getFileOffset() << '\n';
    }
    OS.pwrite(reinterpret_cast<const char*>(Section.getOutputData()),
              Section.getOutputSize(),
              Section.getFileOffset());
  }

  // If .eh_frame is present create .eh_frame_hdr.
  if (EHFrameSection && EHFrameSection->isFinalized()) {
    writeEHFrameHeader();
  }

  // Add BOLT Addresses Translation maps to allow profile collection to
  // happen in the output binary
  if (opts::EnableBAT)
    addBATSection();

  // Patch program header table.
  patchELFPHDRTable();

  // Finalize memory image of section string table.
  finalizeSectionStringTable();

  // Update symbol tables.
  patchELFSymTabs();

  patchBuildID();

  if (opts::EnableBAT)
    encodeBATSection();

  // Copy non-allocatable sections once allocatable part is finished.
  rewriteNoteSections();

  // Patch dynamic section/segment.
  patchELFDynamic();

  if (BC->HasRelocations) {
    patchELFAllocatableRelaSections();
    patchELFGOT();
  }

  // Update ELF book-keeping info.
  patchELFSectionHeaderTable();

  if (opts::PrintSections) {
    outs() << "BOLT-INFO: Sections after processing:\n";
    BC->printSections(outs());
  }

  Out->keep();

  // If requested, open again the binary we just wrote to dump its EH Frame
  if (opts::DumpEHFrame) {
    Expected<OwningBinary<Binary>> BinaryOrErr =
        createBinary(opts::OutputFilename);
    if (auto E = BinaryOrErr.takeError())
      report_error(opts::OutputFilename, std::move(E));
    Binary &Binary = *BinaryOrErr.get().getBinary();

    if (auto *E = dyn_cast<ELFObjectFileBase>(&Binary)) {
      auto DwCtx = DWARFContext::create(*E);
      const auto &EHFrame = DwCtx->getEHFrame();
      outs() << "BOLT-INFO: Dumping rewritten .eh_frame\n";
      EHFrame->dump(outs(), &*BC->MRI, NoneType());
    }
  }
}

void RewriteInstance::writeEHFrameHeader() {
  DWARFDebugFrame NewEHFrame(true, EHFrameSection->getOutputAddress());
  NewEHFrame.parse(DWARFDataExtractor(EHFrameSection->getOutputContents(),
                                      BC->AsmInfo->isLittleEndian(),
                                      BC->AsmInfo->getCodePointerSize()));

  auto OldEHFrameSection = BC->getUniqueSectionByName(".eh_frame_old");
  assert(OldEHFrameSection && "expected .eh_frame_old to be present");
  DWARFDebugFrame OldEHFrame(true, OldEHFrameSection->getOutputAddress());
  OldEHFrame.parse(DWARFDataExtractor(OldEHFrameSection->getOutputContents(),
                                      BC->AsmInfo->isLittleEndian(),
                                      BC->AsmInfo->getCodePointerSize()));

  DEBUG(dbgs() << "BOLT: writing a new .eh_frame_hdr\n");

  NextAvailableAddress =
    appendPadding(Out->os(), NextAvailableAddress, EHFrameHdrAlign);

  const auto EHFrameHdrOutputAddress = NextAvailableAddress;
  const auto EHFrameHdrFileOffset =
    getFileOffsetForAddress(NextAvailableAddress);

  auto NewEHFrameHdr =
      CFIRdWrt->generateEHFrameHeader(OldEHFrame,
                                      NewEHFrame,
                                      EHFrameHdrOutputAddress,
                                      FailedAddresses);

  assert(Out->os().tell() == EHFrameHdrFileOffset && "offset mismatch");
  Out->os().write(NewEHFrameHdr.data(), NewEHFrameHdr.size());

  const auto Flags = BinarySection::getFlags(/*IsReadOnly=*/true,
                                             /*IsText=*/false,
                                             /*IsAllocatable=*/true);
  auto &EHFrameHdrSec = BC->registerOrUpdateSection(".eh_frame_hdr",
                                                    ELF::SHT_PROGBITS,
                                                    Flags,
                                                    nullptr,
                                                    NewEHFrameHdr.size(),
                                                    /*Alignment=*/1);
  EHFrameHdrSec.setFileOffset(EHFrameHdrFileOffset);
  EHFrameHdrSec.setOutputAddress(EHFrameHdrOutputAddress);

  NextAvailableAddress += EHFrameHdrSec.getOutputSize();

  // Merge .eh_frame and .eh_frame_old so that gdb can locate all FDEs.
  const auto EHFrameSectionSize = (OldEHFrameSection->getOutputAddress() +
                                   OldEHFrameSection->getOutputSize() -
                                   EHFrameSection->getOutputAddress());

  EHFrameSection =
    BC->registerOrUpdateSection(".eh_frame",
                                EHFrameSection->getELFType(),
                                EHFrameSection->getELFFlags(),
                                EHFrameSection->getOutputData(),
                                EHFrameSectionSize,
                                EHFrameSection->getAlignment());

  BC->deregisterSection(*OldEHFrameSection);

  DEBUG(dbgs() << "BOLT-DEBUG: size of .eh_frame after merge is "
               << EHFrameSection->getOutputSize() << '\n');
}

uint64_t RewriteInstance::getFileOffsetForAddress(uint64_t Address) const {
  // Check if it's possibly part of the new segment.
  if (Address >= NewTextSegmentAddress) {
    return Address - NewTextSegmentAddress + NewTextSegmentOffset;
  }

  // Find an existing segment that matches the address.
  const auto SegmentInfoI = BC->EFMM->SegmentMapInfo.upper_bound(Address);
  if (SegmentInfoI == BC->EFMM->SegmentMapInfo.begin())
    return 0;

  const auto &SegmentInfo = std::prev(SegmentInfoI)->second;
  if (Address < SegmentInfo.Address ||
      Address >= SegmentInfo.Address + SegmentInfo.FileSize)
    return 0;

  return  SegmentInfo.FileOffset + Address - SegmentInfo.Address;
}

bool RewriteInstance::willOverwriteSection(StringRef SectionName) {
  for (auto &OverwriteName : SectionsToOverwrite) {
    if (SectionName == OverwriteName)
      return true;
  }
  for (auto &OverwriteName : DebugSectionsToOverwrite) {
    if (SectionName == OverwriteName)
      return true;
  }

  auto Section = BC->getUniqueSectionByName(SectionName);
  return Section && Section->isAllocatable() && Section->isFinalized();
}

bool RewriteInstance::isDebugSection(StringRef SectionName) {
  if (SectionName.startswith(".debug_") || SectionName == ".gdb_index")
    return true;

  return false;
}
