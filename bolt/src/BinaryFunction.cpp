//===--- BinaryFunction.cpp - Interface for machine-level function --------===//
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
#include "BinaryFunction.h"
#include "DataReader.h"
#include "DynoStats.h"
#include "MCPlusBuilder.h"
#include "llvm/ADT/edit_distance.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"
#include <cxxabi.h>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>
#include <string>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltCategory;
extern cl::OptionCategory BoltOptCategory;
extern cl::OptionCategory BoltRelocCategory;

extern bool shouldProcess(const BinaryFunction &);

extern cl::opt<bool> UpdateDebugSections;
extern cl::opt<unsigned> Verbosity;

cl::opt<bool>
AlignBlocks("align-blocks",
  cl::desc("align basic blocks"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

cl::opt<MacroFusionType>
AlignMacroOpFusion("align-macro-fusion",
  cl::desc("fix instruction alignment for macro-fusion (x86 relocation mode)"),
  cl::init(MFT_HOT),
  cl::values(clEnumValN(MFT_NONE, "none",
               "do not insert alignment no-ops for macro-fusion"),
             clEnumValN(MFT_HOT, "hot",
               "only insert alignment no-ops on hot execution paths (default)"),
             clEnumValN(MFT_ALL, "all",
               "always align instructions to allow macro-fusion")),
  cl::ZeroOrMore,
  cl::cat(BoltRelocCategory));

cl::opt<bool>
PreserveBlocksAlignment("preserve-blocks-alignment",
  cl::desc("try to preserve basic block alignment"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

static cl::opt<bool>
DotToolTipCode("dot-tooltip-code",
  cl::desc("add basic block instructions as tool tips on nodes"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<JumpTableSupportLevel>
JumpTables("jump-tables",
  cl::desc("jump tables support (default=basic)"),
  cl::init(JTS_BASIC),
  cl::values(
      clEnumValN(JTS_NONE, "none",
                 "do not optimize functions with jump tables"),
      clEnumValN(JTS_BASIC, "basic",
                 "optimize functions with jump tables"),
      clEnumValN(JTS_MOVE, "move",
                 "move jump tables to a separate section"),
      clEnumValN(JTS_SPLIT, "split",
                 "split jump tables section into hot and cold based on "
                 "function execution frequency"),
      clEnumValN(JTS_AGGRESSIVE, "aggressive",
                 "aggressively split jump tables section based on usage "
                 "of the tables")),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

cl::opt<bool>
PrintDynoStats("dyno-stats",
  cl::desc("print execution info based on profile"),
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintDynoStatsOnly("print-dyno-stats-only",
  cl::desc("while printing functions output dyno-stats and skip instructions"),
  cl::init(false),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintJumpTables("print-jump-tables",
  cl::desc("print jump tables"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
PrintOnly("print-only",
  cl::CommaSeparated,
  cl::desc("list of functions to print"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
PrintOnlyRegex("print-only-regex",
  cl::CommaSeparated,
  cl::desc("list of function regexes to print"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
TimeBuild("time-build",
  cl::desc("print time spent constructing binary functions"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
TrapOnAVX512("trap-avx512",
  cl::desc("in relocation mode trap upon entry to any function that uses "
            "AVX-512 instructions (on by default)"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

bool shouldPrint(const BinaryFunction &Function) {
  if (PrintOnly.empty() && PrintOnlyRegex.empty())
    return true;

  for (auto &Name : opts::PrintOnly) {
    if (Function.hasName(Name)) {
      return true;
    }
  }

  for (auto &Name : opts::PrintOnlyRegex) {
    if (Function.hasNameRegex(Name)) {
      return true;
    }
  }

  return false;
}

} // namespace opts

namespace llvm {
namespace bolt {

constexpr unsigned BinaryFunction::MinAlign;
const char BinaryFunction::TimerGroupName[] = "buildfuncs";
const char BinaryFunction::TimerGroupDesc[] = "Build Binary Functions";

namespace {

template <typename R>
bool emptyRange(const R &Range) {
  return Range.begin() == Range.end();
}

/// Gets debug line information for the instruction located at the given
/// address in the original binary. The SMLoc's pointer is used
/// to point to this information, which is represented by a
/// DebugLineTableRowRef. The returned pointer is null if no debug line
/// information for this instruction was found.
SMLoc findDebugLineInformationForInstructionAt(
    uint64_t Address,
    DWARFUnitLineTable &ULT) {
  // We use the pointer in SMLoc to store an instance of DebugLineTableRowRef,
  // which occupies 64 bits. Thus, we can only proceed if the struct fits into
  // the pointer itself.
  assert(
      sizeof(decltype(SMLoc().getPointer())) >= sizeof(DebugLineTableRowRef) &&
      "Cannot fit instruction debug line information into SMLoc's pointer");

  SMLoc NullResult = DebugLineTableRowRef::NULL_ROW.toSMLoc();

  auto &LineTable = ULT.second;
  if (!LineTable)
    return NullResult;

  uint32_t RowIndex = LineTable->lookupAddress(Address);
  if (RowIndex == LineTable->UnknownRowIndex)
    return NullResult;

  assert(RowIndex < LineTable->Rows.size() &&
         "Line Table lookup returned invalid index.");

  decltype(SMLoc().getPointer()) Ptr;
  DebugLineTableRowRef *InstructionLocation =
    reinterpret_cast<DebugLineTableRowRef *>(&Ptr);

  InstructionLocation->DwCompileUnitIndex = ULT.first->getOffset();
  InstructionLocation->RowIndex = RowIndex + 1;

  return SMLoc::getFromPointer(Ptr);
}

} // namespace

uint64_t BinaryFunction::Count = 0;

const std::string *
BinaryFunction::hasNameRegex(const std::string &NameRegex) const {
  Regex MatchName(NameRegex);
  for (auto &Name : Names)
    if (MatchName.match(Name))
      return &Name;
  return nullptr;
}

std::string BinaryFunction::getDemangledName() const {
  StringRef MangledName = Names.back();
  MangledName = MangledName.substr(0, MangledName.find_first_of('/'));
  int Status = 0;
  char *const Name =
      abi::__cxa_demangle(MangledName.str().c_str(), 0, 0, &Status);
  const std::string NameStr(Status == 0 ? Name : MangledName);
  free(Name);
  return NameStr;
}

BinaryBasicBlock *
BinaryFunction::getBasicBlockContainingOffset(uint64_t Offset) {
  if (Offset > Size)
    return nullptr;

  if (BasicBlockOffsets.empty())
    return nullptr;

  /*
   * This is commented out because it makes BOLT too slow.
   * assert(std::is_sorted(BasicBlockOffsets.begin(),
   *                       BasicBlockOffsets.end(),
   *                       CompareBasicBlockOffsets())));
   */
  auto I = std::upper_bound(BasicBlockOffsets.begin(),
                            BasicBlockOffsets.end(),
                            BasicBlockOffset(Offset, nullptr),
                            CompareBasicBlockOffsets());
  assert(I != BasicBlockOffsets.begin() && "first basic block not at offset 0");
  --I;
  auto *BB = I->second;
  return (Offset < BB->getOffset() + BB->getOriginalSize()) ? BB : nullptr;
}

void BinaryFunction::markUnreachableBlocks() {
  std::stack<BinaryBasicBlock *> Stack;

  for (auto *BB : layout()) {
    BB->markValid(false);
  }

  // Add all entries and landing pads as roots.
  for (auto *BB : BasicBlocks) {
    if (BB->isEntryPoint() || BB->isLandingPad()) {
      Stack.push(BB);
      BB->markValid(true);
      continue;
    }
    // FIXME:
    // Also mark BBs with indirect jumps as reachable, since we do not
    // support removing unused jump tables yet (T29418024 / GH-issue20)
    for (const auto &Inst : *BB) {
      if (BC.MIB->getJumpTable(Inst)) {
        Stack.push(BB);
        BB->markValid(true);
        break;
      }
    }
  }

  // Determine reachable BBs from the entry point
  while (!Stack.empty()) {
    auto BB = Stack.top();
    Stack.pop();
    for (auto Succ : BB->successors()) {
      if (Succ->isValid())
        continue;
      Succ->markValid(true);
      Stack.push(Succ);
    }
  }
}

// Any unnecessary fallthrough jumps revealed after calling eraseInvalidBBs
// will be cleaned up by fixBranches().
std::pair<unsigned, uint64_t> BinaryFunction::eraseInvalidBBs() {
  BasicBlockOrderType NewLayout;
  unsigned Count = 0;
  uint64_t Bytes = 0;
  for (auto *BB : layout()) {
    if (BB->isValid()) {
      NewLayout.push_back(BB);
    } else {
      assert(!BB->isEntryPoint() && "all entry blocks must be valid");
      ++Count;
      Bytes += BC.computeCodeSize(BB->begin(), BB->end());
    }
  }
  BasicBlocksLayout = std::move(NewLayout);

  BasicBlockListType NewBasicBlocks;
  for (auto I = BasicBlocks.begin(), E = BasicBlocks.end(); I != E; ++I) {
    auto *BB = *I;
    if (BB->isValid()) {
      NewBasicBlocks.push_back(BB);
    } else {
      // Make sure the block is removed from the list of predecessors.
      BB->removeAllSuccessors();
      DeletedBasicBlocks.push_back(BB);
    }
  }
  BasicBlocks = std::move(NewBasicBlocks);

  assert(BasicBlocks.size() == BasicBlocksLayout.size());

  // Update CFG state if needed
  if (Count > 0)
    recomputeLandingPads();

  return std::make_pair(Count, Bytes);
}

bool BinaryFunction::isForwardCall(const MCSymbol *CalleeSymbol) const {
  // This function should work properly before and after function reordering.
  // In order to accomplish this, we use the function index (if it is valid).
  // If the function indices are not valid, we fall back to the original
  // addresses.  This should be ok because the functions without valid indices
  // should have been ordered with a stable sort.
  const auto *CalleeBF = BC.getFunctionForSymbol(CalleeSymbol);
  if (CalleeBF) {
    if(CalleeBF->isInjected())
      return true;

    if (hasValidIndex() && CalleeBF->hasValidIndex()) {
      return getIndex() < CalleeBF->getIndex();
    } else if (hasValidIndex() && !CalleeBF->hasValidIndex()) {
      return true;
    } else if (!hasValidIndex() && CalleeBF->hasValidIndex()) {
      return false;
    } else {
      return getAddress() < CalleeBF->getAddress();
    }
  } else {
    // Absolute symbol.
    auto *CalleeSI = BC.getBinaryDataByName(CalleeSymbol->getName());
    assert(CalleeSI && "unregistered symbol found");
    return CalleeSI->getAddress() > getAddress();
  }
}

void BinaryFunction::dump(bool PrintInstructions) const {
  print(dbgs(), "", PrintInstructions);
}

void BinaryFunction::print(raw_ostream &OS, std::string Annotation,
                           bool PrintInstructions) const {
  // FIXME: remove after #15075512 is done.
  if (!opts::shouldProcess(*this) || !opts::shouldPrint(*this))
    return;

  StringRef SectionName =
      IsInjected ? "<no input section>" : InputSection->getName();
  OS << "Binary Function \"" << *this << "\" " << Annotation << " {";
  if (Names.size() > 1) {
    OS << "\n  Other names : ";
    auto Sep = "";
    for (unsigned i = 0; i < Names.size() - 1; ++i) {
      OS << Sep << Names[i];
      Sep = "\n                ";
    }
  }
  OS << "\n  Number      : "   << FunctionNumber
     << "\n  State       : "   << CurrentState
     << "\n  Address     : 0x" << Twine::utohexstr(Address)
     << "\n  Size        : 0x" << Twine::utohexstr(Size)
     << "\n  MaxSize     : 0x" << Twine::utohexstr(MaxSize)
     << "\n  Offset      : 0x" << Twine::utohexstr(FileOffset)
     << "\n  Section     : "   << SectionName
     << "\n  Orc Section : "   << getCodeSectionName()
     << "\n  LSDA        : 0x" << Twine::utohexstr(getLSDAAddress())
     << "\n  IsSimple    : "   << IsSimple
     << "\n  IsSplit     : "   << isSplit()
     << "\n  BB Count    : "   << size();

  if (IsFragment) {
    OS << "\n  IsFragment  : true";
  }
  if (ParentFunction) {
    OS << "\n  Parent      : " << *ParentFunction;
  }
  if (!Fragments.empty()) {
    OS << "\n  Fragments   : ";
    auto Sep = "";
    for (auto *Frag : Fragments) {
      OS << Sep << *Frag;
      Sep = ", ";
    }
  }
  if (hasCFG()) {
    OS << "\n  Hash        : "   << Twine::utohexstr(hash());
  }
  if (FrameInstructions.size()) {
    OS << "\n  CFI Instrs  : "   << FrameInstructions.size();
  }
  if (BasicBlocksLayout.size()) {
    OS << "\n  BB Layout   : ";
    auto Sep = "";
    for (auto BB : BasicBlocksLayout) {
      OS << Sep << BB->getName();
      Sep = ", ";
    }
  }
  if (ImageAddress)
    OS << "\n  Image       : 0x" << Twine::utohexstr(ImageAddress);
  if (ExecutionCount != COUNT_NO_PROFILE) {
    OS << "\n  Exec Count  : " << ExecutionCount;
    OS << "\n  Profile Acc : " << format("%.1f%%", ProfileMatchRatio * 100.0f);
  }

  if (opts::PrintDynoStats && !BasicBlocksLayout.empty()) {
    OS << '\n';
    DynoStats dynoStats = getDynoStats(*this);
    OS << dynoStats;
  }

  OS << "\n}\n";

  if (opts::PrintDynoStatsOnly || !PrintInstructions || !BC.InstPrinter)
    return;

  // Offset of the instruction in function.
  uint64_t Offset{0};

  if (BasicBlocks.empty() && !Instructions.empty()) {
    // Print before CFG was built.
    for (const auto &II : Instructions) {
      Offset = II.first;

      // Print label if exists at this offset.
      auto LI = Labels.find(Offset);
      if (LI != Labels.end())
        OS << LI->second->getName() << ":\n";

      BC.printInstruction(OS, II.second, Offset, this);
    }
  }

  for (uint32_t I = 0, E = BasicBlocksLayout.size(); I != E; ++I) {
    auto BB = BasicBlocksLayout[I];
    if (I != 0 &&
        BB->isCold() != BasicBlocksLayout[I - 1]->isCold())
      OS << "-------   HOT-COLD SPLIT POINT   -------\n\n";

    OS << BB->getName() << " ("
       << BB->size() << " instructions, align : " << BB->getAlignment()
       << ")\n";

    if (BB->isEntryPoint())
      OS << "  Entry Point\n";

    if (BB->isLandingPad())
      OS << "  Landing Pad\n";

    uint64_t BBExecCount = BB->getExecutionCount();
    if (hasValidProfile()) {
      OS << "  Exec Count : ";
      if (BB->getExecutionCount() != BinaryBasicBlock::COUNT_NO_PROFILE)
        OS << BBExecCount << '\n';
      else
        OS << "<unknown>\n";
    }
    if (BB->getCFIState() >= 0) {
      OS << "  CFI State : " << BB->getCFIState() << '\n';
    }
    if (!BB->pred_empty()) {
      OS << "  Predecessors: ";
      auto Sep = "";
      for (auto Pred : BB->predecessors()) {
        OS << Sep << Pred->getName();
        Sep = ", ";
      }
      OS << '\n';
    }
    if (!BB->throw_empty()) {
      OS << "  Throwers: ";
      auto Sep = "";
      for (auto Throw : BB->throwers()) {
        OS << Sep << Throw->getName();
        Sep = ", ";
      }
      OS << '\n';
    }

    Offset = alignTo(Offset, BB->getAlignment());

    // Note: offsets are imprecise since this is happening prior to relaxation.
    Offset = BC.printInstructions(OS, BB->begin(), BB->end(), Offset, this);

    if (!BB->succ_empty()) {
      OS << "  Successors: ";
      // For more than 2 successors, sort them based on frequency.
      std::vector<uint64_t> Indices(BB->succ_size());
      std::iota(Indices.begin(), Indices.end(), 0);
      if (BB->succ_size() > 2 && BB->getKnownExecutionCount()) {
        std::stable_sort(Indices.begin(), Indices.end(),
                         [&](const uint64_t A, const uint64_t B) {
                           return BB->BranchInfo[B] < BB->BranchInfo[A];
                         });
      }
      auto Sep = "";
      for (unsigned I = 0; I < Indices.size(); ++I) {
        auto *Succ = BB->Successors[Indices[I]];
        auto &BI = BB->BranchInfo[Indices[I]];
        OS << Sep << Succ->getName();
        if (ExecutionCount != COUNT_NO_PROFILE &&
            BI.MispredictedCount != BinaryBasicBlock::COUNT_INFERRED) {
          OS << " (mispreds: " << BI.MispredictedCount
             << ", count: " << BI.Count << ")";
        } else if (ExecutionCount != COUNT_NO_PROFILE &&
                   BI.Count != BinaryBasicBlock::COUNT_NO_PROFILE) {
          OS << " (inferred count: " << BI.Count << ")";
        }
        Sep = ", ";
      }
      OS << '\n';
    }

    if (!BB->lp_empty()) {
      OS << "  Landing Pads: ";
      auto Sep = "";
      for (auto LP : BB->landing_pads()) {
        OS << Sep << LP->getName();
        if (ExecutionCount != COUNT_NO_PROFILE) {
          OS << " (count: " << LP->getExecutionCount() << ")";
        }
        Sep = ", ";
      }
      OS << '\n';
    }

    // In CFG_Finalized state we can miscalculate CFI state at exit.
    if (CurrentState == State::CFG) {
      const auto CFIStateAtExit = BB->getCFIStateAtExit();
      if (CFIStateAtExit >= 0)
        OS << "  CFI State: " << CFIStateAtExit << '\n';
    }

    OS << '\n';
  }

  // Dump new exception ranges for the function.
  if (!CallSites.empty()) {
    OS << "EH table:\n";
    for (auto &CSI : CallSites) {
      OS << "  [" << *CSI.Start << ", " << *CSI.End << ") landing pad : ";
      if (CSI.LP)
        OS << *CSI.LP;
      else
        OS << "0";
      OS << ", action : " << CSI.Action << '\n';
    }
    OS << '\n';
  }

  // Print all jump tables.
  for (auto &JTI : JumpTables) {
    JTI.second->print(OS);
  }

  OS << "DWARF CFI Instructions:\n";
  if (OffsetToCFI.size()) {
    // Pre-buildCFG information
    for (auto &Elmt : OffsetToCFI) {
      OS << format("    %08x:\t", Elmt.first);
      assert(Elmt.second < FrameInstructions.size() && "Incorrect CFI offset");
      BinaryContext::printCFI(OS, FrameInstructions[Elmt.second]);
      OS << "\n";
    }
  } else {
    // Post-buildCFG information
    for (uint32_t I = 0, E = FrameInstructions.size(); I != E; ++I) {
      const MCCFIInstruction &CFI = FrameInstructions[I];
      OS << format("    %d:\t", I);
      BinaryContext::printCFI(OS, CFI);
      OS << "\n";
    }
  }
  if (FrameInstructions.empty())
    OS << "    <empty>\n";

  OS << "End of Function \"" << *this << "\"\n\n";
}

void BinaryFunction::printRelocations(raw_ostream &OS,
                                      uint64_t Offset,
                                      uint64_t Size) const {
  const char *Sep = " # Relocs: ";

  auto RI = Relocations.lower_bound(Offset);
  while (RI != Relocations.end() && RI->first < Offset + Size) {
    OS << Sep << "(R: " << RI->second << ")";
    Sep = ", ";
    ++RI;
  }

  RI = MoveRelocations.lower_bound(Offset);
  while (RI != MoveRelocations.end() && RI->first < Offset + Size) {
    OS << Sep << "(M: " << RI->second << ")";
    Sep = ", ";
    ++RI;
  }

  auto PI = PCRelativeRelocationOffsets.lower_bound(Offset);
  if (PI != PCRelativeRelocationOffsets.end() && *PI < Offset + Size) {
    OS << Sep << "(pcrel)";
  }
}

IndirectBranchType
BinaryFunction::processIndirectBranch(MCInst &Instruction,
                                      unsigned Size,
                                      uint64_t Offset,
                                      uint64_t &TargetAddress) {
  const auto PtrSize = BC.AsmInfo->getCodePointerSize();

  // An instruction referencing memory used by jump instruction (directly or
  // via register). This location could be an array of function pointers
  // in case of indirect tail call, or a jump table.
  MCInst *MemLocInstr;

  // Address of the table referenced by MemLocInstr. Could be either an
  // array of function pointers, or a jump table.
  uint64_t ArrayStart = 0;

  unsigned BaseRegNum, IndexRegNum;
  int64_t DispValue;
  const MCExpr *DispExpr;

  // In AArch, identify the instruction adding the PC-relative offset to
  // jump table entries to correctly decode it.
  MCInst *PCRelBaseInstr;
  uint64_t PCRelAddr = 0;

  auto Begin = Instructions.begin();
  auto End = Instructions.end();

  if (BC.isAArch64()) {
    PreserveNops = BC.HasRelocations;
    // Start at the last label as an approximation of the current basic block.
    // This is a heuristic, since the full set of labels have yet to be
    // determined
    for (auto LI = Labels.rbegin(); LI != Labels.rend(); ++LI) {
      auto II = Instructions.find(LI->first);
      if (II != Instructions.end()) {
        Begin = II;
        break;
      }
    }
  }

  auto Type = BC.MIB->analyzeIndirectBranch(Instruction,
                                            Begin,
                                            End,
                                            PtrSize,
                                            MemLocInstr,
                                            BaseRegNum,
                                            IndexRegNum,
                                            DispValue,
                                            DispExpr,
                                            PCRelBaseInstr);

  if (Type == IndirectBranchType::UNKNOWN && !MemLocInstr)
    return Type;

  if (MemLocInstr != &Instruction)
    IndexRegNum = 0;

  if (BC.isAArch64()) {
    const auto *Sym = BC.MIB->getTargetSymbol(*PCRelBaseInstr, 1);
    assert (Sym && "Symbol extraction failed");
    if (auto *BD = BC.getBinaryDataByName(Sym->getName())) {
      PCRelAddr = BD->getAddress();
    } else {
      for (auto &Elmt : Labels) {
        if (Elmt.second == Sym) {
          PCRelAddr = Elmt.first + getAddress();
          break;
        }
      }
    }
    uint64_t InstrAddr = 0;
    for (auto II = Instructions.rbegin(); II != Instructions.rend(); ++II) {
      if (&II->second == PCRelBaseInstr) {
        InstrAddr = II->first + getAddress();
        break;
      }
    }
    assert(InstrAddr != 0 && "instruction not found");
    // We do this to avoid spurious references to code locations outside this
    // function (for example, if the indirect jump lives in the last basic
    // block of the function, it will create a reference to the next function).
    // This replaces a symbol reference with an immediate.
    BC.MIB->replaceMemOperandDisp(*PCRelBaseInstr,
                                  MCOperand::createImm(PCRelAddr - InstrAddr));
    // FIXME: Disable full jump table processing for AArch64 until we have a
    // proper way of determining the jump table limits.
    return IndirectBranchType::UNKNOWN;
  }

  // RIP-relative addressing should be converted to symbol form by now
  // in processed instructions (but not in jump).
  if (DispExpr) {
    const MCSymbol *TargetSym;
    uint64_t TargetOffset;
    std::tie(TargetSym, TargetOffset) = BC.MIB->getTargetSymbolInfo(DispExpr);
    auto *BD = BC.getBinaryDataByName(TargetSym->getName());
    assert(BD && "global symbol needs a value");
    ArrayStart = BD->getAddress() + TargetOffset;
    BaseRegNum = 0;
    if (BC.isAArch64()) {
      ArrayStart &= ~0xFFFULL;
      ArrayStart += DispValue & 0xFFFULL;
    }
  } else {
    ArrayStart = static_cast<uint64_t>(DispValue);
  }

  if (BaseRegNum == BC.MRI->getProgramCounter())
    ArrayStart += getAddress() + Offset + Size;

  DEBUG(dbgs() << "BOLT-DEBUG: addressed memory is 0x"
               << Twine::utohexstr(ArrayStart) << '\n');

  // List of possible jump targets.
  std::vector<uint64_t> JTOffsetCandidates;

  auto useJumpTableForInstruction = [&](JumpTable::JumpTableType JTType) {
    JumpTable *JT;
    const MCSymbol *JTLabel;
    std::tie(JT, JTLabel) = BC.createJumpTable(*this,
                                               ArrayStart,
                                               JTType,
                                               std::move(JTOffsetCandidates));

    BC.MIB->replaceMemOperandDisp(const_cast<MCInst &>(*MemLocInstr),
                                  JTLabel, BC.Ctx.get());
    BC.MIB->setJumpTable(Instruction, ArrayStart, IndexRegNum);

    JTSites.emplace_back(Offset, ArrayStart);
  };

  // Check if there's already a jump table registered at this address.
  if (auto *JT = BC.getJumpTableContainingAddress(ArrayStart)) {
    const auto JTOffset = ArrayStart - JT->getAddress();
    if (Type == IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE && JTOffset != 0) {
        // Adjust the size of this jump table and create a new one if necessary.
        // We cannot re-use the entries since the offsets are relative to the
        // table start.
        DEBUG(dbgs() << "BOLT-DEBUG: adjusting size of jump table at 0x"
                     << Twine::utohexstr(JT->getAddress()) << '\n');
        JT->OffsetEntries.resize(JTOffset / JT->EntrySize);
    } else if (Type != IndirectBranchType::POSSIBLE_FIXED_BRANCH) {
      // Re-use the existing jump table or parts of it.
      if (Type != IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE) {
        assert(JT->Type == JumpTable::JTT_NORMAL &&
               "normal jump table expected");
        Type = IndirectBranchType::POSSIBLE_JUMP_TABLE;
      } else {
        assert(JT->Type == JumpTable::JTT_PIC && "PIC jump table expected");
      }

      useJumpTableForInstruction(JT->Type);

      return Type;
    }
  }

  auto Section = BC.getSectionForAddress(ArrayStart);
  if (!Section) {
    // No section - possibly an absolute address. Since we don't allow
    // internal function addresses to escape the function scope - we
    // consider it a tail call.
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: no section for address 0x"
             << Twine::utohexstr(ArrayStart) << " referenced from function "
             << *this << '\n';
    }
    return IndirectBranchType::POSSIBLE_TAIL_CALL;
  }
  if (Section->isVirtual()) {
    // The contents are filled at runtime.
    return IndirectBranchType::POSSIBLE_TAIL_CALL;
  }

  // Extract the value at the start of the array.
  StringRef SectionContents = Section->getContents();
  const auto EntrySize =
    Type == IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE ? 4 : PtrSize;
  DataExtractor DE(SectionContents, BC.AsmInfo->isLittleEndian(), EntrySize);
  auto ValueOffset = static_cast<uint32_t>(ArrayStart - Section->getAddress());
  uint64_t Value = 0;
  auto UpperBound = Section->getSize();
  const auto *JumpTableBD = BC.getBinaryDataAtAddress(ArrayStart);
  if (JumpTableBD && JumpTableBD->getSize()) {
    UpperBound = ValueOffset + JumpTableBD->getSize();
    assert(UpperBound <= Section->getSize() &&
           "data object cannot cross a section boundary");
  }

  while (ValueOffset <= UpperBound - EntrySize) {
    DEBUG(dbgs() << "BOLT-DEBUG: indirect jmp at 0x"
                 << Twine::utohexstr(getAddress() + Offset)
                 << " is referencing address 0x"
                 << Twine::utohexstr(Section->getAddress() + ValueOffset));
    // Extract the value and increment the offset.
    if (BC.isAArch64()) {
      Value = PCRelAddr + DE.getSigned(&ValueOffset, EntrySize);
    } else if (Type == IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE) {
      Value = ArrayStart + DE.getSigned(&ValueOffset, 4);
    } else {
      Value = DE.getAddress(&ValueOffset);
    }
    DEBUG(dbgs() << ", which contains value "
                 << Twine::utohexstr(Value) << '\n');
    if (Type == IndirectBranchType::POSSIBLE_FIXED_BRANCH) {
      if (Section->isReadOnly()) {
        outs() << "BOLT-INFO: fixed indirect branch detected in " << *this
               << " at 0x" << Twine::utohexstr(getAddress() + Offset)
               << " the destination value is 0x" << Twine::utohexstr(Value)
               << '\n';
        TargetAddress = Value;
        return Type;
      }
      return IndirectBranchType::UNKNOWN;
    }
    if (containsAddress(Value) && Value != getAddress()) {
      // Is it possible to have a jump table with function start as an entry?
      JTOffsetCandidates.push_back(Value - getAddress());
      if (Type == IndirectBranchType::UNKNOWN)
        Type = IndirectBranchType::POSSIBLE_JUMP_TABLE;
      continue;
    }
    // Potentially a switch table can contain __builtin_unreachable() entry
    // pointing just right after the function. In this case we have to check
    // another entry. Otherwise the entry is outside of this function scope
    // and it's not a switch table.
    if (Value == getAddress() + getSize()) {
      JTOffsetCandidates.push_back(getSize());
      IgnoredBranches.emplace_back(Offset, getSize());
    } else {
      break;
    }
  }
  if (Type == IndirectBranchType::POSSIBLE_JUMP_TABLE ||
      Type == IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE) {
    assert(JTOffsetCandidates.size() > 1 &&
           "expected more than one jump table entry");

    const auto JumpTableType = Type == IndirectBranchType::POSSIBLE_JUMP_TABLE
        ? JumpTable::JTT_NORMAL
        : JumpTable::JTT_PIC;
    useJumpTableForInstruction(JumpTableType);

    return Type;
  }

  // We have a possible tail call, so let's add the value read from the possible
  // memory location as a reference. Only do that if the address we read is sane
  // enough (is inside an allocatable section). It is possible that we read
  // garbage if the load instruction we analyzed is in a basic block different
  // than the one where the indirect jump is. However, later,
  // postProcessIndirectBranches() is going to mark the function as non-simple
  // in this case.
  if (Value && BC.getSectionForAddress(Value))
    BC.InterproceduralReferences.insert(std::make_pair(this, Value));

  return IndirectBranchType::POSSIBLE_TAIL_CALL;
}

MCSymbol *BinaryFunction::getOrCreateLocalLabel(uint64_t Address,
                                                bool CreatePastEnd) {
  // Check if there's already a registered label.
  auto Offset = Address - getAddress();

  if ((Offset == getSize()) && CreatePastEnd)
    return getFunctionEndLabel();

  // Check if there's a global symbol registered at given address.
  // If so - reuse it since we want to keep the symbol value updated.
  if (Offset != 0) {
    if (auto *BD = BC.getBinaryDataAtAddress(Address)) {
      Labels[Offset] = BD->getSymbol();
      return BD->getSymbol();
    }
  }

  auto LI = Labels.find(Offset);
  if (LI != Labels.end())
    return LI->second;

  // For AArch64, check if this address is part of a constant island.
  if (MCSymbol *IslandSym = getOrCreateIslandAccess(Address)) {
    return IslandSym;
  }

  MCSymbol *Result = BC.Ctx->createTempSymbol();
  Labels[Offset] = Result;
  return Result;
}

void BinaryFunction::disassemble(ArrayRef<uint8_t> FunctionData) {
  NamedRegionTimer T("disassemble", "Disassemble function", TimerGroupName,
                     TimerGroupDesc, opts::TimeBuild);

  assert(FunctionData.size() == getSize() &&
         "function size does not match raw data size");

  auto &Ctx = BC.Ctx;
  auto &MIB = BC.MIB;

  DWARFUnitLineTable ULT = getDWARFUnitLineTable();

  matchProfileMemData();

  // Insert a label at the beginning of the function. This will be our first
  // basic block.
  Labels[0] = Ctx->createTempSymbol("BB0", false);
  addEntryPointAtOffset(0);

  auto getOrCreateSymbolForAddress = [&](const MCInst &Instruction,
                                         uint64_t TargetAddress,
                                         uint64_t &SymbolAddend) {
    if (BC.isAArch64()) {
      // Check if this is an access to a constant island and create bookkeeping
      // to keep track of it and emit it later as part of this function
      if (MCSymbol *IslandSym = getOrCreateIslandAccess(TargetAddress)) {
        return IslandSym;
      } else {
        // Detect custom code written in assembly that refers to arbitrary
        // constant islands from other functions. Write this reference so we
        // can pull this constant island and emit it as part of this function
        // too.
        auto IslandIter =
            BC.AddressToConstantIslandMap.lower_bound(TargetAddress);
        if (IslandIter != BC.AddressToConstantIslandMap.end()) {
          if (MCSymbol *IslandSym =
                  IslandIter->second->getOrCreateProxyIslandAccess(
                      TargetAddress, this)) {
            /// Make this function depend on IslandIter->second because we have
            /// a reference to its constant island. When emitting this function,
            /// we will also emit IslandIter->second's constants. This only
            /// happens in custom AArch64 assembly code.
            IslandDependency.insert(IslandIter->second);
            ProxyIslandSymbols[IslandSym] = IslandIter->second;
            return IslandSym;
          }
        }
      }
    }

    // Note that the address does not necessarily have to reside inside
    // a section, it could be an absolute address too.
    auto Section = BC.getSectionForAddress(TargetAddress);
    if (Section && Section->isText()) {
      if (containsAddress(TargetAddress, /*UseMaxSize=*/ BC.isAArch64())) {
        if (TargetAddress != getAddress()) {
          // The address could potentially escape. Mark it as another entry
          // point into the function.
          DEBUG(dbgs() << "BOLT-DEBUG: potentially escaped address 0x"
                       << Twine::utohexstr(TargetAddress) << " in function "
                       << *this << '\n');
          return addEntryPointAtOffset(TargetAddress - getAddress());
        }
      } else {
        BC.InterproceduralReferences.insert(
            std::make_pair(this, TargetAddress));
      }
    }

    auto *BD = BC.getBinaryDataContainingAddress(TargetAddress);
    if (BD) {
      auto *TargetSymbol = BD->getSymbol();
      SymbolAddend = TargetAddress - BD->getAddress();
      return TargetSymbol;
    }
    // TODO: use DWARF info to get size/alignment here?
    auto *TargetSymbol =
        BC.getOrCreateGlobalSymbol(TargetAddress, "DATAat");
    DEBUG(if (opts::Verbosity >= 2) {
      auto SectionName = BD ? BD->getSectionName() : "<unknown>";
      dbgs() << "Created DATAat sym: " << TargetSymbol->getName()
             << " in section " << SectionName  << "\n";
    });
    return TargetSymbol;
  };

  auto handlePCRelOperand =
      [&](MCInst &Instruction, uint64_t Address, uint64_t Size) {
    uint64_t TargetAddress{0};
    uint64_t TargetOffset{0};
    MCSymbol *TargetSymbol{nullptr};
    if (!MIB->evaluateMemOperandTarget(Instruction, TargetAddress, Address,
                                       Size)) {
      errs() << "BOLT-ERROR: PC-relative operand can't be evaluated:\n";
      BC.InstPrinter->printInst(&Instruction, errs(), "", *BC.STI);
      errs() << '\n';
      Instruction.dump_pretty(errs(), BC.InstPrinter.get());
      errs() << '\n';
      return false;
    }
    if (TargetAddress == 0) {
      if (opts::Verbosity >= 1) {
        outs() << "BOLT-INFO: PC-relative operand is zero in function "
               << *this << ".\n";
      }
    }

    TargetSymbol =
        getOrCreateSymbolForAddress(Instruction, TargetAddress, TargetOffset);

    const MCExpr *Expr = MCSymbolRefExpr::create(TargetSymbol,
                                                 MCSymbolRefExpr::VK_None,
                                                 *BC.Ctx);
    if (TargetOffset) {
      auto *Offset = MCConstantExpr::create(TargetOffset, *BC.Ctx);
      Expr = MCBinaryExpr::createAdd(Expr, Offset, *BC.Ctx);
    }
    MIB->replaceMemOperandDisp(
        Instruction, MCOperand::createExpr(BC.MIB->getTargetExprFor(
                         Instruction,
                         Expr,
                         *BC.Ctx, 0)));
    return true;
  };

  // Used to fix the target of linker-generated AArch64 stubs with no relocation
  // info
  auto fixStubTarget = [&](MCInst &LoadLowBits, MCInst &LoadHiBits,
                           uint64_t Target) {
    uint64_t Addend{0};
    int64_t Val;
    MCSymbol *TargetSymbol;
    TargetSymbol = getOrCreateSymbolForAddress(LoadLowBits, Target, Addend);
    MIB->replaceImmWithSymbol(LoadHiBits, TargetSymbol, Addend, Ctx.get(),
                              Val, ELF::R_AARCH64_ADR_PREL_PG_HI21);
    MIB->replaceImmWithSymbol(LoadLowBits, TargetSymbol, Addend, Ctx.get(), Val,
                              ELF::R_AARCH64_ADD_ABS_LO12_NC);
  };

  uint64_t Size = 0;  // instruction size
  for (uint64_t Offset = 0; Offset < getSize(); Offset += Size) {
    MCInst Instruction;
    const uint64_t AbsoluteInstrAddr = getAddress() + Offset;

    // Check for data inside code and ignore it
    if (DataOffsets.find(Offset) != DataOffsets.end()) {
      auto Iter = CodeOffsets.upper_bound(Offset);
      if (Iter != CodeOffsets.end()) {
        Size = *Iter - Offset;
        continue;
      }
      break;
    }

    if (!BC.DisAsm->getInstruction(Instruction,
                                   Size,
                                   FunctionData.slice(Offset),
                                   AbsoluteInstrAddr,
                                   nulls(),
                                   nulls())) {
      // Functions with "soft" boundaries, e.g. coming from assembly source,
      // can have 0-byte padding at the end.
      bool IsZeroPadding = true;
      uint64_t EndOfCode = getSize();
      auto Iter = DataOffsets.upper_bound(Offset);
      if (Iter != DataOffsets.end())
        EndOfCode = *Iter;
      for (auto I = Offset; I < EndOfCode; ++I) {
        if (FunctionData[I] != 0) {
          IsZeroPadding = false;
          break;
        }
      }

      if (!IsZeroPadding) {
        // Ignore this function. Skip to the next one in non-relocs mode.
        errs() << "BOLT-WARNING: unable to disassemble instruction at offset 0x"
               << Twine::utohexstr(Offset) << " (address 0x"
               << Twine::utohexstr(AbsoluteInstrAddr) << ") in function "
               << *this << '\n';
        // Some AVX-512 instructions could not be disassembled at all.
        if (BC.HasRelocations && opts::TrapOnAVX512 &&
            BC.TheTriple->getArch() == llvm::Triple::x86_64) {
          setTrapOnEntry();
          BC.TrappedFunctions.push_back(this);
        } else {
          IsSimple = false;
        }
      }
      break;
    }

    // Cannot process functions with AVX-512 instructions.
    if (MIB->hasEVEXEncoding(Instruction)) {
      if (opts::Verbosity >= 1) {
        errs() << "BOLT-WARNING: function " << *this << " uses instruction"
               " encoded with EVEX (AVX-512) at offset 0x"
               << Twine::utohexstr(Offset) << ". Disassembly could be wrong."
               " Skipping further processing.\n";
      }

      if (BC.HasRelocations && opts::TrapOnAVX512) {
        setTrapOnEntry();
        BC.TrappedFunctions.push_back(this);
      } else {
        IsSimple = false;
      }
      break;
    }

    // Check if there's a relocation associated with this instruction.
    bool UsedReloc{false};
    for (auto Itr = Relocations.lower_bound(Offset);
         Itr != Relocations.upper_bound(Offset + Size);
         ++Itr) {
      const auto &Relocation = Itr->second;
      if (Relocation.Offset >= Offset + Size)
        continue;

      DEBUG(dbgs() << "BOLT-DEBUG: replacing immediate with relocation"
            " against " << Relocation.Symbol->getName()
            << "+" << Relocation.Addend
            << " in function " << *this
            << " for instruction at offset 0x"
            << Twine::utohexstr(Offset) << '\n');
      int64_t Value = Relocation.Value;
      const auto Result = BC.MIB->replaceImmWithSymbol(Instruction,
                                                       Relocation.Symbol,
                                                       Relocation.Addend,
                                                       Ctx.get(),
                                                       Value,
                                                       Relocation.Type);
      (void)Result;
      assert(Result && "cannot replace immediate with relocation");
      // For aarch, if we replaced an immediate with a symbol from a
      // relocation, we mark it so we do not try to further process a
      // pc-relative operand. All we need is the symbol.
      if (BC.isAArch64())
        UsedReloc = true;

      // Make sure we replaced the correct immediate (instruction
      // can have multiple immediate operands).
      if (BC.isX86()) {
        assert(truncateToSize(static_cast<uint64_t>(Value),
                              Relocation::getSizeForType(Relocation.Type)) ==
               truncateToSize(Relocation.Value,
                              Relocation::getSizeForType(Relocation.Type)) &&
             "immediate value mismatch in function");
      }
    }

    // Convert instruction to a shorter version that could be relaxed if
    // needed.
    MIB->shortenInstruction(Instruction);

    if (MIB->isBranch(Instruction) || MIB->isCall(Instruction)) {
      uint64_t TargetAddress = 0;
      if (MIB->evaluateBranch(Instruction, AbsoluteInstrAddr, Size,
                              TargetAddress)) {
        // Check if the target is within the same function. Otherwise it's
        // a call, possibly a tail call.
        //
        // If the target *is* the function address it could be either a branch
        // or a recursive call.
        bool IsCall = MIB->isCall(Instruction);
        const bool IsCondBranch = MIB->isConditionalBranch(Instruction);
        MCSymbol *TargetSymbol = nullptr;

        if (IsCall && containsAddress(TargetAddress)) {
          if (TargetAddress == getAddress()) {
            // Recursive call.
            TargetSymbol = getSymbol();
          } else {
            if (BC.isX86()) {
              // Dangerous old-style x86 PIC code. We may need to freeze this
              // function, so preserve the function as is for now.
              PreserveNops = true;
            } else {
              errs() << "BOLT-WARNING: internal call detected at 0x"
                     << Twine::utohexstr(AbsoluteInstrAddr) << " in function "
                     << *this << ". Skipping.\n";
              IsSimple = false;
            }
          }
        }

        if (!TargetSymbol) {
          // Create either local label or external symbol.
          if (containsAddress(TargetAddress)) {
            TargetSymbol = getOrCreateLocalLabel(TargetAddress);
          } else {
            if (TargetAddress == getAddress() + getSize() &&
                TargetAddress < getAddress() + getMaxSize()) {
              // Result of __builtin_unreachable().
              DEBUG(dbgs() << "BOLT-DEBUG: jump past end detected at 0x"
                           << Twine::utohexstr(AbsoluteInstrAddr)
                           << " in function " << *this
                           << " : replacing with nop.\n");
              BC.MIB->createNoop(Instruction);
              if (IsCondBranch) {
                // Register branch offset for profile validation.
                IgnoredBranches.emplace_back(Offset, Offset + Size);
              }
              goto add_instruction;
            }
            BC.InterproceduralReferences.insert(
                std::make_pair(this, TargetAddress));
            if (opts::Verbosity >= 2 && !IsCall && Size == 2 &&
                !BC.HasRelocations) {
              errs() << "BOLT-WARNING: relaxed tail call detected at 0x"
                     << Twine::utohexstr(AbsoluteInstrAddr) << " in function "
                     << *this << ". Code size will be increased.\n";
            }

            assert(!MIB->isTailCall(Instruction) &&
                   "synthetic tail call instruction found");

            // This is a call regardless of the opcode.
            // Assign proper opcode for tail calls, so that they could be
            // treated as calls.
            if (!IsCall) {
              if (!MIB->convertJmpToTailCall(Instruction, BC.Ctx.get())) {
                assert(IsCondBranch && "unknown tail call instruction");
                if (opts::Verbosity >= 2) {
                  errs() << "BOLT-WARNING: conditional tail call detected in "
                         << "function " << *this << " at 0x"
                         << Twine::utohexstr(AbsoluteInstrAddr) << ".\n";
                }
              }
              IsCall = true;
            }

            TargetSymbol =
                BC.getOrCreateGlobalSymbol(TargetAddress, "FUNCat");
            if (TargetAddress == 0) {
              // We actually see calls to address 0 in presence of weak
              // symbols originating from libraries. This code is never meant
              // to be executed.
              if (opts::Verbosity >= 2) {
                outs() << "BOLT-INFO: Function " << *this
                       << " has a call to address zero.\n";
              }
            }

            if (BC.HasRelocations) {
              // Check if we need to create relocation to move this function's
              // code without re-assembly.
              size_t RelSize = (Size < 5) ? 1 : 4;
              auto RelOffset = Offset + Size - RelSize;
              if (BC.isAArch64()) {
                RelSize = 0;
                RelOffset = Offset;
              }
              auto RI = MoveRelocations.find(RelOffset);
              if (RI == MoveRelocations.end()) {
                uint64_t RelType =
                    (RelSize == 1) ? ELF::R_X86_64_PC8 : ELF::R_X86_64_PC32;
                if (BC.isAArch64())
                  RelType = ELF::R_AARCH64_CALL26;
                DEBUG(dbgs() << "BOLT-DEBUG: creating relocation for static"
                             << " function call to " << TargetSymbol->getName()
                             << " at offset 0x" << Twine::utohexstr(RelOffset)
                             << " with size " << RelSize << " for function "
                             << *this << '\n');
                addRelocation(getAddress() + RelOffset, TargetSymbol, RelType,
                              -RelSize, 0);
              }
              auto OI = PCRelativeRelocationOffsets.find(RelOffset);
              if (OI != PCRelativeRelocationOffsets.end()) {
                PCRelativeRelocationOffsets.erase(OI);
              }
            }
          }
        }

        if (!IsCall) {
          // Add taken branch info.
          TakenBranches.emplace_back(Offset, TargetAddress - getAddress());
        }
        BC.MIB->replaceBranchTarget(Instruction, TargetSymbol, &*Ctx);

        // Mark CTC.
        if (IsCondBranch && IsCall) {
          MIB->setConditionalTailCall(Instruction, TargetAddress);
        }
      } else {
        // Could not evaluate branch. Should be an indirect call or an
        // indirect branch. Bail out on the latter case.
        if (MIB->isIndirectBranch(Instruction)) {
          uint64_t IndirectTarget{0};
          auto Result =
              processIndirectBranch(Instruction, Size, Offset, IndirectTarget);
          switch (Result) {
          default:
            llvm_unreachable("unexpected result");
          case IndirectBranchType::POSSIBLE_TAIL_CALL: {
            auto Result = MIB->convertJmpToTailCall(Instruction, BC.Ctx.get());
            (void)Result;
            assert(Result);
            break;
          }
          case IndirectBranchType::POSSIBLE_JUMP_TABLE:
          case IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE:
            if (opts::JumpTables == JTS_NONE)
              IsSimple = false;
            break;
          case IndirectBranchType::POSSIBLE_FIXED_BRANCH: {
            if (containsAddress(IndirectTarget)) {
              const auto *TargetSymbol = getOrCreateLocalLabel(IndirectTarget);
              Instruction.clear();
              MIB->createUncondBranch(Instruction, TargetSymbol, BC.Ctx.get());
              TakenBranches.emplace_back(Offset, IndirectTarget - getAddress());
              HasFixedIndirectBranch = true;
            } else {
              MIB->convertJmpToTailCall(Instruction, BC.Ctx.get());
              BC.InterproceduralReferences.insert(
                  std::make_pair(this, IndirectTarget));
            }
            break;
          }
          case IndirectBranchType::UNKNOWN:
            // Keep processing. We'll do more checks and fixes in
            // postProcessIndirectBranches().
            break;
          };
        }
        // Indirect call. We only need to fix it if the operand is RIP-relative
        if (IsSimple && MIB->hasPCRelOperand(Instruction)) {
          if (!handlePCRelOperand(Instruction, AbsoluteInstrAddr, Size)) {
            errs() << "BOLT-ERROR: cannot handle PC-relative operand at 0x"
                   << Twine::utohexstr(AbsoluteInstrAddr)
                   << ". Skipping function " << *this << ".\n";
            if (BC.HasRelocations)
              exit(1);
            IsSimple = false;
          }
        }
        // AArch64 indirect call - check for linker veneers, which lack
        // relocations and need manual adjustments
        MCInst *TargetHiBits, *TargetLowBits;
        uint64_t TargetAddress;
        if (BC.isAArch64() &&
            MIB->matchLinkerVeneer(Instructions.begin(), Instructions.end(),
                                   AbsoluteInstrAddr, Instruction, TargetHiBits,
                                   TargetLowBits, TargetAddress)) {
          MIB->addAnnotation(Instruction, "AArch64Veneer", true);

          uint8_t Counter = 0;
          for (auto It = std::prev(Instructions.end()); Counter != 2;
               --It, ++Counter) {
            MIB->addAnnotation(It->second, "AArch64Veneer", true);
          }

          fixStubTarget(*TargetLowBits, *TargetHiBits, TargetAddress);
        }
      }
    } else {
      if (MIB->hasPCRelOperand(Instruction) && !UsedReloc) {
        if (!handlePCRelOperand(Instruction, AbsoluteInstrAddr, Size)) {
          errs() << "BOLT-ERROR: cannot handle PC-relative operand at 0x"
                 << Twine::utohexstr(AbsoluteInstrAddr)
                 << ". Skipping function " << *this << ".\n";
          if (BC.HasRelocations)
            exit(1);
          IsSimple = false;
        }
      }
    }

add_instruction:
    if (ULT.first && ULT.second) {
      Instruction.setLoc(
          findDebugLineInformationForInstructionAt(AbsoluteInstrAddr, ULT));
    }

    // Record offset of the instruction for profile matching.
    if (BC.keepOffsetForInstruction(Instruction)) {
      MIB->addAnnotation(Instruction, "Offset", Offset);
    }

    if (MemData && !emptyRange(MemData->getMemInfoRange(Offset))) {
      MIB->addAnnotation(Instruction, "MemDataOffset", Offset);
    }

    addInstruction(Offset, std::move(Instruction));
  }

  updateState(State::Disassembled);

  postProcessEntryPoints();

  postProcessJumpTables();
}

void BinaryFunction::postProcessEntryPoints() {
  if (!isSimple())
    return;

  for (auto Offset : EntryOffsets) {
    if (!getInstructionAtOffset(Offset)) {
      // On AArch64 there are legitimate reasons to have references past the
      // end of the function, e.g. jump tables.
      if (BC.isAArch64() && Offset == getSize()) {
        continue;
      }

      errs() << "BOLT-WARNING: reference in the middle of instruction "
                "detected in function " << *this
             << " at offset 0x" << Twine::utohexstr(Offset) << '\n';
      if (BC.HasRelocations) {
        errs() << "BOLT-ERROR: unable to keep processing in relocation mode\n";
        exit(1);
      }
      setSimple(false);
    }
  }
}

void BinaryFunction::postProcessJumpTables() {
  // Create labels for all entries.
  for (auto &JTI : JumpTables) {
    auto &JT = *JTI.second;
    if (JT.Type == JumpTable::JTT_PIC && opts::JumpTables == JTS_BASIC) {
      opts::JumpTables = JTS_MOVE;
      outs() << "BOLT-INFO: forcing -jump-tables=move as PIC jump table was "
                "detected in function " << *this << '\n';
    }
    for (unsigned I = 0; I < JT.OffsetEntries.size(); ++I) {
      auto Offset = JT.OffsetEntries[I];
      if (Offset != getSize() && !getInstructionAtOffset(Offset)) {
        DEBUG(dbgs() << "BOLT-DEBUG: truncating jump table " << JT.getName()
                     << " at index " << I << " containing offset 0x"
                     << Twine::utohexstr(Offset) << '\n');
        assert(I > 1 && "jump table with a size smaller than 1 detected");
        assert(JT.Type == JumpTable::JTT_PIC &&
               "unexpected truncation of non-PIC jump table");
        JT.OffsetEntries.resize(I);
        break;
      }
      auto *Label = getOrCreateLocalLabel(getAddress() + Offset,
                                          /*CreatePastEnd*/ true);
      JT.Entries.push_back(Label);
    }
    BC.setBinaryDataSize(JT.getAddress(), JT.getSize());
  }

  // Add TakenBranches from JumpTables.
  //
  // We want to do it after initial processing since we don't know jump tables'
  // boundaries until we process them all.
  for (auto &JTSite : JTSites) {
    const auto JTSiteOffset = JTSite.first;
    const auto JTAddress = JTSite.second;
    const auto *JT = getJumpTableContainingAddress(JTAddress);
    assert(JT && "cannot find jump table for address");
    auto EntryOffset = JTAddress - JT->getAddress();
    while (EntryOffset < JT->getSize()) {
      auto TargetOffset = JT->OffsetEntries[EntryOffset / JT->EntrySize];
      if (TargetOffset < getSize())
        TakenBranches.emplace_back(JTSiteOffset, TargetOffset);

      // Take ownership of jump table relocations.
      if (BC.HasRelocations) {
        auto EntryAddress = JT->getAddress() + EntryOffset;
        auto Res = BC.removeRelocationAt(EntryAddress);
        (void)Res;
        DEBUG(
          auto Section = BC.getSectionForAddress(EntryAddress);
          auto Offset = EntryAddress - Section->getAddress();
          dbgs() << "BOLT-DEBUG: removing relocation from section "
                 << Section->getName() << " at offset 0x"
                 << Twine::utohexstr(Offset) << " = "
                 << Res << '\n');
      }

      EntryOffset += JT->EntrySize;

      // A label at the next entry means the end of this jump table.
      if (JT->Labels.count(EntryOffset))
        break;
    }
  }
  clearList(JTSites);

  // Free memory used by jump table offsets.
  for (auto &JTI : JumpTables) {
    auto &JT = *JTI.second;
    clearList(JT.OffsetEntries);
  }

  // Remove duplicates branches. We can get a bunch of them from jump tables.
  // Without doing jump table value profiling we don't have use for extra
  // (duplicate) branches.
  std::sort(TakenBranches.begin(), TakenBranches.end());
  auto NewEnd = std::unique(TakenBranches.begin(), TakenBranches.end());
  TakenBranches.erase(NewEnd, TakenBranches.end());
}

bool BinaryFunction::postProcessIndirectBranches() {
  for (auto *BB : layout()) {
    for (auto &Instr : *BB) {
      if (!BC.MIB->isIndirectBranch(Instr))
        continue;

      // If there's an indirect branch in a single-block function -
      // it must be a tail call.
      if (layout_size() == 1) {
        BC.MIB->convertJmpToTailCall(Instr, BC.Ctx.get());
        return true;
      }

      // Validate the tail call or jump table assumptions now that we know
      // basic block boundaries.
      if (BC.MIB->isTailCall(Instr) || BC.MIB->getJumpTable(Instr)) {
        const auto PtrSize = BC.AsmInfo->getCodePointerSize();
        MCInst *MemLocInstr;
        unsigned BaseRegNum, IndexRegNum;
        int64_t DispValue;
        const MCExpr *DispExpr;
        MCInst *PCRelBaseInstr;
        auto Type = BC.MIB->analyzeIndirectBranch(Instr,
                                                  BB->begin(),
                                                  BB->end(),
                                                  PtrSize,
                                                  MemLocInstr,
                                                  BaseRegNum,
                                                  IndexRegNum,
                                                  DispValue,
                                                  DispExpr,
                                                  PCRelBaseInstr);
        if (Type == IndirectBranchType::UNKNOWN && MemLocInstr == nullptr)
          return false;

        continue;
      }

      // If this block contains an epilogue code and has an indirect branch,
      // then most likely it's a tail call. Otherwise, we cannot tell for sure
      // what it is and conservatively reject the function's CFG.
      bool IsEpilogue = false;
      for (const auto &Instr : *BB) {
        if (BC.MIB->isLeave(Instr) || BC.MIB->isPop(Instr)) {
          IsEpilogue = true;
          break;
        }
      }
      if (!IsEpilogue) {
        if (opts::Verbosity >= 2) {
          outs() << "BOLT-INFO: rejected potential indirect tail call in "
                 << "function " << *this << " in basic block "
                 << BB->getName() << ".\n";
          DEBUG(BC.printInstructions(dbgs(), BB->begin(), BB->end(),
                                     BB->getOffset(), this, true));
        }
        return false;
      }
      BC.MIB->convertJmpToTailCall(Instr, BC.Ctx.get());
    }
  }

  if (HasFixedIndirectBranch)
    return false;

  return true;
}

void BinaryFunction::recomputeLandingPads() {
  updateBBIndices(0);

  for (auto *BB : BasicBlocks) {
    BB->LandingPads.clear();
    BB->Throwers.clear();
  }

  for (auto *BB : BasicBlocks) {
    std::unordered_set<const BinaryBasicBlock *> BBLandingPads;
    for (auto &Instr : *BB) {
      if (!BC.MIB->isInvoke(Instr))
        continue;

      const auto EHInfo = BC.MIB->getEHInfo(Instr);
      if (!EHInfo || !EHInfo->first)
        continue;

      auto *LPBlock = getBasicBlockForLabel(EHInfo->first);
      if (!BBLandingPads.count(LPBlock)) {
        BBLandingPads.insert(LPBlock);
        BB->LandingPads.emplace_back(LPBlock);
        LPBlock->Throwers.emplace_back(BB);
      }
    }
  }
}

bool BinaryFunction::buildCFG() {
  NamedRegionTimer T("buildcfg", "Build CFG", TimerGroupName, TimerGroupDesc,
                     opts::TimeBuild);
  auto &MIB = BC.MIB;

  if (!isSimple()) {
    assert(!BC.HasRelocations &&
           "cannot process file with non-simple function in relocs mode");
    return false;
  }

  if (CurrentState != State::Disassembled)
    return false;

  assert(BasicBlocks.empty() && "basic block list should be empty");
  assert((Labels.find(0) != Labels.end()) &&
         "first instruction should always have a label");

  // Create basic blocks in the original layout order:
  //
  //  * Every instruction with associated label marks
  //    the beginning of a basic block.
  //  * Conditional instruction marks the end of a basic block,
  //    except when the following instruction is an
  //    unconditional branch, and the unconditional branch is not
  //    a destination of another branch. In the latter case, the
  //    basic block will consist of a single unconditional branch
  //    (missed "double-jump" optimization).
  //
  // Created basic blocks are sorted in layout order since they are
  // created in the same order as instructions, and instructions are
  // sorted by offsets.
  BinaryBasicBlock *InsertBB{nullptr};
  BinaryBasicBlock *PrevBB{nullptr};
  bool IsLastInstrNop{false};
  uint64_t LastInstrOffset{0};

  auto addCFIPlaceholders =
      [this](uint64_t CFIOffset, BinaryBasicBlock *InsertBB) {
        for (auto FI = OffsetToCFI.lower_bound(CFIOffset),
                  FE = OffsetToCFI.upper_bound(CFIOffset);
             FI != FE; ++FI) {
          addCFIPseudo(InsertBB, InsertBB->end(), FI->second);
        }
      };

  // For profiling purposes we need to save the offset of the last instruction
  // in the basic block. But in certain cases we don't if the instruction was
  // the last one, and we have to go back and update its offset.
  auto updateOffset = [&](uint64_t Offset) {
    assert(PrevBB && PrevBB != InsertBB && "invalid previous block");
    auto *PrevInstr = PrevBB->getLastNonPseudoInstr();
    if (PrevInstr && !MIB->hasAnnotation(*PrevInstr, "Offset"))
      MIB->addAnnotation(*PrevInstr, "Offset", Offset);
  };

  for (auto I = Instructions.begin(), E = Instructions.end(); I != E; ++I) {
    const auto Offset = I->first;
    auto &Instr = I->second;

    auto LI = Labels.find(Offset);
    if (LI != Labels.end()) {
      // Always create new BB at branch destination.
      PrevBB = InsertBB;
      InsertBB = addBasicBlock(LI->first, LI->second,
                               opts::PreserveBlocksAlignment && IsLastInstrNop);
      if (hasEntryPointAtOffset(Offset))
        InsertBB->setEntryPoint();
      if (PrevBB)
        updateOffset(LastInstrOffset);
    }

    bool IsSDTMarker =
        MIB->isNoop(Instr) && BC.SDTMarkers.count(I->first + Address);

    if (IsSDTMarker)
      HasSDTMarker = true;

    // Ignore nops except SDT markers. We use nops to derive alignment of the
    // next basic block. It will not always work, as some blocks are naturally
    // aligned, but it's just part of heuristic for block alignment.
    if (MIB->isNoop(Instr) && !PreserveNops && !IsSDTMarker) {
      IsLastInstrNop = true;
      continue;
    }
    if (!InsertBB) {
      // It must be a fallthrough or unreachable code. Create a new block unless
      // we see an unconditional branch following a conditional one. The latter
      // should not be a conditional tail call.
      assert(PrevBB && "no previous basic block for a fall through");
      auto *PrevInstr = PrevBB->getLastNonPseudoInstr();
      assert(PrevInstr && "no previous instruction for a fall through");
      if (MIB->isUnconditionalBranch(Instr) &&
          !MIB->isUnconditionalBranch(*PrevInstr) &&
          !MIB->getConditionalTailCall(*PrevInstr)) {
        // Temporarily restore inserter basic block.
        InsertBB = PrevBB;
      } else {
        InsertBB = addBasicBlock(Offset,
                                 BC.Ctx->createTempSymbol("FT", true),
                                 opts::PreserveBlocksAlignment &&
                                   IsLastInstrNop);
        updateOffset(LastInstrOffset);
      }
    }
    if (Offset == 0) {
      // Add associated CFI pseudos in the first offset (0)
      addCFIPlaceholders(0, InsertBB);
    }

    const auto IsBlockEnd = MIB->isTerminator(Instr);
    IsLastInstrNop = MIB->isNoop(Instr);
    LastInstrOffset = Offset;
    InsertBB->addInstruction(std::move(Instr));

    // Add associated CFI instrs. We always add the CFI instruction that is
    // located immediately after this instruction, since the next CFI
    // instruction reflects the change in state caused by this instruction.
    auto NextInstr = std::next(I);
    uint64_t CFIOffset;
    if (NextInstr != E)
      CFIOffset = NextInstr->first;
    else
      CFIOffset = getSize();

    // Note: this potentially invalidates instruction pointers/iterators.
    addCFIPlaceholders(CFIOffset, InsertBB);

    if (IsBlockEnd) {
      PrevBB = InsertBB;
      InsertBB = nullptr;
    }
  }

  if (BasicBlocks.empty()) {
    setSimple(false);
    return false;
  }

  // Intermediate dump.
  DEBUG(print(dbgs(), "after creating basic blocks"));

  // TODO: handle properly calls to no-return functions,
  // e.g. exit(3), etc. Otherwise we'll see a false fall-through
  // blocks.

  for (auto &Branch : TakenBranches) {
    DEBUG(dbgs() << "registering branch [0x" << Twine::utohexstr(Branch.first)
                 << "] -> [0x" << Twine::utohexstr(Branch.second) << "]\n");
    auto *FromBB = getBasicBlockContainingOffset(Branch.first);
    auto *ToBB = getBasicBlockAtOffset(Branch.second);
    if (!FromBB || !ToBB) {
      if (!FromBB)
        errs() << "BOLT-ERROR: cannot find BB containing the branch.\n";
      if (!ToBB)
        errs() << "BOLT-ERROR: cannot find BB containing branch destination.\n";
      BC.exitWithBugReport("disassembly failed - inconsistent branch found.",
                           *this);
    }

    FromBB->addSuccessor(ToBB);
  }

  // Add fall-through branches.
  PrevBB = nullptr;
  bool IsPrevFT = false; // Is previous block a fall-through.
  for (auto BB : BasicBlocks) {
    if (IsPrevFT) {
      PrevBB->addSuccessor(BB);
    }
    if (BB->empty()) {
      IsPrevFT = true;
      PrevBB = BB;
      continue;
    }

    auto LastInstr = BB->getLastNonPseudoInstr();
    assert(LastInstr &&
           "should have non-pseudo instruction in non-empty block");

    if (BB->succ_size() == 0) {
      // Since there's no existing successors, we know the last instruction is
      // not a conditional branch. Thus if it's a terminator, it shouldn't be a
      // fall-through.
      //
      // Conditional tail call is a special case since we don't add a taken
      // branch successor for it.
      IsPrevFT = !MIB->isTerminator(*LastInstr) ||
                 MIB->getConditionalTailCall(*LastInstr);
    } else if (BB->succ_size() == 1) {
      IsPrevFT = MIB->isConditionalBranch(*LastInstr);
    } else {
      IsPrevFT = false;
    }

    PrevBB = BB;
  }

  if (!IsPrevFT) {
    // Possibly a call that does not return.
    DEBUG(dbgs() << "last block was marked as a fall-through in " << *this
                 << '\n');
  }

  // Assign landing pads and throwers info.
  recomputeLandingPads();

  // Assign CFI information to each BB entry.
  annotateCFIState();

  // Annotate invoke instructions with GNU_args_size data.
  propagateGnuArgsSizeInfo();

  // Set the basic block layout to the original order and set end offsets.
  PrevBB = nullptr;
  for (auto BB : BasicBlocks) {
    BasicBlocksLayout.emplace_back(BB);
    if (PrevBB)
      PrevBB->setEndOffset(BB->getOffset());
    PrevBB = BB;
  }
  PrevBB->setEndOffset(getSize());

  updateLayoutIndices();

  normalizeCFIState();

  // Clean-up memory taken by intermediate structures.
  //
  // NB: don't clear Labels list as we may need them if we mark the function
  //     as non-simple later in the process of discovering extra entry points.
  clearList(Instructions);
  clearList(OffsetToCFI);
  clearList(TakenBranches);

  // Update the state.
  CurrentState = State::CFG;

  // Make any necessary adjustments for indirect branches.
  if (!postProcessIndirectBranches()) {
    if (opts::Verbosity) {
      errs() << "BOLT-WARNING: failed to post-process indirect branches for "
             << *this << '\n';
    }
    // In relocation mode we want to keep processing the function but avoid
    // optimizing it.
    setSimple(false);
  }

  return true;
}

void BinaryFunction::postProcessCFG() {
  if (isSimple() && !BasicBlocks.empty()) {
    // Convert conditional tail call branches to conditional branches that jump
    // to a tail call.
    removeConditionalTailCalls();

    postProcessProfile();

    // Eliminate inconsistencies between branch instructions and CFG.
    postProcessBranches();
  }

  calculateMacroOpFusionStats();

  // The final cleanup of intermediate structures.
  clearList(IgnoredBranches);
  clearList(EntryOffsets);

  // Remove "Offset" annotations.
  for (auto *BB : layout())
    for (auto &Inst : *BB)
      BC.MIB->removeAnnotation(Inst, "Offset");

  assert((!isSimple() || validateCFG()) &&
         "invalid CFG detected after post-processing");
}

void BinaryFunction::calculateMacroOpFusionStats() {
  if (!getBinaryContext().isX86())
    return;
  for (auto *BB : layout()) {
    auto II = BB->getMacroOpFusionPair();
    if (II == BB->end())
      continue;

    // Check offset of the second instruction.
    // FIXME: arch-specific.
    const auto Offset =
      BC.MIB->getAnnotationWithDefault<uint64_t>(*std::next(II), "Offset", 0);
    if (!Offset || (getAddress() + Offset) % 64)
      continue;

    DEBUG(dbgs() << "\nmissed macro-op fusion at address 0x"
                 << Twine::utohexstr(getAddress() + Offset) << " in function "
                 << *this << "; executed " << BB->getKnownExecutionCount()
                 << " times.\n");
    ++BC.MissedMacroFusionPairs;
    BC.MissedMacroFusionExecCount += BB->getKnownExecutionCount();
  }
}

void BinaryFunction::removeTagsFromProfile() {
  for (auto *BB : BasicBlocks) {
    if (BB->ExecutionCount == BinaryBasicBlock::COUNT_NO_PROFILE)
      BB->ExecutionCount = 0;
    for (auto &BI : BB->branch_info()) {
      if (BI.Count != BinaryBasicBlock::COUNT_NO_PROFILE &&
          BI.MispredictedCount != BinaryBasicBlock::COUNT_NO_PROFILE)
        continue;
      BI.Count = 0;
      BI.MispredictedCount = 0;
    }
  }
}

void BinaryFunction::updateReferences(const MCSymbol *From, const MCSymbol *To) {
  assert(CurrentState == State::Empty || CurrentState == State::Disassembled);
  assert(From && To && "invalid symbols");

  for (auto I = Instructions.begin(), E = Instructions.end(); I != E; ++I) {
    auto &Inst = I->second;
    for (int I = 0, E = MCPlus::getNumPrimeOperands(Inst); I != E; ++I) {
      const MCSymbol *S = BC.MIB->getTargetSymbol(Inst, I);
      if (S == From)
        BC.MIB->setOperandToSymbolRef(Inst, I, To, 0, &*BC.Ctx, 0);
    }
  }
}

void BinaryFunction::addEntryPoint(uint64_t Address) {
  assert(containsAddress(Address) && "address does not belong to the function");

  auto Offset = Address - getAddress();

  DEBUG(dbgs() << "BOLT-INFO: adding external entry point to function " << *this
               << " at offset 0x" << Twine::utohexstr(Address - getAddress())
               << '\n');

  auto *EntryBD = BC.getBinaryDataAtAddress(Address);
  auto *EntrySymbol = EntryBD ? EntryBD->getSymbol() : nullptr;

  // If we haven't built CFG for the function, we can add a new entry point
  // even if it doesn't have an associated entry in the symbol table.
  if (CurrentState == State::Empty || CurrentState == State::Disassembled) {
    auto Iter = Labels.find(Offset);
    const MCSymbol *OldSym = Iter != Labels.end() ? Iter->second : nullptr;
    if (!EntrySymbol) {
      DEBUG(dbgs() << "creating local label\n");
      EntrySymbol = getOrCreateLocalLabel(Address);
    } else {
      DEBUG(dbgs() << "using global symbol " << EntrySymbol->getName() << '\n');
    }
    addEntryPointAtOffset(Address - getAddress());
    Labels.emplace(Offset, EntrySymbol);
    if (OldSym != nullptr && EntrySymbol != OldSym) {
      updateReferences(OldSym, EntrySymbol);
    }
    return;
  }

  assert(EntrySymbol && "expected symbol at address");

  if (isSimple()) {
    // Find basic block corresponding to the address and substitute label.
    auto *BB = getBasicBlockAtOffset(Offset);
    if (!BB) {
      // TODO #14762450: split basic block and process function.
      if (opts::Verbosity || BC.HasRelocations) {
        errs() << "BOLT-WARNING: no basic block at offset 0x"
               << Twine::utohexstr(Offset) << " in function " << *this
               << ". Marking non-simple.\n";
      }
      setSimple(false);
    } else {
      BB->setLabel(EntrySymbol);
      BB->setEntryPoint(true);
    }
  }

  // Fix/append labels list.
  auto LI = Labels.find(Offset);
  if (LI != Labels.end()) {
    LI->second = EntrySymbol;
  } else {
    Labels.emplace(Offset, EntrySymbol);
  }
}

void BinaryFunction::removeConditionalTailCalls() {
  // Blocks to be appended at the end.
  std::vector<std::unique_ptr<BinaryBasicBlock>> NewBlocks;

  for (auto BBI = begin(); BBI != end(); ++BBI) {
    auto &BB = *BBI;
    auto *CTCInstr = BB.getLastNonPseudoInstr();
    if (!CTCInstr)
      continue;

    auto TargetAddressOrNone = BC.MIB->getConditionalTailCall(*CTCInstr);
    if (!TargetAddressOrNone)
      continue;

    // Gather all necessary information about CTC instruction before
    // annotations are destroyed.
    const auto CFIStateBeforeCTC = BB.getCFIStateAtInstr(CTCInstr);
    uint64_t CTCTakenCount = BinaryBasicBlock::COUNT_NO_PROFILE;
    uint64_t CTCMispredCount = BinaryBasicBlock::COUNT_NO_PROFILE;
    if (hasValidProfile()) {
      CTCTakenCount =
        BC.MIB->getAnnotationWithDefault<uint64_t>(*CTCInstr, "CTCTakenCount");
      CTCMispredCount =
        BC.MIB->getAnnotationWithDefault<uint64_t>(*CTCInstr,
                                                   "CTCMispredCount");
    }

    // Assert that the tail call does not throw.
    assert(!BC.MIB->getEHInfo(*CTCInstr) &&
           "found tail call with associated landing pad");

    // Create a basic block with an unconditional tail call instruction using
    // the same destination.
    const auto *CTCTargetLabel = BC.MIB->getTargetSymbol(*CTCInstr);
    assert(CTCTargetLabel && "symbol expected for conditional tail call");
    MCInst TailCallInstr;
    BC.MIB->createTailCall(TailCallInstr, CTCTargetLabel, BC.Ctx.get());
    auto TailCallBB = createBasicBlock(BinaryBasicBlock::INVALID_OFFSET,
                                       BC.Ctx->createTempSymbol("TC", true));
    TailCallBB->addInstruction(TailCallInstr);
    TailCallBB->setCFIState(CFIStateBeforeCTC);

    // Add CFG edge with profile info from BB to TailCallBB.
    BB.addSuccessor(TailCallBB.get(), CTCTakenCount, CTCMispredCount);

    // Add execution count for the block.
    TailCallBB->setExecutionCount(CTCTakenCount);

    BC.MIB->convertTailCallToJmp(*CTCInstr);

    BC.MIB->replaceBranchTarget(*CTCInstr, TailCallBB->getLabel(),
                                BC.Ctx.get());

    // Add basic block to the list that will be added to the end.
    NewBlocks.emplace_back(std::move(TailCallBB));

    // Swap edges as the TailCallBB corresponds to the taken branch.
    BB.swapConditionalSuccessors();

    // This branch is no longer a conditional tail call.
    BC.MIB->unsetConditionalTailCall(*CTCInstr);
  }

  insertBasicBlocks(std::prev(end()),
                    std::move(NewBlocks),
                    /* UpdateLayout */ true,
                    /* UpdateCFIState */ false);
}

uint64_t BinaryFunction::getFunctionScore() const {
  if (FunctionScore != -1)
    return FunctionScore;

  if (!isSimple() || !hasValidProfile()) {
    FunctionScore = 0;
    return FunctionScore;
  }

  uint64_t TotalScore = 0ULL;
  for (auto BB : layout()) {
    uint64_t BBExecCount = BB->getExecutionCount();
    if (BBExecCount == BinaryBasicBlock::COUNT_NO_PROFILE)
      continue;
    TotalScore += BBExecCount;
  }
  FunctionScore = TotalScore;
  return FunctionScore;
}

void BinaryFunction::annotateCFIState() {
  assert(CurrentState == State::Disassembled && "unexpected function state");
  assert(!BasicBlocks.empty() && "basic block list should not be empty");

  // This is an index of the last processed CFI in FDE CFI program.
  uint32_t State = 0;

  // This is an index of RememberState CFI reflecting effective state right
  // after execution of RestoreState CFI.
  //
  // It differs from State iff the CFI at (State-1)
  // was RestoreState (modulo GNU_args_size CFIs, which are ignored).
  //
  // This allows us to generate shorter replay sequences when producing new
  // CFI programs.
  uint32_t EffectiveState = 0;

  // For tracking RememberState/RestoreState sequences.
  std::stack<uint32_t> StateStack;

  for (auto *BB : BasicBlocks) {
    BB->setCFIState(EffectiveState);

    for (const auto &Instr : *BB) {
      const auto *CFI = getCFIFor(Instr);
      if (!CFI)
        continue;

      ++State;

      switch (CFI->getOperation()) {
      case MCCFIInstruction::OpRememberState:
        StateStack.push(EffectiveState);
        EffectiveState = State;
        break;
      case MCCFIInstruction::OpRestoreState:
        assert(!StateStack.empty() && "corrupt CFI stack");
        EffectiveState = StateStack.top();
        StateStack.pop();
        break;
      case MCCFIInstruction::OpGnuArgsSize:
        // OpGnuArgsSize CFIs do not affect the CFI state.
        break;
      default:
        // Any other CFI updates the state.
        EffectiveState = State;
        break;
      }
    }
  }

  assert(StateStack.empty() && "corrupt CFI stack");
}

namespace {

/// Our full interpretation of a DWARF CFI machine state at a given point
struct CFISnapshot {
  /// CFA register number and offset defining the canonical frame at this
  /// point, or the number of a rule (CFI state) that computes it with a
  /// DWARF expression. This number will be negative if it refers to a CFI
  /// located in the CIE instead of the FDE.
  uint32_t CFAReg;
  int32_t CFAOffset;
  int32_t CFARule;
  /// Mapping of rules (CFI states) that define the location of each
  /// register. If absent, no rule defining the location of such register
  /// was ever read. This number will be negative if it refers to a CFI
  /// located in the CIE instead of the FDE.
  DenseMap<int32_t, int32_t> RegRule;

  /// References to CIE, FDE and expanded instructions after a restore state
  const std::vector<MCCFIInstruction> &CIE;
  const std::vector<MCCFIInstruction> &FDE;
  const DenseMap<int32_t, SmallVector<int32_t, 4>> &FrameRestoreEquivalents;

  /// Current FDE CFI number representing the state where the snapshot is at
  int32_t CurState;

  /// Used when we don't have information about which state/rule to apply
  /// to recover the location of either the CFA or a specific register
  constexpr static int32_t UNKNOWN = std::numeric_limits<int32_t>::min();

private:
  /// Update our snapshot by executing a single CFI
  void update(const MCCFIInstruction &Instr, int32_t RuleNumber) {
    switch (Instr.getOperation()) {
    case MCCFIInstruction::OpSameValue:
    case MCCFIInstruction::OpRelOffset:
    case MCCFIInstruction::OpOffset:
    case MCCFIInstruction::OpRestore:
    case MCCFIInstruction::OpUndefined:
    case MCCFIInstruction::OpRegister:
    case MCCFIInstruction::OpExpression:
    case MCCFIInstruction::OpValExpression:
      RegRule[Instr.getRegister()] = RuleNumber;
      break;
    case MCCFIInstruction::OpDefCfaRegister:
      CFAReg = Instr.getRegister();
      CFARule = UNKNOWN;
      break;
    case MCCFIInstruction::OpDefCfaOffset:
      CFAOffset = Instr.getOffset();
      CFARule = UNKNOWN;
      break;
    case MCCFIInstruction::OpDefCfa:
      CFAReg = Instr.getRegister();
      CFAOffset = Instr.getOffset();
      CFARule = UNKNOWN;
      break;
    case MCCFIInstruction::OpDefCfaExpression:
      CFARule = RuleNumber;
      break;
    case MCCFIInstruction::OpAdjustCfaOffset:
    case MCCFIInstruction::OpWindowSave:
    case MCCFIInstruction::OpEscape:
      llvm_unreachable("unsupported CFI opcode");
      break;
    case MCCFIInstruction::OpRememberState:
    case MCCFIInstruction::OpRestoreState:
    case MCCFIInstruction::OpGnuArgsSize:
      // do not affect CFI state
      break;
    }
  }

public:
  /// Advance state reading FDE CFI instructions up to State number
  void advanceTo(int32_t State) {
    for (int32_t I = CurState, E = State; I != E; ++I) {
      const auto &Instr = FDE[I];
      if (Instr.getOperation() != MCCFIInstruction::OpRestoreState) {
        update(Instr, I);
        continue;
      }
      // If restore state instruction, fetch the equivalent CFIs that have
      // the same effect of this restore. This is used to ensure remember-
      // restore pairs are completely removed.
      auto Iter = FrameRestoreEquivalents.find(I);
      if (Iter == FrameRestoreEquivalents.end())
        continue;
      for (int32_t RuleNumber : Iter->second) {
        update(FDE[RuleNumber], RuleNumber);
      }
    }

    assert(((CFAReg != (uint32_t)UNKNOWN && CFAOffset != UNKNOWN) ||
            CFARule != UNKNOWN) &&
           "CIE did not define default CFA?");

    CurState = State;
  }

  /// Interpret all CIE and FDE instructions up until CFI State number and
  /// populate this snapshot
  CFISnapshot(
      const std::vector<MCCFIInstruction> &CIE,
      const std::vector<MCCFIInstruction> &FDE,
      const DenseMap<int32_t, SmallVector<int32_t, 4>> &FrameRestoreEquivalents,
      int32_t State)
      : CIE(CIE), FDE(FDE), FrameRestoreEquivalents(FrameRestoreEquivalents) {
    CFAReg = UNKNOWN;
    CFAOffset = UNKNOWN;
    CFARule = UNKNOWN;
    CurState = 0;

    for (int32_t I = 0, E = CIE.size(); I != E; ++I) {
      const auto &Instr = CIE[I];
      update(Instr, -I);
    }

    advanceTo(State);
  }

};

/// A CFI snapshot with the capability of checking if incremental additions to
/// it are redundant. This is used to ensure we do not emit two CFI instructions
/// back-to-back that are doing the same state change, or to avoid emitting a
/// CFI at all when the state at that point would not be modified after that CFI
struct CFISnapshotDiff : public CFISnapshot {
  bool RestoredCFAReg{false};
  bool RestoredCFAOffset{false};
  DenseMap<int32_t, bool> RestoredRegs;

  CFISnapshotDiff(const CFISnapshot &S) : CFISnapshot(S) {}

  CFISnapshotDiff(
      const std::vector<MCCFIInstruction> &CIE,
      const std::vector<MCCFIInstruction> &FDE,
      const DenseMap<int32_t, SmallVector<int32_t, 4>> &FrameRestoreEquivalents,
      int32_t State)
      : CFISnapshot(CIE, FDE, FrameRestoreEquivalents, State) {}

  /// Return true if applying Instr to this state is redundant and can be
  /// dismissed.
  bool isRedundant(const MCCFIInstruction &Instr) {
    switch (Instr.getOperation()) {
    case MCCFIInstruction::OpSameValue:
    case MCCFIInstruction::OpRelOffset:
    case MCCFIInstruction::OpOffset:
    case MCCFIInstruction::OpRestore:
    case MCCFIInstruction::OpUndefined:
    case MCCFIInstruction::OpRegister:
    case MCCFIInstruction::OpExpression:
    case MCCFIInstruction::OpValExpression: {
      if (RestoredRegs[Instr.getRegister()])
        return true;
      RestoredRegs[Instr.getRegister()] = true;
      const int32_t CurRegRule =
          RegRule.find(Instr.getRegister()) != RegRule.end()
              ? RegRule[Instr.getRegister()]
              : UNKNOWN;
      if (CurRegRule == UNKNOWN) {
        if (Instr.getOperation() == MCCFIInstruction::OpRestore ||
            Instr.getOperation() == MCCFIInstruction::OpSameValue)
          return true;
        return false;
      }
      const MCCFIInstruction &LastDef =
          CurRegRule < 0 ? CIE[-CurRegRule] : FDE[CurRegRule];
      return LastDef == Instr;
    }
    case MCCFIInstruction::OpDefCfaRegister:
      if (RestoredCFAReg)
        return true;
      RestoredCFAReg = true;
      return CFAReg == Instr.getRegister();
    case MCCFIInstruction::OpDefCfaOffset:
      if (RestoredCFAOffset)
        return true;
      RestoredCFAOffset = true;
      return CFAOffset == Instr.getOffset();
    case MCCFIInstruction::OpDefCfa:
      if (RestoredCFAReg && RestoredCFAOffset)
        return true;
      RestoredCFAReg = true;
      RestoredCFAOffset = true;
      return CFAReg == Instr.getRegister() && CFAOffset == Instr.getOffset();
    case MCCFIInstruction::OpDefCfaExpression:
      if (RestoredCFAReg && RestoredCFAOffset)
        return true;
      RestoredCFAReg = true;
      RestoredCFAOffset = true;
      return false;
    case MCCFIInstruction::OpAdjustCfaOffset:
    case MCCFIInstruction::OpWindowSave:
    case MCCFIInstruction::OpEscape:
      llvm_unreachable("unsupported CFI opcode");
      return false;
    case MCCFIInstruction::OpRememberState:
    case MCCFIInstruction::OpRestoreState:
    case MCCFIInstruction::OpGnuArgsSize:
      // do not affect CFI state
      return true;
    }
    return false;
  }
};

} // end anonymous namespace

bool BinaryFunction::replayCFIInstrs(int32_t FromState, int32_t ToState,
                                     BinaryBasicBlock *InBB,
                                     BinaryBasicBlock::iterator InsertIt) {
  if (FromState == ToState)
    return true;
  assert(FromState < ToState && "can only replay CFIs forward");

  CFISnapshotDiff CFIDiff(CIEFrameInstructions, FrameInstructions,
                          FrameRestoreEquivalents, FromState);

  std::vector<uint32_t> NewCFIs;
  for (auto CurState = FromState; CurState < ToState; ++CurState) {
    MCCFIInstruction *Instr = &FrameInstructions[CurState];
    if (Instr->getOperation() == MCCFIInstruction::OpRestoreState) {
      auto Iter = FrameRestoreEquivalents.find(CurState);
      assert(Iter != FrameRestoreEquivalents.end());
      NewCFIs.insert(NewCFIs.end(), Iter->second.begin(),
                     Iter->second.end());
      // RestoreState / Remember will be filtered out later by CFISnapshotDiff,
      // so we might as well fall-through here.
    }
    NewCFIs.push_back(CurState);
    continue;
  }

  // Replay instructions while avoiding duplicates
  for (auto I = NewCFIs.rbegin(), E = NewCFIs.rend(); I != E; ++I) {
    if (CFIDiff.isRedundant(FrameInstructions[*I]))
      continue;
    InsertIt = addCFIPseudo(InBB, InsertIt, *I);
  }

  return true;
}

SmallVector<int32_t, 4>
BinaryFunction::unwindCFIState(int32_t FromState, int32_t ToState,
                               BinaryBasicBlock *InBB,
                               BinaryBasicBlock::iterator &InsertIt) {
  SmallVector<int32_t, 4> NewStates;

  CFISnapshot ToCFITable(CIEFrameInstructions, FrameInstructions,
                                FrameRestoreEquivalents, ToState);
  CFISnapshotDiff FromCFITable(ToCFITable);
  FromCFITable.advanceTo(FromState);

  auto undoState = [&](const MCCFIInstruction &Instr) {
    switch (Instr.getOperation()) {
    case MCCFIInstruction::OpRememberState:
    case MCCFIInstruction::OpRestoreState:
      break;
    case MCCFIInstruction::OpSameValue:
    case MCCFIInstruction::OpRelOffset:
    case MCCFIInstruction::OpOffset:
    case MCCFIInstruction::OpRestore:
    case MCCFIInstruction::OpUndefined:
    case MCCFIInstruction::OpRegister:
    case MCCFIInstruction::OpExpression:
    case MCCFIInstruction::OpValExpression: {
      if (ToCFITable.RegRule.find(Instr.getRegister()) ==
          ToCFITable.RegRule.end()) {
        FrameInstructions.emplace_back(
            MCCFIInstruction::createRestore(nullptr, Instr.getRegister()));
        if (FromCFITable.isRedundant(FrameInstructions.back())) {
          FrameInstructions.pop_back();
          break;
        }
        NewStates.push_back(FrameInstructions.size() - 1);
        InsertIt = addCFIPseudo(InBB, InsertIt, FrameInstructions.size() - 1);
        ++InsertIt;
        break;
      }
      const int32_t Rule = ToCFITable.RegRule[Instr.getRegister()];
      if (Rule < 0) {
        if (FromCFITable.isRedundant(CIEFrameInstructions[-Rule]))
          break;
        NewStates.push_back(FrameInstructions.size());
        InsertIt = addCFIPseudo(InBB, InsertIt, FrameInstructions.size());
        ++InsertIt;
        FrameInstructions.emplace_back(CIEFrameInstructions[-Rule]);
        break;
      }
      if (FromCFITable.isRedundant(FrameInstructions[Rule]))
        break;
      NewStates.push_back(Rule);
      InsertIt = addCFIPseudo(InBB, InsertIt, Rule);
      ++InsertIt;
      break;
    }
    case MCCFIInstruction::OpDefCfaRegister:
    case MCCFIInstruction::OpDefCfaOffset:
    case MCCFIInstruction::OpDefCfa:
    case MCCFIInstruction::OpDefCfaExpression:
      if (ToCFITable.CFARule == CFISnapshot::UNKNOWN) {
        FrameInstructions.emplace_back(MCCFIInstruction::createDefCfa(
            nullptr, ToCFITable.CFAReg, -ToCFITable.CFAOffset));
        if (FromCFITable.isRedundant(FrameInstructions.back())) {
          FrameInstructions.pop_back();
          break;
        }
        NewStates.push_back(FrameInstructions.size() - 1);
        InsertIt = addCFIPseudo(InBB, InsertIt, FrameInstructions.size() - 1);
        ++InsertIt;
      } else if (ToCFITable.CFARule < 0) {
        if (FromCFITable.isRedundant(CIEFrameInstructions[-ToCFITable.CFARule]))
          break;
        NewStates.push_back(FrameInstructions.size());
        InsertIt = addCFIPseudo(InBB, InsertIt, FrameInstructions.size());
        ++InsertIt;
        FrameInstructions.emplace_back(
            CIEFrameInstructions[-ToCFITable.CFARule]);
      } else if (!FromCFITable.isRedundant(
                     FrameInstructions[ToCFITable.CFARule])) {
        NewStates.push_back(ToCFITable.CFARule);
        InsertIt = addCFIPseudo(InBB, InsertIt, ToCFITable.CFARule);
        ++InsertIt;
      }
      break;
    case MCCFIInstruction::OpAdjustCfaOffset:
    case MCCFIInstruction::OpWindowSave:
    case MCCFIInstruction::OpEscape:
      llvm_unreachable("unsupported CFI opcode");
      break;
    case MCCFIInstruction::OpGnuArgsSize:
      // do not affect CFI state
      break;
    }
  };


  // Undo all modifications from ToState to FromState
  for (int32_t I = ToState, E = FromState; I != E; ++I) {
    const auto &Instr = FrameInstructions[I];
    if (Instr.getOperation() != MCCFIInstruction::OpRestoreState) {
      undoState(Instr);
      continue;
    }
    auto Iter = FrameRestoreEquivalents.find(I);
    if (Iter == FrameRestoreEquivalents.end())
      continue;
    for (int32_t State : Iter->second)
      undoState(FrameInstructions[State]);
  }

  return NewStates;
}

void BinaryFunction::normalizeCFIState() {
  // Reordering blocks with remember-restore state instructions can be specially
  // tricky. When rewriting the CFI, we omit remember-restore state instructions
  // entirely. For restore state, we build a map expanding each restore to the
  // equivalent unwindCFIState sequence required at that point to achieve the
  // same effect of the restore. All remember state are then just ignored.
  std::stack<int32_t> Stack;
  for (BinaryBasicBlock *CurBB : BasicBlocksLayout) {
    for (auto II = CurBB->begin(); II != CurBB->end(); ++II) {
      if (auto *CFI = getCFIFor(*II)) {
        if (CFI->getOperation() == MCCFIInstruction::OpRememberState) {
          Stack.push(II->getOperand(0).getImm());
          continue;
        }
        if (CFI->getOperation() == MCCFIInstruction::OpRestoreState) {
          const int32_t RememberState = Stack.top();
          const int32_t CurState = II->getOperand(0).getImm();
          FrameRestoreEquivalents[CurState] =
              unwindCFIState(CurState, RememberState, CurBB, II);
          Stack.pop();
        }
      }
    }
  }
}

bool BinaryFunction::finalizeCFIState() {
  DEBUG(dbgs() << "Trying to fix CFI states for each BB after reordering.\n");
  DEBUG(dbgs() << "This is the list of CFI states for each BB of " << *this
               << ": ");

  int32_t State = 0;
  bool SeenCold = false;
  auto Sep = "";
  (void)Sep;
  for (auto *BB : BasicBlocksLayout) {
    const auto CFIStateAtExit = BB->getCFIStateAtExit();

    // Hot-cold border: check if this is the first BB to be allocated in a cold
    // region (with a different FDE). If yes, we need to reset the CFI state.
    if (!SeenCold && BB->isCold()) {
      State = 0;
      SeenCold = true;
    }

    // We need to recover the correct state if it doesn't match expected
    // state at BB entry point.
    if (BB->getCFIState() < State) {
      // In this case, State is currently higher than what this BB expect it
      // to be. To solve this, we need to insert CFI instructions to undo
      // the effect of all CFI from BB's state to current State.
      auto InsertIt = BB->begin();
      unwindCFIState(State, BB->getCFIState(), BB, InsertIt);
    } else if (BB->getCFIState() > State) {
      // If BB's CFI state is greater than State, it means we are behind in the
      // state. Just emit all instructions to reach this state at the
      // beginning of this BB. If this sequence of instructions involve
      // remember state or restore state, bail out.
      if (!replayCFIInstrs(State, BB->getCFIState(), BB, BB->begin()))
        return false;
    }

    State = CFIStateAtExit;
    DEBUG(dbgs() << Sep << State; Sep = ", ");
  }
  DEBUG(dbgs() << "\n");

  for (auto BB : BasicBlocksLayout) {
    for (auto II = BB->begin(); II != BB->end(); ) {
      auto CFI = getCFIFor(*II);
      if (CFI &&
          (CFI->getOperation() == MCCFIInstruction::OpRememberState ||
           CFI->getOperation() == MCCFIInstruction::OpRestoreState)) {
        II = BB->eraseInstruction(II);
      } else {
        ++II;
      }
    }
  }

  return true;
}

uint64_t BinaryFunction::getInstructionCount() const {
  uint64_t Count = 0;
  for (auto &Block : BasicBlocksLayout) {
    Count += Block->getNumNonPseudos();
  }
  return Count;
}

bool BinaryFunction::hasLayoutChanged() const {
  return ModifiedLayout;
}

uint64_t BinaryFunction::getEditDistance() const {
  return ComputeEditDistance<BinaryBasicBlock *>(BasicBlocksPreviousLayout,
                                                 BasicBlocksLayout);
}

void BinaryFunction::emitBody(MCStreamer &Streamer, bool EmitColdPart,
                              bool EmitCodeOnly) {
  if (!EmitCodeOnly && EmitColdPart && hasConstantIsland())
    duplicateConstantIslands();

  // Track first emitted instruction with debug info.
  bool FirstInstr = true;
  for (auto BB : layout()) {
    if (EmitColdPart != BB->isCold())
      continue;

    if ((opts::AlignBlocks || opts::PreserveBlocksAlignment)
        && BB->getAlignment() > 1) {
      Streamer.EmitCodeAlignment(BB->getAlignment(),
                                 BB->getAlignmentMaxBytes());
    }
    Streamer.EmitLabel(BB->getLabel());

    // Check if special alignment for macro-fusion is needed.
    bool MayNeedMacroFusionAlignment =
      (opts::AlignMacroOpFusion == MFT_ALL) ||
      (opts::AlignMacroOpFusion == MFT_HOT &&
       BB->getKnownExecutionCount());
    BinaryBasicBlock::const_iterator MacroFusionPair;
    if (MayNeedMacroFusionAlignment) {
      MacroFusionPair = BB->getMacroOpFusionPair();
      if (MacroFusionPair == BB->end())
        MayNeedMacroFusionAlignment = false;
    }

    SMLoc LastLocSeen;
    // Remember if the last instruction emitted was a prefix.
    bool LastIsPrefix = false;
    for (auto I = BB->begin(), E = BB->end(); I != E; ++I) {
      auto &Instr = *I;

      if (EmitCodeOnly && BC.MII->get(Instr.getOpcode()).isPseudo())
        continue;

      // Handle pseudo instructions.
      if (BC.MIB->isEHLabel(Instr)) {
        const auto *Label = BC.MIB->getTargetSymbol(Instr);
        assert(Instr.getNumOperands() == 1 && Label &&
               "bad EH_LABEL instruction");
        Streamer.EmitLabel(const_cast<MCSymbol *>(Label));
        continue;
      }
      if (BC.MIB->isCFI(Instr)) {
        Streamer.EmitCFIInstruction(*getCFIFor(Instr));
        continue;
      }

      // Handle macro-fusion alignment. If we emitted a prefix as
      // the last instruction, we should've already emitted the associated
      // alignment hint, so don't emit it twice.
      if (MayNeedMacroFusionAlignment && !LastIsPrefix && I == MacroFusionPair){
        // This assumes the second instruction in the macro-op pair will get
        // assigned to its own MCRelaxableFragment. Since all JCC instructions
        // are relaxable, we should be safe.
        Streamer.EmitNeverAlignCodeAtEnd(/*Alignment to avoid=*/64);
      }

      if (!EmitCodeOnly && opts::UpdateDebugSections && UnitLineTable.first) {
        LastLocSeen = emitLineInfo(Instr.getLoc(), LastLocSeen, FirstInstr);
        FirstInstr = false;
      }

      Streamer.EmitInstruction(Instr, *BC.STI);
      LastIsPrefix = BC.MIB->isPrefix(Instr);
    }
  }

  if (!EmitCodeOnly)
    emitConstantIslands(Streamer, EmitColdPart);
}

void BinaryFunction::emitBodyRaw(MCStreamer *Streamer) {

  // #14998851: Fix gold linker's '--emit-relocs'.
  assert(false &&
         "cannot emit raw body unless relocation accuracy is guaranteed");

  assert(!isInjected() && "cannot emit raw body of injected function");

  // Raw contents of the function.
  StringRef SectionContents = InputSection->getContents();

  // Raw contents of the function.
  StringRef FunctionContents =
      SectionContents.substr(getAddress() - InputSection->getAddress(),
      getSize());

  if (opts::Verbosity)
    outs() << "BOLT-INFO: emitting function " << *this << " in raw ("
           << getSize() << " bytes).\n";

  // We split the function blob into smaller blocks and output relocations
  // and/or labels between them.
  uint64_t FunctionOffset = 0;
  auto LI = Labels.begin();
  auto RI = MoveRelocations.begin();
  while (LI != Labels.end() ||
         RI != MoveRelocations.end()) {
    uint64_t NextLabelOffset = (LI == Labels.end() ? getSize() : LI->first);
    uint64_t NextRelocationOffset =
      (RI == MoveRelocations.end() ? getSize() : RI->first);
    auto NextStop = std::min(NextLabelOffset, NextRelocationOffset);
    assert(NextStop <= getSize() && "internal overflow error");
    if (FunctionOffset < NextStop) {
      Streamer->EmitBytes(
          FunctionContents.slice(FunctionOffset, NextStop));
      FunctionOffset = NextStop;
    }
    if (LI != Labels.end() && FunctionOffset == LI->first) {
      Streamer->EmitLabel(LI->second);
      DEBUG(dbgs() << "BOLT-DEBUG: emitted label " << LI->second->getName()
                   << " at offset 0x" << Twine::utohexstr(LI->first) << '\n');
      ++LI;
    }
    if (RI != MoveRelocations.end() && FunctionOffset == RI->first) {
      auto RelocationSize = RI->second.emit(Streamer);
      DEBUG(dbgs() << "BOLT-DEBUG: emitted relocation for symbol "
                   << RI->second.Symbol->getName() << " at offset 0x"
                   << Twine::utohexstr(RI->first)
                   << " with size " << RelocationSize << '\n');
      FunctionOffset += RelocationSize;
      ++RI;
    }
  }
  assert(FunctionOffset <= getSize() && "overflow error");
  if (FunctionOffset < getSize()) {
    Streamer->EmitBytes(FunctionContents.substr(FunctionOffset));
  }
}

void BinaryFunction::setTrapOnEntry() {
  clearList(Instructions);
  clearList(IgnoredBranches);
  clearList(TakenBranches);

  for (const auto EntryOffset : EntryOffsets) {
    MCInst TrapInstr;
    BC.MIB->createTrap(TrapInstr);
    addInstruction(EntryOffset, std::move(TrapInstr));
  }

  TrapsOnEntry = true;
}

void BinaryFunction::emitConstantIslands(
    MCStreamer &Streamer, bool EmitColdPart,
    BinaryFunction *OnBehalfOf) {
  if (DataOffsets.empty() && IslandDependency.empty())
    return;

  if (!OnBehalfOf) {
    if (!EmitColdPart)
      Streamer.EmitLabel(getFunctionConstantIslandLabel());
    else
      Streamer.EmitLabel(getFunctionColdConstantIslandLabel());
  }

  assert((!OnBehalfOf || IslandProxies[OnBehalfOf].size() > 0) &&
         "spurious OnBehalfOf constant island emission");

  assert(!isInjected() &&
         "injected functions should not have constant islands");
  // Raw contents of the function.
  StringRef SectionContents = InputSection->getContents();

  // Raw contents of the function.
  StringRef FunctionContents =
      SectionContents.substr(getAddress() - InputSection->getAddress(),
                             getMaxSize());

  if (opts::Verbosity && !OnBehalfOf)
    outs() << "BOLT-INFO: emitting constant island for function " << *this
           << "\n";

  // We split the island into smaller blocks and output labels between them.
  auto IS = IslandOffsets.begin();
  for (auto DataIter = DataOffsets.begin(); DataIter != DataOffsets.end();
       ++DataIter) {
    uint64_t FunctionOffset = *DataIter;
    uint64_t EndOffset = 0ULL;

    // Determine size of this data chunk
    auto NextData = std::next(DataIter);
    auto CodeIter = CodeOffsets.lower_bound(*DataIter);
    if (CodeIter == CodeOffsets.end() && NextData == DataOffsets.end()) {
      EndOffset = getMaxSize();
    } else if (CodeIter == CodeOffsets.end()) {
      EndOffset = *NextData;
    } else if (NextData == DataOffsets.end()) {
      EndOffset = *CodeIter;
    } else {
      EndOffset = (*CodeIter > *NextData) ? *NextData : *CodeIter;
    }

    if (FunctionOffset == EndOffset)
      continue;    // Size is zero, nothing to emit

    // Emit labels, relocs and data
    auto RI = MoveRelocations.lower_bound(FunctionOffset);
    while ((IS != IslandOffsets.end() && IS->first < EndOffset) ||
           (RI != MoveRelocations.end() && RI->first < EndOffset)) {
      auto NextLabelOffset = IS == IslandOffsets.end() ? EndOffset : IS->first;
      auto NextRelOffset = RI == MoveRelocations.end() ? EndOffset : RI->first;
      auto NextStop = std::min(NextLabelOffset, NextRelOffset);
      assert(NextStop <= EndOffset && "internal overflow error");
      if (FunctionOffset < NextStop) {
        Streamer.EmitBytes(FunctionContents.slice(FunctionOffset, NextStop));
        FunctionOffset = NextStop;
      }
      if (IS != IslandOffsets.end() && FunctionOffset == IS->first) {
        // This is a slightly complex code to decide which label to emit. We
        // have 4 cases to handle: regular symbol, cold symbol, regular or cold
        // symbol being emitted on behalf of an external function.
        if (!OnBehalfOf) {
          if (!EmitColdPart) {
            DEBUG(dbgs() << "BOLT-DEBUG: emitted label "
                         << IS->second->getName() << " at offset 0x"
                         << Twine::utohexstr(IS->first) << '\n');
            if (IS->second->isUndefined())
              Streamer.EmitLabel(IS->second);
            else
              assert(hasName(IS->second->getName()));
          } else if (ColdIslandSymbols.count(IS->second) != 0) {
            DEBUG(dbgs() << "BOLT-DEBUG: emitted label "
                         << ColdIslandSymbols[IS->second]->getName() << '\n');
            if (ColdIslandSymbols[IS->second]->isUndefined())
              Streamer.EmitLabel(ColdIslandSymbols[IS->second]);
          }
        } else {
          if (!EmitColdPart) {
            if (MCSymbol *Sym = IslandProxies[OnBehalfOf][IS->second]) {
              DEBUG(dbgs() << "BOLT-DEBUG: emitted label " << Sym->getName()
                           << '\n');
              Streamer.EmitLabel(Sym);
            }
          } else if (MCSymbol *Sym =
                         ColdIslandProxies[OnBehalfOf][IS->second]) {
            DEBUG(dbgs() << "BOLT-DEBUG: emitted label " << Sym->getName()
                         << '\n');
            Streamer.EmitLabel(Sym);
          }
        }
        ++IS;
      }
      if (RI != MoveRelocations.end() && FunctionOffset == RI->first) {
        auto RelocationSize = RI->second.emit(&Streamer);
        DEBUG(dbgs() << "BOLT-DEBUG: emitted relocation for symbol "
                     << RI->second.Symbol->getName() << " at offset 0x"
                     << Twine::utohexstr(RI->first)
                     << " with size " << RelocationSize << '\n');
        FunctionOffset += RelocationSize;
        ++RI;
      }
    }
    assert(FunctionOffset <= EndOffset && "overflow error");
    if (FunctionOffset < EndOffset) {
      Streamer.EmitBytes(FunctionContents.slice(FunctionOffset, EndOffset));
    }
  }
  assert(IS == IslandOffsets.end() && "some symbols were not emitted!");

  if (OnBehalfOf)
    return;
  // Now emit constant islands from other functions that we may have used in
  // this function.
  for (auto *ExternalFunc : IslandDependency) {
    ExternalFunc->emitConstantIslands(Streamer, EmitColdPart, this);
  }

}

void BinaryFunction::duplicateConstantIslands() {
  for (auto BB : layout()) {
    if (!BB->isCold())
      continue;

    for (auto &Inst : *BB) {
      int OpNum = 0;
      for (auto &Operand : Inst) {
        if (!Operand.isExpr()) {
          ++OpNum;
          continue;
        }
        auto *Symbol = BC.MIB->getTargetSymbol(Inst, OpNum);
        // Check if this is an island symbol
        if (!IslandSymbols.count(Symbol) && !ProxyIslandSymbols.count(Symbol))
          continue;

        // Create cold symbol, if missing
        auto ISym = ColdIslandSymbols.find(Symbol);
        MCSymbol *ColdSymbol;
        if (ISym != ColdIslandSymbols.end()) {
          ColdSymbol = ISym->second;
        } else {
          ColdSymbol = BC.Ctx->getOrCreateSymbol(Symbol->getName() + ".cold");
          ColdIslandSymbols[Symbol] = ColdSymbol;
          // Check if this is a proxy island symbol and update owner proxy map
          if (ProxyIslandSymbols.count(Symbol)) {
            BinaryFunction *Owner = ProxyIslandSymbols[Symbol];
            auto IProxiedSym = Owner->IslandProxies[this].find(Symbol);
            Owner->ColdIslandProxies[this][IProxiedSym->second] = ColdSymbol;
          }
        }

        // Update instruction reference
        Operand = MCOperand::createExpr(BC.MIB->getTargetExprFor(
            Inst,
            MCSymbolRefExpr::create(ColdSymbol, MCSymbolRefExpr::VK_None,
                                    *BC.Ctx),
            *BC.Ctx, 0));
        ++OpNum;
      }
    }
  }
}

namespace {

#ifndef MAX_PATH
#define MAX_PATH 255
#endif

std::string constructFilename(std::string Filename,
                              std::string Annotation,
                              std::string Suffix) {
  std::replace(Filename.begin(), Filename.end(), '/', '-');
  if (!Annotation.empty()) {
    Annotation.insert(0, "-");
  }
  if (Filename.size() + Annotation.size() + Suffix.size() > MAX_PATH) {
    assert(Suffix.size() + Annotation.size() <= MAX_PATH);
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: Filename \"" << Filename << Annotation << Suffix
             << "\" exceeds the " << MAX_PATH << " size limit, truncating.\n";
    }
    Filename.resize(MAX_PATH - (Suffix.size() + Annotation.size()));
  }
  Filename += Annotation;
  Filename += Suffix;
  return Filename;
}

std::string formatEscapes(const std::string& Str) {
  std::string Result;
  for (unsigned I = 0; I < Str.size(); ++I) {
    auto C = Str[I];
    switch (C) {
    case '\n':
      Result += "&#13;";
      break;
    case '"':
      break;
    default:
      Result += C;
      break;
    }
  }
  return Result;
}

}

void BinaryFunction::dumpGraph(raw_ostream& OS) const {
  OS << "strict digraph \"" << getPrintName() << "\" {\n";
  uint64_t Offset = Address;
  for (auto *BB : BasicBlocks) {
    auto LayoutPos = std::find(BasicBlocksLayout.begin(),
                               BasicBlocksLayout.end(),
                               BB);
    unsigned Layout = LayoutPos - BasicBlocksLayout.begin();
    const char* ColdStr = BB->isCold() ? " (cold)" : "";
    OS << format("\"%s\" [label=\"%s%s\\n(C:%lu,O:%lu,I:%u,L:%u:CFI:%u)\"]\n",
                 BB->getName().data(),
                 BB->getName().data(),
                 ColdStr,
                 (BB->ExecutionCount != BinaryBasicBlock::COUNT_NO_PROFILE
                  ? BB->ExecutionCount
                  : 0),
                 BB->getOffset(),
                 getIndex(BB),
                 Layout,
                 BB->getCFIState());
    OS << format("\"%s\" [shape=box]\n", BB->getName().data());
    if (opts::DotToolTipCode) {
      std::string Str;
      raw_string_ostream CS(Str);
      Offset = BC.printInstructions(CS, BB->begin(), BB->end(), Offset, this);
      const auto Code = formatEscapes(CS.str());
      OS << format("\"%s\" [tooltip=\"%s\"]\n",
                   BB->getName().data(),
                   Code.c_str());
    }

    // analyzeBranch is just used to get the names of the branch
    // opcodes.
    const MCSymbol *TBB = nullptr;
    const MCSymbol *FBB = nullptr;
    MCInst *CondBranch = nullptr;
    MCInst *UncondBranch = nullptr;
    const bool Success = BB->analyzeBranch(TBB,
                                           FBB,
                                           CondBranch,
                                           UncondBranch);

    const auto *LastInstr = BB->getLastNonPseudoInstr();
    const bool IsJumpTable = LastInstr && BC.MIB->getJumpTable(*LastInstr);

    auto BI = BB->branch_info_begin();
    for (auto *Succ : BB->successors()) {
      std::string Branch;
      if (Success) {
        if (Succ == BB->getConditionalSuccessor(true)) {
          Branch = CondBranch
            ? BC.InstPrinter->getOpcodeName(CondBranch->getOpcode())
            : "TB";
        } else if (Succ == BB->getConditionalSuccessor(false)) {
          Branch = UncondBranch
            ? BC.InstPrinter->getOpcodeName(UncondBranch->getOpcode())
            : "FB";
        } else {
          Branch = "FT";
        }
      }
      if (IsJumpTable) {
        Branch = "JT";
      }
      OS << format("\"%s\" -> \"%s\" [label=\"%s",
                   BB->getName().data(),
                   Succ->getName().data(),
                   Branch.c_str());

      if (BB->getExecutionCount() != COUNT_NO_PROFILE &&
          BI->MispredictedCount != BinaryBasicBlock::COUNT_INFERRED) {
        OS << "\\n(C:" << BI->Count << ",M:" << BI->MispredictedCount << ")";
      } else if (ExecutionCount != COUNT_NO_PROFILE &&
                 BI->Count != BinaryBasicBlock::COUNT_NO_PROFILE) {
        OS << "\\n(IC:" << BI->Count << ")";
      }
      OS << "\"]\n";

      ++BI;
    }
    for (auto *LP : BB->landing_pads()) {
      OS << format("\"%s\" -> \"%s\" [constraint=false style=dashed]\n",
                   BB->getName().data(),
                   LP->getName().data());
    }
  }
  OS << "}\n";
}

void BinaryFunction::viewGraph() const {
  SmallString<MAX_PATH> Filename;
  if (auto EC = sys::fs::createTemporaryFile("bolt-cfg", "dot", Filename)) {
    errs() << "BOLT-ERROR: " << EC.message() << ", unable to create "
           << " bolt-cfg-XXXXX.dot temporary file.\n";
    return;
  }
  dumpGraphToFile(Filename.str());
  if (DisplayGraph(Filename)) {
    errs() << "BOLT-ERROR: Can't display " << Filename << " with graphviz.\n";
  }
  if (auto EC = sys::fs::remove(Filename)) {
    errs() << "BOLT-WARNING: " << EC.message() << ", failed to remove "
           << Filename << "\n";
  }
}

void BinaryFunction::dumpGraphForPass(std::string Annotation) const {
  auto Filename = constructFilename(getPrintName(), Annotation, ".dot");
  outs() << "BOLT-DEBUG: Dumping CFG to " << Filename << "\n";
  dumpGraphToFile(Filename);
}

void BinaryFunction::dumpGraphToFile(std::string Filename) const {
  std::error_code EC;
  raw_fd_ostream of(Filename, EC, sys::fs::F_None);
  if (EC) {
    if (opts::Verbosity >= 1) {
      errs() << "BOLT-WARNING: " << EC.message() << ", unable to open "
             << Filename << " for output.\n";
    }
    return;
  }
  dumpGraph(of);
}

bool BinaryFunction::validateCFG() const {
  bool Valid = true;
  for (auto *BB : BasicBlocks) {
    Valid &= BB->validateSuccessorInvariants();
  }

  if (!Valid)
    return Valid;

  // Make sure all blocks in CFG are valid.
  auto validateBlock = [this](const BinaryBasicBlock *BB, StringRef Desc) {
    if (!BB->isValid()) {
      errs() << "BOLT-ERROR: deleted " << Desc << " " << BB->getName()
             << " detected in:\n";
      this->dump();
      return false;
    }
    return true;
  };
  for (const auto *BB : BasicBlocks) {
    if (!validateBlock(BB, "block"))
      return false;
    for (const auto *PredBB : BB->predecessors())
      if (!validateBlock(PredBB, "predecessor"))
        return false;
    for (const auto *SuccBB: BB->successors())
      if (!validateBlock(SuccBB, "successor"))
        return false;
    for (const auto *LP: BB->landing_pads())
      if (!validateBlock(LP, "landing pad"))
        return false;
    for (const auto *Thrower: BB->throwers())
      if (!validateBlock(Thrower, "thrower"))
        return false;
  }

  for (const auto *BB : BasicBlocks) {
    std::unordered_set<const BinaryBasicBlock *> BBLandingPads;
    for (const auto *LP : BB->landing_pads()) {
      if (BBLandingPads.count(LP)) {
        errs() << "BOLT-ERROR: duplicate landing pad detected in"
               << BB->getName() << " in function " << *this << '\n';
        return false;
      }
      BBLandingPads.insert(LP);
    }

    std::unordered_set<const BinaryBasicBlock *> BBThrowers;
    for (const auto *Thrower : BB->throwers()) {
      if (BBThrowers.count(Thrower)) {
        errs() << "BOLT-ERROR: duplicate thrower detected in"
               << BB->getName() << " in function " << *this << '\n';
        return false;
      }
      BBThrowers.insert(Thrower);
    }

    for (const auto *LPBlock : BB->landing_pads()) {
      if (std::find(LPBlock->throw_begin(), LPBlock->throw_end(), BB)
            == LPBlock->throw_end()) {
        errs() << "BOLT-ERROR: inconsistent landing pad detected in "
               << *this << ": " << BB->getName()
               << " is in LandingPads but not in " << LPBlock->getName()
               << " Throwers\n";
        return false;
      }
    }
    for (const auto *Thrower : BB->throwers()) {
      if (std::find(Thrower->lp_begin(), Thrower->lp_end(), BB)
            == Thrower->lp_end()) {
        errs() << "BOLT-ERROR: inconsistent thrower detected in "
               << *this << ": " << BB->getName()
               << " is in Throwers list but not in " << Thrower->getName()
               << " LandingPads\n";
        return false;
      }
    }
  }

  return Valid;
}

void BinaryFunction::fixBranches() {
  auto &MIB = BC.MIB;
  auto *Ctx = BC.Ctx.get();

  for (unsigned I = 0, E = BasicBlocksLayout.size(); I != E; ++I) {
    BinaryBasicBlock *BB = BasicBlocksLayout[I];
    const MCSymbol *TBB = nullptr;
    const MCSymbol *FBB = nullptr;
    MCInst *CondBranch = nullptr;
    MCInst *UncondBranch = nullptr;
    if (!BB->analyzeBranch(TBB, FBB, CondBranch, UncondBranch))
      continue;

    // We will create unconditional branch with correct destination if needed.
    if (UncondBranch)
      BB->eraseInstruction(BB->findInstruction(UncondBranch));

    // Basic block that follows the current one in the final layout.
    const BinaryBasicBlock *NextBB = nullptr;
    if (I + 1 != E && BB->isCold() == BasicBlocksLayout[I + 1]->isCold())
      NextBB = BasicBlocksLayout[I + 1];

    if (BB->succ_size() == 1) {
      // __builtin_unreachable() could create a conditional branch that
      // falls-through into the next function - hence the block will have only
      // one valid successor. Since behaviour is undefined - we replace
      // the conditional branch with an unconditional if required.
      if (CondBranch)
        BB->eraseInstruction(BB->findInstruction(CondBranch));
      if (BB->getSuccessor() == NextBB)
        continue;
      BB->addBranchInstruction(BB->getSuccessor());
    } else if (BB->succ_size() == 2) {
      assert(CondBranch && "conditional branch expected");
      const auto *TSuccessor = BB->getConditionalSuccessor(true);
      const auto *FSuccessor = BB->getConditionalSuccessor(false);
      if (NextBB && NextBB == TSuccessor) {
        std::swap(TSuccessor, FSuccessor);
        MIB->reverseBranchCondition(*CondBranch, TSuccessor->getLabel(), Ctx);
        BB->swapConditionalSuccessors();
      } else {
        MIB->replaceBranchTarget(*CondBranch, TSuccessor->getLabel(), Ctx);
      }
      if (TSuccessor == FSuccessor) {
        BB->removeDuplicateConditionalSuccessor(CondBranch);
      }
      if (!NextBB || (NextBB != TSuccessor && NextBB != FSuccessor)) {
        // If one of the branches is guaranteed to be "long" while the other
        // could be "short", then prioritize short for "taken". This will
        // generate a sequence 1 byte shorter on x86.
        if (BC.isX86() &&
            TSuccessor->isCold() != FSuccessor->isCold() &&
            BB->isCold() != TSuccessor->isCold()) {
          std::swap(TSuccessor, FSuccessor);
          MIB->reverseBranchCondition(*CondBranch, TSuccessor->getLabel(), Ctx);
          BB->swapConditionalSuccessors();
        }
        BB->addBranchInstruction(FSuccessor);
      }
    }
    // Cases where the number of successors is 0 (block ends with a
    // terminator) or more than 2 (switch table) don't require branch
    // instruction adjustments.
  }
  assert((!isSimple() || validateCFG())
         && "Invalid CFG detected after fixing branches");
}

void BinaryFunction::propagateGnuArgsSizeInfo() {
  assert(CurrentState == State::Disassembled && "unexpected function state");

  if (!hasEHRanges() || !usesGnuArgsSize())
    return;

  // The current value of DW_CFA_GNU_args_size affects all following
  // invoke instructions until the next CFI overrides it.
  // It is important to iterate basic blocks in the original order when
  // assigning the value.
  uint64_t CurrentGnuArgsSize = 0;
  for (auto BB : BasicBlocks) {
    for (auto II = BB->begin(); II != BB->end(); ) {
      auto &Instr = *II;
      if (BC.MIB->isCFI(Instr)) {
        auto CFI = getCFIFor(Instr);
        if (CFI->getOperation() == MCCFIInstruction::OpGnuArgsSize) {
          CurrentGnuArgsSize = CFI->getOffset();
          // Delete DW_CFA_GNU_args_size instructions and only regenerate
          // during the final code emission. The information is embedded
          // inside call instructions.
          II = BB->erasePseudoInstruction(II);
          continue;
        }
      } else if (BC.MIB->isInvoke(Instr)) {
        // Add the value of GNU_args_size as an extra operand to invokes.
        BC.MIB->addGnuArgsSize(Instr, CurrentGnuArgsSize);
      }
      ++II;
    }
  }
}

void BinaryFunction::postProcessBranches() {
  if (!isSimple())
    return;
  for (auto *BB : BasicBlocksLayout) {
    auto LastInstrRI = BB->getLastNonPseudo();
    if (BB->succ_size() == 1) {
      if (LastInstrRI != BB->rend() &&
          BC.MIB->isConditionalBranch(*LastInstrRI)) {
        // __builtin_unreachable() could create a conditional branch that
        // falls-through into the next function - hence the block will have only
        // one valid successor. Such behaviour is undefined and thus we remove
        // the conditional branch while leaving a valid successor.
        BB->eraseInstruction(std::prev(LastInstrRI.base()));
        DEBUG(dbgs() << "BOLT-DEBUG: erasing conditional branch in "
                     << BB->getName() << " in function " << *this << '\n');
      }
    } else if (BB->succ_size() == 0) {
      // Ignore unreachable basic blocks.
      if (BB->pred_size() == 0 || BB->isLandingPad())
        continue;

      // If it's the basic block that does not end up with a terminator - we
      // insert a return instruction unless it's a call instruction.
      if (LastInstrRI == BB->rend()) {
        DEBUG(dbgs() << "BOLT-DEBUG: at least one instruction expected in BB "
                     << BB->getName() << " in function " << *this << '\n');
        continue;
      }
      if (!BC.MIB->isTerminator(*LastInstrRI) &&
          !BC.MIB->isCall(*LastInstrRI)) {
        DEBUG(dbgs() << "BOLT-DEBUG: adding return to basic block "
                     << BB->getName() << " in function " << *this << '\n');
        MCInst ReturnInstr;
        BC.MIB->createReturn(ReturnInstr);
        BB->addInstruction(ReturnInstr);
      }
    }
  }
  assert(validateCFG() && "invalid CFG");
}

const MCSymbol *BinaryFunction::getSymbolForEntry(uint64_t EntryNum) const {
  if (EntryNum == 0)
    return getSymbol();

  if (!isMultiEntry())
    return nullptr;

  uint64_t NumEntries = 0;
  for (auto *BB : BasicBlocks) {
    if (!BB->isEntryPoint())
      continue;
    if (NumEntries == EntryNum)
      return BB->getLabel();
    ++NumEntries;
  }

  return nullptr;
}

uint64_t BinaryFunction::getEntryForSymbol(const MCSymbol *EntrySymbol) const {
  if (getSymbol() == EntrySymbol)
    return 0;

  uint64_t NumEntries = 0;
  for (const auto *BB : BasicBlocks) {
    if (!BB->isEntryPoint())
      continue;
    if (BB->getLabel() == EntrySymbol)
      return NumEntries;
    ++NumEntries;
  }

  llvm_unreachable("no entry for symbol");
}

BinaryFunction::BasicBlockOrderType BinaryFunction::dfs() const {
  BasicBlockOrderType DFS;
  unsigned Index = 0;
  std::stack<BinaryBasicBlock *> Stack;

  // Push entry points to the stack in reverse order.
  //
  // NB: we rely on the original order of entries to match.
  for (auto BBI = layout_rbegin(); BBI != layout_rend(); ++BBI) {
    auto *BB = *BBI;
    if (BB->isEntryPoint())
      Stack.push(BB);
    BB->setLayoutIndex(BinaryBasicBlock::InvalidIndex);
  }

  while (!Stack.empty()) {
    auto *BB = Stack.top();
    Stack.pop();

    if (BB->getLayoutIndex() != BinaryBasicBlock::InvalidIndex)
      continue;

    BB->setLayoutIndex(Index++);
    DFS.push_back(BB);

    for (auto *SuccBB : BB->landing_pads()) {
      Stack.push(SuccBB);
    }

    const MCSymbol *TBB = nullptr;
    const MCSymbol *FBB = nullptr;
    MCInst *CondBranch = nullptr;
    MCInst *UncondBranch = nullptr;
    if (BB->analyzeBranch(TBB, FBB, CondBranch, UncondBranch) &&
        CondBranch && BB->succ_size() == 2) {
      if (BC.MIB->getCanonicalBranchOpcode(CondBranch->getOpcode()) ==
          CondBranch->getOpcode()) {
        Stack.push(BB->getConditionalSuccessor(true));
        Stack.push(BB->getConditionalSuccessor(false));
      } else {
        Stack.push(BB->getConditionalSuccessor(false));
        Stack.push(BB->getConditionalSuccessor(true));
      }
    } else {
      for (auto *SuccBB : BB->successors()) {
        Stack.push(SuccBB);
      }
    }
  }

  return DFS;
}

std::size_t BinaryFunction::hash(bool Recompute, bool UseDFS) const {
  if (size() == 0)
    return 0;

  assert(hasCFG() && "function is expected to have CFG");

  if (!Recompute)
    return Hash;

  const auto &Order = UseDFS ? dfs() : BasicBlocksLayout;

  // The hash is computed by creating a string of all the opcodes
  // in the function and hashing that string with std::hash.
  std::string Opcodes;
  for (const auto *BB : Order) {
    for (const auto &Inst : *BB) {
      unsigned Opcode = Inst.getOpcode();

      if (BC.MII->get(Opcode).isPseudo())
        continue;

      // Ignore unconditional jumps since we check CFG consistency by processing
      // basic blocks in order and do not rely on branches to be in-sync with
      // CFG. Note that we still use condition code of conditional jumps.
      if (BC.MIB->isUnconditionalBranch(Inst))
        continue;

      if (Opcode == 0) {
        Opcodes.push_back(0);
        continue;
      }

      while (Opcode) {
        uint8_t LSB = Opcode & 0xff;
        Opcodes.push_back(LSB);
        Opcode = Opcode >> 8;
      }
    }
  }

  return Hash = std::hash<std::string>{}(Opcodes);
}

void BinaryFunction::insertBasicBlocks(
    BinaryBasicBlock *Start,
    std::vector<std::unique_ptr<BinaryBasicBlock>> &&NewBBs,
    const bool UpdateLayout,
    const bool UpdateCFIState) {
  const auto StartIndex = Start ? getIndex(Start) : -1;
  const auto NumNewBlocks = NewBBs.size();

  BasicBlocks.insert(BasicBlocks.begin() + (StartIndex + 1),
                     NumNewBlocks,
                     nullptr);

  auto I = StartIndex + 1;
  for (auto &BB : NewBBs) {
    assert(!BasicBlocks[I]);
    BasicBlocks[I++] = BB.release();
  }

  recomputeLandingPads();

  if (UpdateLayout) {
    updateLayout(Start, NumNewBlocks);
  }

  if (UpdateCFIState) {
    updateCFIState(Start, NumNewBlocks);
  }
}

BinaryFunction::iterator BinaryFunction::insertBasicBlocks(
    BinaryFunction::iterator StartBB,
    std::vector<std::unique_ptr<BinaryBasicBlock>> &&NewBBs,
    const bool UpdateLayout,
    const bool UpdateCFIState) {
  const auto StartIndex = getIndex(&*StartBB);
  const auto NumNewBlocks = NewBBs.size();

  BasicBlocks.insert(BasicBlocks.begin() + StartIndex + 1,
		     NumNewBlocks,
		     nullptr);
  auto RetIter = BasicBlocks.begin() + StartIndex + 1;

  auto I = StartIndex + 1;
  for (auto &BB : NewBBs) {
    assert(!BasicBlocks[I]);
    BasicBlocks[I++] = BB.release();
  }

  recomputeLandingPads();

  if (UpdateLayout) {
    updateLayout(*std::prev(RetIter), NumNewBlocks);
  }

  if (UpdateCFIState) {
    updateCFIState(*std::prev(RetIter), NumNewBlocks);
  }

  return RetIter;
}

void BinaryFunction::updateBBIndices(const unsigned StartIndex) {
  for (auto I = StartIndex; I < BasicBlocks.size(); ++I) {
    BasicBlocks[I]->Index = I;
  }
}

void BinaryFunction::updateCFIState(BinaryBasicBlock *Start,
                                    const unsigned NumNewBlocks) {
  const auto CFIState = Start->getCFIStateAtExit();
  const auto StartIndex = getIndex(Start) + 1;
  for (unsigned I = 0; I < NumNewBlocks; ++I) {
    BasicBlocks[StartIndex + I]->setCFIState(CFIState);
  }
}

void BinaryFunction::updateLayout(BinaryBasicBlock *Start,
                                  const unsigned NumNewBlocks) {
  // If start not provided insert new blocks at the beginning
  if (!Start) {
    BasicBlocksLayout.insert(layout_begin(), BasicBlocks.begin(),
                             BasicBlocks.begin() + NumNewBlocks);
    updateLayoutIndices();
    return;
  }

  // Insert new blocks in the layout immediately after Start.
  auto Pos = std::find(layout_begin(), layout_end(), Start);
  assert(Pos != layout_end());
  auto Begin = &BasicBlocks[getIndex(Start) + 1];
  auto End = &BasicBlocks[getIndex(Start) + NumNewBlocks + 1];
  BasicBlocksLayout.insert(Pos + 1, Begin, End);
  updateLayoutIndices();
}

bool BinaryFunction::replaceJumpTableEntryIn(BinaryBasicBlock *BB,
                                             BinaryBasicBlock *OldDest,
                                             BinaryBasicBlock *NewDest) {
  auto *Instr = BB->getLastNonPseudoInstr();
  if (!Instr || !BC.MIB->isIndirectBranch(*Instr))
    return false;
  auto JTAddress = BC.MIB->getJumpTable(*Instr);
  assert(JTAddress && "Invalid jump table address");
  auto *JT = getJumpTableContainingAddress(JTAddress);
  assert(JT && "No jump table structure for this indirect branch");
  bool Patched = JT->replaceDestination(JTAddress, OldDest->getLabel(),
                                        NewDest->getLabel());
  assert(Patched && "Invalid entry to be replaced in jump table");
  return true;
}

BinaryBasicBlock *BinaryFunction::splitEdge(BinaryBasicBlock *From,
                                            BinaryBasicBlock *To) {
  // Create intermediate BB
  MCSymbol *Tmp = BC.Ctx->createTempSymbol("SplitEdge", true);
  auto NewBB = createBasicBlock(0, Tmp);
  auto NewBBPtr = NewBB.get();

  // Update "From" BB
  auto I = From->succ_begin();
  auto BI = From->branch_info_begin();
  for (; I != From->succ_end(); ++I) {
    if (*I == To)
      break;
    ++BI;
  }
  assert(I != From->succ_end() && "Invalid CFG edge in splitEdge!");
  uint64_t OrigCount{BI->Count};
  uint64_t OrigMispreds{BI->MispredictedCount};
  replaceJumpTableEntryIn(From, To, NewBBPtr);
  From->replaceSuccessor(To, NewBBPtr, OrigCount, OrigMispreds);

  NewBB->addSuccessor(To, OrigCount, OrigMispreds);
  NewBB->setExecutionCount(OrigCount);
  NewBB->setIsCold(From->isCold());

  // Update CFI and BB layout with new intermediate BB
  std::vector<std::unique_ptr<BinaryBasicBlock>> NewBBs;
  NewBBs.emplace_back(std::move(NewBB));
  insertBasicBlocks(From, std::move(NewBBs), true, true);
  return NewBBPtr;
}

bool BinaryFunction::isDataMarker(const SymbolRef &Symbol,
                                  uint64_t SymbolSize) const {
  // For aarch64, the ABI defines mapping symbols so we identify data in the
  // code section (see IHI0056B). $d identifies a symbol starting data contents.
  if (BC.isAArch64() && Symbol.getType() &&
      cantFail(Symbol.getType()) == SymbolRef::ST_Unknown && SymbolSize == 0 &&
      Symbol.getName() && cantFail(Symbol.getName()) == "$d")
    return true;
  return false;
}

bool BinaryFunction::isCodeMarker(const SymbolRef &Symbol,
                                  uint64_t SymbolSize) const {
  // For aarch64, the ABI defines mapping symbols so we identify data in the
  // code section (see IHI0056B). $x identifies a symbol starting code or the
  // end of a data chunk inside code.
  if (BC.isAArch64() && Symbol.getType() &&
      cantFail(Symbol.getType()) == SymbolRef::ST_Unknown && SymbolSize == 0 &&
      Symbol.getName() && cantFail(Symbol.getName()) == "$x")
    return true;
  return false;
}

bool BinaryFunction::isSymbolValidInScope(const SymbolRef &Symbol,
                                          uint64_t SymbolSize) const {
  // If this symbol is in a different section from the one where the
  // function symbol is, don't consider it as valid.
  if (!getSection().containsAddress(
          cantFail(Symbol.getAddress(), "cannot get symbol address")))
    return false;

  // Some symbols are tolerated inside function bodies, others are not.
  // The real function boundaries may not be known at this point.
  if (isDataMarker(Symbol, SymbolSize) || isCodeMarker(Symbol, SymbolSize))
    return true;

  // It's okay to have a zero-sized symbol in the middle of non-zero-sized
  // function.
  if (SymbolSize == 0 && containsAddress(cantFail(Symbol.getAddress())))
    return true;

  if (cantFail(Symbol.getType()) != SymbolRef::ST_Unknown)
    return false;

  if (Symbol.getFlags() & SymbolRef::SF_Global)
    return false;

  return true;
}

SMLoc BinaryFunction::emitLineInfo(SMLoc NewLoc, SMLoc PrevLoc,
                                   bool FirstInstr) const {
  auto *FunctionCU = UnitLineTable.first;
  const auto *FunctionLineTable = UnitLineTable.second;
  assert(FunctionCU && "cannot emit line info for function without CU");

  auto RowReference = DebugLineTableRowRef::fromSMLoc(NewLoc);

  // Check if no new line info needs to be emitted.
  if (RowReference == DebugLineTableRowRef::NULL_ROW ||
      NewLoc.getPointer() == PrevLoc.getPointer())
    return PrevLoc;

  unsigned CurrentFilenum = 0;
  const auto *CurrentLineTable = FunctionLineTable;

  // If the CU id from the current instruction location does not
  // match the CU id from the current function, it means that we
  // have come across some inlined code.  We must look up the CU
  // for the instruction's original function and get the line table
  // from that.
  const auto FunctionUnitIndex = FunctionCU->getOffset();
  const auto CurrentUnitIndex = RowReference.DwCompileUnitIndex;
  if (CurrentUnitIndex != FunctionUnitIndex) {
    CurrentLineTable = BC.DwCtx->getLineTableForUnit(
        BC.DwCtx->getCompileUnitForOffset(CurrentUnitIndex));
    // Add filename from the inlined function to the current CU.
    CurrentFilenum =
      BC.addDebugFilenameToUnit(FunctionUnitIndex, CurrentUnitIndex,
        CurrentLineTable->Rows[RowReference.RowIndex - 1].File);
  }

  const auto &CurrentRow = CurrentLineTable->Rows[RowReference.RowIndex - 1];
  if (!CurrentFilenum)
    CurrentFilenum = CurrentRow.File;

  unsigned Flags = (DWARF2_FLAG_IS_STMT * CurrentRow.IsStmt) |
                   (DWARF2_FLAG_BASIC_BLOCK * CurrentRow.BasicBlock) |
                   (DWARF2_FLAG_PROLOGUE_END * CurrentRow.PrologueEnd) |
                   (DWARF2_FLAG_EPILOGUE_BEGIN * CurrentRow.EpilogueBegin);

  // Always emit is_stmt at the beginning of function fragment.
  if (FirstInstr)
    Flags |= DWARF2_FLAG_IS_STMT;

  BC.Ctx->setCurrentDwarfLoc(
    CurrentFilenum,
    CurrentRow.Line,
    CurrentRow.Column,
    Flags,
    CurrentRow.Isa,
    CurrentRow.Discriminator);
  BC.Ctx->setDwarfCompileUnitID(FunctionUnitIndex);

  return NewLoc;
}

void BinaryFunction::adjustExecutionCount(uint64_t Count) {
  if (getKnownExecutionCount() == 0 || Count == 0)
    return;

  if (ExecutionCount < Count)
    Count = ExecutionCount;

  double AdjustmentRatio = ((double) ExecutionCount - Count) / ExecutionCount;
  if (AdjustmentRatio < 0.0)
    AdjustmentRatio = 0.0;

  for (auto &BB : layout())
    BB->adjustExecutionCount(AdjustmentRatio);

  ExecutionCount -= Count;
}

BinaryFunction::~BinaryFunction() {
  for (auto BB : BasicBlocks) {
    delete BB;
  }
  for (auto BB : DeletedBasicBlocks) {
    delete BB;
  }
}

void BinaryFunction::emitJumpTables(MCStreamer *Streamer) {
  if (JumpTables.empty())
    return;
  if (opts::PrintJumpTables) {
    outs() << "BOLT-INFO: jump tables for function " << *this << ":\n";
  }
  for (auto &JTI : JumpTables) {
    auto &JT = *JTI.second;
    if (opts::PrintJumpTables)
      JT.print(outs());
    if ((opts::JumpTables == JTS_BASIC || !isSimple()) && BC.HasRelocations) {
      JT.updateOriginal();
    } else {
      MCSection *HotSection, *ColdSection;
      if (opts::JumpTables == JTS_BASIC) {
        std::string Name = ".local." + JT.Labels[0]->getName().str();
        std::replace(Name.begin(), Name.end(), '/', '.');
        auto &Section = BC.registerOrUpdateSection(Name,
                                                   ELF::SHT_PROGBITS,
                                                   ELF::SHF_ALLOC);
        Section.setAnonymous(true);
        JT.setOutputSection(Section);
        HotSection = BC.Ctx->getELFSection(Name,
                                           ELF::SHT_PROGBITS,
                                           ELF::SHF_ALLOC);
        ColdSection = HotSection;
      } else {
        if (isSimple()) {
          HotSection = BC.MOFI->getReadOnlySection();
          ColdSection = BC.MOFI->getReadOnlyColdSection();
        } else {
          HotSection = hasProfile() ? BC.MOFI->getReadOnlySection()
                                    : BC.MOFI->getReadOnlyColdSection();
          ColdSection = HotSection;
        }
      }
      JT.emit(Streamer, HotSection, ColdSection);
    }
  }
}

void BinaryFunction::calculateLoopInfo() {
  // Discover loops.
  BinaryDominatorTree DomTree;
  DomTree.recalculate(*this);
  BLI.reset(new BinaryLoopInfo());
  BLI->analyze(DomTree);

  // Traverse discovered loops and add depth and profile information.
  std::stack<BinaryLoop *> St;
  for (auto I = BLI->begin(), E = BLI->end(); I != E; ++I) {
    St.push(*I);
    ++BLI->OuterLoops;
  }

  while (!St.empty()) {
    BinaryLoop *L = St.top();
    St.pop();
    ++BLI->TotalLoops;
    BLI->MaximumDepth = std::max(L->getLoopDepth(), BLI->MaximumDepth);

    // Add nested loops in the stack.
    for (BinaryLoop::iterator I = L->begin(), E = L->end(); I != E; ++I) {
      St.push(*I);
    }

    // Skip if no valid profile is found.
    if (!hasValidProfile()) {
      L->EntryCount = COUNT_NO_PROFILE;
      L->ExitCount = COUNT_NO_PROFILE;
      L->TotalBackEdgeCount = COUNT_NO_PROFILE;
      continue;
    }

    // Compute back edge count.
    SmallVector<BinaryBasicBlock *, 1> Latches;
    L->getLoopLatches(Latches);

    for (BinaryBasicBlock *Latch : Latches) {
      auto BI = Latch->branch_info_begin();
      for (BinaryBasicBlock *Succ : Latch->successors()) {
        if (Succ == L->getHeader()) {
          assert(BI->Count != BinaryBasicBlock::COUNT_NO_PROFILE &&
                 "profile data not found");
          L->TotalBackEdgeCount += BI->Count;
        }
        ++BI;
      }
    }

    // Compute entry count.
    L->EntryCount = L->getHeader()->getExecutionCount() - L->TotalBackEdgeCount;

    // Compute exit count.
    SmallVector<BinaryLoop::Edge, 1> ExitEdges;
    L->getExitEdges(ExitEdges);
    for (BinaryLoop::Edge &Exit : ExitEdges) {
      const BinaryBasicBlock *Exiting = Exit.first;
      const BinaryBasicBlock *ExitTarget = Exit.second;
      auto BI = Exiting->branch_info_begin();
      for (BinaryBasicBlock *Succ : Exiting->successors()) {
        if (Succ == ExitTarget) {
          assert(BI->Count != BinaryBasicBlock::COUNT_NO_PROFILE &&
                 "profile data not found");
          L->ExitCount += BI->Count;
        }
        ++BI;
      }
    }
  }
}

DebugAddressRangesVector BinaryFunction::getOutputAddressRanges() const {
  DebugAddressRangesVector OutputRanges;

  if (IsFragment)
    return OutputRanges;

  OutputRanges.emplace_back(getOutputAddress(),
                            getOutputAddress() + getOutputSize());
  if (isSplit()) {
    assert(isEmitted() && "split function should be emitted");
    OutputRanges.emplace_back(cold().getAddress(),
                              cold().getAddress() + cold().getImageSize());
  }

  if (isSimple())
    return OutputRanges;

  for (auto *Frag : Fragments) {
    assert(!Frag->isSimple() &&
           "fragment of non-simple function should also be non-simple");
    OutputRanges.emplace_back(Frag->getOutputAddress(),
                              Frag->getOutputAddress() + Frag->getOutputSize());
  }

  return OutputRanges;
}

uint64_t BinaryFunction::translateInputToOutputAddress(uint64_t Address) const {
  // If the function hasn't changed return the same address.
  if (!isEmitted() && !BC.HasRelocations)
    return Address;

  if (Address < getAddress())
    return 0;

  // FIXME: #18950828 - we rely on relative offsets inside basic blocks to stay
  //        intact. Instead we can use pseudo instructions and/or annotations.
  const auto Offset = Address - getAddress();
  const auto *BB = getBasicBlockContainingOffset(Offset);
  if (!BB) {
    // Special case for address immediately past the end of the function.
    if (Offset == getSize())
      return getOutputAddress() + getOutputSize();

    return 0;
  }

  return std::min(BB->getOutputAddressRange().first + Offset - BB->getOffset(),
                  BB->getOutputAddressRange().second);
}

DebugAddressRangesVector BinaryFunction::translateInputToOutputRanges(
    const DWARFAddressRangesVector &InputRanges) const {
  DebugAddressRangesVector OutputRanges;

  // If the function hasn't changed return the same ranges.
  if (!isEmitted() && !BC.HasRelocations) {
    OutputRanges.resize(InputRanges.size());
    std::transform(InputRanges.begin(), InputRanges.end(),
                   OutputRanges.begin(),
                   [](const DWARFAddressRange &Range) {
                     return DebugAddressRange(Range.LowPC, Range.HighPC);
                   });
    return OutputRanges;
  }

  // Even though we will merge ranges in a post-processing pass, we attempt to
  // merge them in a main processing loop as it improves the processing time.
  uint64_t PrevEndAddress = 0;
  for (const auto &Range : InputRanges) {
    if (!containsAddress(Range.LowPC)) {
      DEBUG(dbgs() << "BOLT-DEBUG: invalid debug address range detected for "
                   << *this << " : [0x" << Twine::utohexstr(Range.LowPC)
                   << ", 0x" << Twine::utohexstr(Range.HighPC) << "]\n");
      PrevEndAddress = 0;
      continue;
    }
    auto InputOffset = Range.LowPC - getAddress();
    const auto InputEndOffset = std::min(Range.HighPC - getAddress(), getSize());

    auto BBI = std::upper_bound(BasicBlockOffsets.begin(),
                                BasicBlockOffsets.end(),
                                BasicBlockOffset(InputOffset, nullptr),
                                CompareBasicBlockOffsets());
    --BBI;
    do {
      const auto *BB = BBI->second;
      if (InputOffset < BB->getOffset() || InputOffset >= BB->getEndOffset()) {
        DEBUG(dbgs() << "BOLT-DEBUG: invalid debug address range detected for "
                     << *this << " : [0x" << Twine::utohexstr(Range.LowPC)
                     << ", 0x" << Twine::utohexstr(Range.HighPC) << "]\n");
        PrevEndAddress = 0;
        break;
      }

      // Skip the range if the block was deleted.
      if (const auto OutputStart = BB->getOutputAddressRange().first) {
        const auto StartAddress = OutputStart + InputOffset - BB->getOffset();
        auto EndAddress = BB->getOutputAddressRange().second;
        if (InputEndOffset < BB->getEndOffset())
          EndAddress = StartAddress + InputEndOffset - InputOffset;

        if (StartAddress == PrevEndAddress) {
          OutputRanges.back().HighPC = std::max(OutputRanges.back().HighPC,
                                                EndAddress);
        } else {
          OutputRanges.emplace_back(StartAddress,
                                    std::max(StartAddress, EndAddress));
        }
        PrevEndAddress = OutputRanges.back().HighPC;
      }

      InputOffset = BB->getEndOffset();
      ++BBI;
    } while (InputOffset < InputEndOffset);
  }

  // Post-processing pass to sort and merge ranges.
  std::sort(OutputRanges.begin(), OutputRanges.end());
  DebugAddressRangesVector MergedRanges;
  PrevEndAddress = 0;
  for (const auto &Range : OutputRanges) {
    if (Range.LowPC <= PrevEndAddress) {
      MergedRanges.back().HighPC = std::max(MergedRanges.back().HighPC,
                                            Range.HighPC);
    } else {
      MergedRanges.emplace_back(Range.LowPC, Range.HighPC);
    }
    PrevEndAddress = MergedRanges.back().HighPC;
  }

  return MergedRanges;
}

MCInst *BinaryFunction::getInstructionAtOffset(uint64_t Offset) {
  if (CurrentState == State::Disassembled) {
    auto II = Instructions.find(Offset);
    return (II == Instructions.end()) ? nullptr : &II->second;
  } else if (CurrentState == State::CFG) {
    auto *BB = getBasicBlockContainingOffset(Offset);
    if (!BB)
      return nullptr;

    for (auto &Inst : *BB) {
      constexpr auto InvalidOffset = std::numeric_limits<uint64_t>::max();
      if (Offset == BC.MIB->getAnnotationWithDefault<uint64_t>(Inst, "Offset",
                                                               InvalidOffset))
        return &Inst;
    }

    return nullptr;
  } else {
    llvm_unreachable("invalid CFG state to use getInstructionAtOffset()");
  }
}

std::set<BinaryData *> BinaryFunction::dataUses(bool OnlyHot) const {
  std::set<BinaryData *> Uses;
  for (auto *BB : BasicBlocks) {
    if (OnlyHot && BB->isCold())
      continue;

    for (const auto &Inst : *BB) {
      if (auto Mem =
            BC.MIB->tryGetAnnotationAs<uint64_t>(Inst, "MemDataOffset")) {
        for (auto &MI : getMemData()->getMemInfoRange(Mem.get())) {
          if (auto *BD = MI.Addr.IsSymbol
                ? BC.getBinaryDataByName(MI.Addr.Name)
                : BC.getBinaryDataContainingAddress(MI.Addr.Offset)) {
            Uses.insert(BD);
          }
        }
      }
    }
  }
  return Uses;
}

DWARFDebugLoc::LocationList BinaryFunction::translateInputToOutputLocationList(
      const DWARFDebugLoc::LocationList &InputLL,
      BaseAddress BaseAddr) const {
  uint64_t BAddr = BaseAddr.Address;
  // If the function wasn't changed - there's nothing to update.
  if (!isEmitted() && !BC.HasRelocations) {
    if (!BAddr) {
      return InputLL;
    } else {
      auto OutputLL = std::move(InputLL);
      for (auto &Entry : OutputLL.Entries) {
        Entry.Begin += BAddr;
        Entry.End += BAddr;
      }
      return OutputLL;
    }
  }

  uint64_t PrevEndAddress = 0;
  SmallVectorImpl<char> *PrevLoc = nullptr;
  DWARFDebugLoc::LocationList OutputLL;
  for (auto &Entry : InputLL.Entries) {
    const auto Start = Entry.Begin + BAddr;
    const auto End = Entry.End + BAddr;
    if (!containsAddress(Start)) {
      DEBUG(dbgs() << "BOLT-DEBUG: invalid debug address range detected for "
                   << *this << " : [0x" << Twine::utohexstr(Start)
                   << ", 0x" << Twine::utohexstr(End) << "]\n");
      continue;
    }
    auto InputOffset = Start - getAddress();
    const auto InputEndOffset = std::min(End - getAddress(), getSize());
    auto BBI = std::upper_bound(BasicBlockOffsets.begin(),
                                BasicBlockOffsets.end(),
                                BasicBlockOffset(InputOffset, nullptr),
                                CompareBasicBlockOffsets());
    --BBI;
    do {
      const auto *BB = BBI->second;
      if (InputOffset < BB->getOffset() || InputOffset >= BB->getEndOffset()) {
        DEBUG(dbgs() << "BOLT-DEBUG: invalid debug address range detected for "
                     << *this << " : [0x" << Twine::utohexstr(Start)
                     << ", 0x" << Twine::utohexstr(End) << "]\n");
        PrevEndAddress = 0;
        break;
      }

      // Skip the range if the block was deleted.
      if (const auto OutputStart = BB->getOutputAddressRange().first) {
        const auto StartAddress = OutputStart + InputOffset - BB->getOffset();
        auto EndAddress = BB->getOutputAddressRange().second;
        if (InputEndOffset < BB->getEndOffset())
          EndAddress = StartAddress + InputEndOffset - InputOffset;

        if (StartAddress == PrevEndAddress && Entry.Loc == *PrevLoc) {
          OutputLL.Entries.back().End = std::max(OutputLL.Entries.back().End,
                                                 EndAddress);
        } else {
          OutputLL.Entries.emplace_back(
              DWARFDebugLoc::Entry{StartAddress,
                                   std::max(StartAddress, EndAddress),
                                   Entry.Loc});
        }
        PrevEndAddress = OutputLL.Entries.back().End;
        PrevLoc = &OutputLL.Entries.back().Loc;
      }

      ++BBI;
      InputOffset = BB->getEndOffset();
    } while (InputOffset < InputEndOffset);
  }

  // Sort and merge adjacent entries with identical location.
  std::stable_sort(OutputLL.Entries.begin(), OutputLL.Entries.end(),
      [] (const DWARFDebugLoc::Entry &A, const DWARFDebugLoc::Entry &B) {
        return A.Begin < B.Begin;
      });
  DWARFDebugLoc::LocationList MergedLL;
  PrevEndAddress = 0;
  PrevLoc = nullptr;
  for (const auto &Entry : OutputLL.Entries) {
    if (Entry.Begin <= PrevEndAddress && *PrevLoc == Entry.Loc) {
      MergedLL.Entries.back().End = std::max(Entry.End,
                                             MergedLL.Entries.back().End);
    } else {
      const auto Begin = std::max(Entry.Begin, PrevEndAddress);
      const auto End = std::max(Begin, Entry.End);
      MergedLL.Entries.emplace_back(DWARFDebugLoc::Entry{Begin,
                                                         End,
                                                         Entry.Loc});
    }
    PrevEndAddress = MergedLL.Entries.back().End;
    PrevLoc = &MergedLL.Entries.back().Loc;
  }

  return MergedLL;
}

void BinaryFunction::printLoopInfo(raw_ostream &OS) const {
  OS << "Loop Info for Function \"" << *this << "\"";
  if (hasValidProfile()) {
    OS << " (count: " << getExecutionCount() << ")";
  }
  OS << "\n";

  std::stack<BinaryLoop *> St;
  for (auto I = BLI->begin(), E = BLI->end(); I != E; ++I) {
    St.push(*I);
  }
  while (!St.empty()) {
    BinaryLoop *L = St.top();
    St.pop();

    for (BinaryLoop::iterator I = L->begin(), E = L->end(); I != E; ++I) {
      St.push(*I);
    }

    if (!hasValidProfile())
      continue;

    OS << (L->getLoopDepth() > 1 ? "Nested" : "Outer") << " loop header: "
       << L->getHeader()->getName();
    OS << "\n";
    OS << "Loop basic blocks: ";
    auto Sep = "";
    for (auto BI = L->block_begin(), BE = L->block_end(); BI != BE; ++BI) {
      OS << Sep << (*BI)->getName();
      Sep = ", ";
    }
    OS << "\n";
    if (hasValidProfile()) {
      OS << "Total back edge count: " << L->TotalBackEdgeCount << "\n";
      OS << "Loop entry count: " << L->EntryCount << "\n";
      OS << "Loop exit count: " << L->ExitCount << "\n";
      if (L->EntryCount > 0) {
        OS << "Average iters per entry: "
           << format("%.4lf", (double)L->TotalBackEdgeCount / L->EntryCount)
           << "\n";
      }
    }
    OS << "----\n";
  }

  OS << "Total number of loops: "<< BLI->TotalLoops << "\n";
  OS << "Number of outer loops: " << BLI->OuterLoops << "\n";
  OS << "Maximum nested loop depth: " << BLI->MaximumDepth << "\n\n";
}

bool BinaryFunction::isAArch64Veneer() const {
  if (BasicBlocks.size() != 1)
    return false;

  auto &BB = **BasicBlocks.begin();
  if (BB.size() != 3)
    return false;

  for (auto &Inst : BB) {
    if (!BC.MIB->hasAnnotation(Inst, "AArch64Veneer"))
      return false;
  }

  return true;
}

} // namespace bolt
} // namespace llvm
