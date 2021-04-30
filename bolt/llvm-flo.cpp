//===-- llvm-flo.cpp - Feedback-directed layout optimizer -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is a binary optimizer that will take 'perf' output and change
// basic block layout for better performance (a.k.a. branch straightening),
// plus some other optimizations that are better performed on a binary.
//
//===----------------------------------------------------------------------===//

#include "BinaryBasicBlock.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "DataReader.h"
#include "Exceptions.h"
#include "llvm/ADT/STLExtras.h"
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
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <map>
#include <stack>
#include <system_error>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "flo"

using namespace llvm;
using namespace object;
using namespace flo;

namespace opts {

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<executable>"), cl::Required);

static cl::opt<std::string>
InputDataFilename("data", cl::desc("<data file>"), cl::Optional);

static cl::opt<std::string>
OutputFilename("o", cl::desc("<output file>"), cl::Required);

static cl::list<std::string>
FunctionNames("funcs",
              cl::CommaSeparated,
              cl::desc("list of functions to optimize"),
              cl::value_desc("func1,func2,func3,..."));

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

static cl::opt<std::string> ReorderBlocks(
    "reorder-blocks",
    cl::desc("redo basic block layout based on profiling data with a specific "
             "priority (none, branch-predictor or cache)"),
    cl::value_desc("priority"), cl::init("disable"));

static cl::opt<bool>
DumpData("dump-data", cl::desc("dump parsed flo data and exit (debugging)"),
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
PrintReordered("print-reordered",
               cl::desc("print functions after layout optimization"),
               cl::Hidden);


// Check against lists of functions from options if we should
// optimize the function with a given name.
bool shouldProcess(StringRef FunctionName) {
  bool IsValid = true;
  if (!FunctionNames.empty()) {
    IsValid = false;
    for (auto &Name : FunctionNames) {
      if (FunctionName == Name) {
        IsValid = true;
        break;
      }
    }
  }
  if (!IsValid)
    return false;

  if (!SkipFunctionNames.empty()) {
    for (auto &Name : SkipFunctionNames) {
      if (FunctionName == Name) {
        IsValid = false;
        break;
      }
    }
  }

  return IsValid;
}

} // namespace opts

static StringRef ToolName;

static void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << ToolName << ": '" << Message << "': " << EC.message() << ".\n";
  exit(1);
}

static void check_error(std::error_code EC, StringRef Message) {
  if (!EC)
    return;
  report_error(Message, EC);
}

template <typename T>
static std::vector<T> singletonSet(T t) {
  std::vector<T> Vec;
  Vec.push_back(std::move(t));
  return Vec;
}

/// Class responsible for allocating and managing code and data sections.
class ExecutableFileMemoryManager : public SectionMemoryManager {
public:

  // Keep [section name] -> [allocated address, size] map for later remapping.
  std::map<std::string, std::pair<uint64_t,uint64_t>> SectionAddressInfo;

  ExecutableFileMemoryManager() {}

  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override {
    auto ret =
      SectionMemoryManager::allocateCodeSection(Size, Alignment, SectionID,
                                                SectionName);
    DEBUG(dbgs() << "FLO: allocating code section : " << SectionName
                 << " with size " << Size << ", alignment " << Alignment
                 << " at 0x" << ret << "\n");

    SectionAddressInfo[SectionName] = {reinterpret_cast<uint64_t>(ret), Size};

    return ret;
  }

  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool IsReadOnly) override {
    DEBUG(dbgs() << "FLO: allocating data section : " << SectionName
                 << " with size " << Size << ", alignment "
                 << Alignment << "\n");
    errs() << "FLO-WARNING: allocating data section.\n";
    return SectionMemoryManager::allocateDataSection(Size, Alignment, SectionID,
                                                     SectionName, IsReadOnly);
  }

  // Tell EE that we guarantee we don't need stubs.
  bool allowStubAllocation() const override { return false; }

  bool finalizeMemory(std::string *ErrMsg = nullptr) override {
    DEBUG(dbgs() << "FLO: finalizeMemory()\n");
    return SectionMemoryManager::finalizeMemory(ErrMsg);
  }
};

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
    errs() << ToolName << ": " << Error;
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

  auto MCE = TheTarget->createMCCodeEmitter(*MII, *MRI, *Ctx);

  auto MAB = TheTarget->createMCAsmBackend(*MRI, TripleName, "");

  // Make sure we don't miss any output on core dumps.
  outs().SetUnbuffered();
  errs().SetUnbuffered();
  dbgs().SetUnbuffered();

  auto BC =
      llvm::make_unique<BinaryContext>(std::move(Ctx),
                                       std::move(TheTriple),
                                       TheTarget,
                                       MCE,
                                       std::move(MOFI),
                                       std::move(AsmInfo),
                                       std::move(MII),
                                       std::move(STI),
                                       std::move(InstructionPrinter),
                                       std::move(MIA),
                                       std::move(MRI),
                                       std::move(DisAsm),
                                       MAB,
                                       DR);

  return BC;
}

static void OptimizeFile(ELFObjectFileBase *File, const DataReader &DR) {

  // FIXME: there should be some way to extract arch and triple information
  //        from the file.
  std::unique_ptr<BinaryContext> BC =
    std::move(CreateBinaryContext("x86-64", "x86_64-unknown-linux", DR));
  if (!BC) {
    errs() << "failed to create a binary context\n";
    return;
  }

  // Store all non-zero symbols in this map for a quick address lookup.
  std::map<uint64_t, SymbolRef> FileSymRefs;

  // Entry point to the binary.
  //
  // Note: this is ELF header entry point, but we could have more entry points
  // from constructors etc.
  BinaryFunction *EntryPointFunction{nullptr};

  // Populate array of binary functions and file symbols
  // from file symbol table.
  //
  // For local symbols we want to keep track of associated FILE symbol for
  // disambiguation by name.
  std::map<uint64_t, BinaryFunction> BinaryFunctions;
  std::string FileSymbolName;
  for (const SymbolRef &Symbol : File->symbols()) {
    // Keep undefined symbols for pretty printing?
    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    ErrorOr<StringRef> Name = Symbol.getName();
    check_error(Name.getError(), "cannot get symbol name");

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
    } else {
      unsigned LocalCount = 1;
      std::string LocalName = (*Name).str() + "/" + FileSymbolName + "/";
      while (BC->GlobalSymbols.find(LocalName + std::to_string(LocalCount)) !=
             BC->GlobalSymbols.end()) {
        ++LocalCount;
      }
      UniqueName = LocalName + std::to_string(LocalCount);
    }

    /// It's possible we are seeing a globalized local. Even though
    /// we've made the name unique, LLVM might still treat it as local
    /// if it has a "private global" prefix, e.g. ".L". Thus we have to
    /// change the prefix to enforce global scope of the symbol.
    if (StringRef(UniqueName).startswith(BC->AsmInfo->getPrivateGlobalPrefix()))
      UniqueName = "PG." + UniqueName;

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

  // Process special sections.
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
      readLSDA(SectionData, *BC);
    }
  }

  // Disassemble every function and build it's control flow graph.
  for (auto &BFI : BinaryFunctions) {
    BinaryFunction &Function = BFI.second;

    if (!opts::shouldProcess(Function.getName())) {
      DEBUG(dbgs() << "FLO: skipping processing function " << Function.getName()
                   << " per user request.\n");
      continue;
    }

    SectionRef Section = Function.getSection();
    assert(Section.containsSymbol(Function.getSymbol()) &&
           "symbol not in section");

    // When could it happen?
    if (!Section.isText() || Section.isVirtual() || !Section.getSize()) {
      DEBUG(dbgs() << "FLO: corresponding section non-executable or empty "
                   << "for function " << Function.getName());
      continue;
    }

    // Set the proper maximum size value after the whole symbol table
    // has been processed.
    auto SymRefI = FileSymRefs.upper_bound(Function.getAddress());
    if (SymRefI != FileSymRefs.end()) {
      auto MaxSize = SymRefI->first - Function.getAddress();
      if (MaxSize < Function.getSize()) {
        DEBUG(dbgs() << "FLO: symbol seen in the middle of the function "
                     << Function.getName() << ". Skipping.\n");
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

    if (!Function.buildCFG())
      continue;

    if (opts::PrintAll || opts::PrintCFG)
      Function.print(errs(), "after building cfg");

  } // Iterate over all functions

  // Run optimization passes.
  //
  // FIXME: use real optimization passes.
  bool NagUser = true;
  if (opts::ReorderBlocks != "" &&
      opts::ReorderBlocks != "disable" &&
      opts::ReorderBlocks != "none" &&
      opts::ReorderBlocks != "branch-predictor" &&
      opts::ReorderBlocks != "cache") {
    errs() << ToolName << ": Unrecognized block reordering priority \""
           << opts::ReorderBlocks << "\".\n";
    exit(1);
  }
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (!opts::shouldProcess(Function.getName()))
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

    if (opts::ReorderBlocks != "disable") {
      if (opts::ReorderBlocks == "branch-predictor") {
        BFI.second.optimizeLayout(BinaryFunction::HP_BRANCH_PREDICTOR);
      } else if (opts::ReorderBlocks == "cache") {
        BFI.second.optimizeLayout(BinaryFunction::HP_CACHE_UTILIZATION);
      } else {
        BFI.second.optimizeLayout(BinaryFunction::HP_NONE);
      }
      if (opts::PrintAll || opts::PrintReordered)
        Function.print(errs(), "after reordering blocks");
    }
  }

  std::error_code EC;

  // This is an object file, which we keep for debugging purposes.
  // Once we decide it's useless, we should create it in memory.
  std::unique_ptr<tool_output_file> Out =
    llvm::make_unique<tool_output_file>(opts::OutputFilename + ".o",
                                        EC, sys::fs::F_None);
  check_error(EC, "cannot create output object file");

  std::unique_ptr<tool_output_file> RealOut =
    llvm::make_unique<tool_output_file>(opts::OutputFilename,
                                        EC,
                                        sys::fs::F_None,
                                        0777);
  check_error(EC, "cannot create output executable file");

  // Copy input file.
  RealOut->os() << File->getData();

  std::unique_ptr<buffer_ostream> BOS =
      make_unique<buffer_ostream>(Out->os());
  raw_pwrite_stream *OS = BOS.get();

  // Implicitly MCObjectStreamer takes ownership of MCAsmBackend (MAB)
  // and MCCodeEmitter (MCE). ~MCObjectStreamer() will delete these
  // two instances.
  std::unique_ptr<MCStreamer> Streamer(
    BC->TheTarget->createMCObjectStreamer(*BC->TheTriple,
                                          *BC->Ctx,
                                          *BC->MAB,
                                          *OS,
                                          BC->MCE,
                                          *BC->STI,
                                          /* RelaxAll */ false,
                                          /* DWARFMustBeAtTheEnd */ false));

  Streamer->InitSections(false);

  // Output functions one by one.
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (!Function.isSimple())
      continue;

    if (!opts::shouldProcess(Function.getName()))
      continue;

    DEBUG(dbgs() << "FLO: generating code for function \""
                 << Function.getName() << "\"\n");

    // No need for human readability?
    // FIXME: what difference does it make in reality?
    //Ctx.setUseNamesOnTempLabels(false);

    // Emit function start

    // Each fuction is emmitted into its own section.
    MCSectionELF *FunctionSection =
      BC->Ctx->getELFSection(Function.getCodeSectionName(),
                             ELF::SHT_PROGBITS,
                             ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);

    MCSection *Section = FunctionSection;
    Streamer->SwitchSection(Section);

    Streamer->EmitCodeAlignment(Function.getAlignment());

    MCSymbol *FunctionSymbol = BC->Ctx->getOrCreateSymbol(Function.getName());
    Streamer->EmitSymbolAttribute(FunctionSymbol, MCSA_ELF_TypeFunction);
    Streamer->EmitLabel(FunctionSymbol);

    // Emit code.
    for (auto BB : Function.layout()) {
      if (BB->getAlignment() > 1)
        Streamer->EmitCodeAlignment(BB->getAlignment());
      Streamer->EmitLabel(BB->getLabel());
      for (const auto &Instr : *BB) {
        Streamer->EmitInstruction(Instr, *BC->STI);
      }
    }

    // TODO: is there any use in emiting end of function?
    //       Perhaps once we have a support for C++ exceptions.
    //auto FunctionEndLabel = Ctx.createTempSymbol("func_end");
    //Streamer->EmitLabel(FunctionEndLabel);
    //Streamer->emitELFSize(FunctionSymbol, MCExpr());
  }

  Streamer->Finish();

  // Get output object as ObjectFile.
  std::unique_ptr<MemoryBuffer> ObjectMemBuffer =
      MemoryBuffer::getMemBuffer(BOS->str(), "in-memory object file", false);
  ErrorOr<std::unique_ptr<object::ObjectFile>> ObjOrErr =
    object::ObjectFile::createObjectFile(ObjectMemBuffer->getMemBufferRef());
  check_error(ObjOrErr.getError(), "error creating in-memory object");

  std::unique_ptr<ExecutableFileMemoryManager>
    EFMM(new ExecutableFileMemoryManager());

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
        EFMM.get(),
        //std::move(EFMM),
        std::move(Resolver));
  //OLT.takeOwnershipOfBuffers(ObjectsHandle, );

  // Map every function/section current address in memory to that in
  // the output binary.
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;
    if (!Function.isSimple())
      continue;

    auto SAI = EFMM->SectionAddressInfo.find(Function.getCodeSectionName());
    if (SAI != EFMM->SectionAddressInfo.end()) {
      DEBUG(dbgs() << "FLO: mapping 0x" << Twine::utohexstr(SAI->second.first)
                   << " to 0x" << Twine::utohexstr(Function.getAddress())
                   << '\n');
      OLT.mapSectionAddress(ObjectsHandle,
          reinterpret_cast<const void*>(SAI->second.first),
          Function.getAddress());
      Function.setImageAddress(SAI->second.first);
      Function.setImageSize(SAI->second.second);
    } else {
      errs() << "FLO: cannot remap function " << Function.getName() << "\n";
    }
  }

  OLT.emitAndFinalize(ObjectsHandle);

  // FIXME: is there a less painful way to obtain assembler/writer?
  auto &Writer =
    static_cast<MCObjectStreamer*>(Streamer.get())->getAssembler().getWriter();
  Writer.setStream(RealOut->os());

  // Overwrite function in the output file.
  uint64_t CountOverwrittenFunctions = 0;
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
      continue;

    if (Function.getImageSize() > Function.getMaxSize()) {
      errs() << "FLO-WARNING: new function size (0x"
             << Twine::utohexstr(Function.getImageSize())
             << ") is larger than maximum allowed size (0x"
             << Twine::utohexstr(Function.getMaxSize())
             << ") for function " << Function.getName() << '\n';
      continue;
    }

    // Overwrite function in the output file.
    outs() << "FLO: rewriting function \"" << Function.getName() << "\"\n";
    RealOut->os().pwrite(
        reinterpret_cast<char *>(Function.getImageAddress()),
        Function.getImageSize(),
        Function.getFileOffset());

    // Write nops at the end of the function.
    auto Pos = RealOut->os().tell();
    RealOut->os().seek(Function.getFileOffset() + Function.getImageSize());
    BC->MAB->writeNopData(Function.getMaxSize() - Function.getImageSize(),
                          &Writer);
    RealOut->os().seek(Pos);

    ++CountOverwrittenFunctions;

    if (opts::MaxFunctions && CountOverwrittenFunctions == opts::MaxFunctions) {
      outs() << "FLO: maximum number of functions reached\n";
      break;
    }
  }

  if (EntryPointFunction) {
    DEBUG(dbgs() << "FLO: entry point function is "
                 << EntryPointFunction->getName() << '\n');
  } else {
    DEBUG(dbgs() << "FLO: no entry point function was set\n");
  }

  outs() << "FLO: " << CountOverwrittenFunctions
         << " out of " << BinaryFunctions.size()
         << " functions were overwritten.\n";
  // TODO: we should find a way to mark the binary as optimized by us.

  Out->keep();
  RealOut->keep();
}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  // Initialize targets and assembly printers/parsers.
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllDisassemblers();

  llvm::InitializeAllTargets();
  llvm::InitializeAllAsmPrinters();

  // Register the target printer for --version.
  cl::AddExtraVersionPrinter(TargetRegistry::printRegisteredTargetsForVersion);

  cl::ParseCommandLineOptions(argc, argv,
                              "llvm feedback-directed layout optimizer\n");

  ToolName = argv[0];

  if (!sys::fs::exists(opts::InputFilename))
    report_error(opts::InputFilename, errc::no_such_file_or_directory);

  std::unique_ptr<flo::DataReader> DR(new DataReader(errs()));
  if (!opts::InputDataFilename.empty()) {
    if (!sys::fs::exists(opts::InputDataFilename))
      report_error(opts::InputDataFilename, errc::no_such_file_or_directory);

    // Attempt to read input flo data
    auto ReaderOrErr =
      flo::DataReader::readPerfData(opts::InputDataFilename, errs());
    if (std::error_code EC = ReaderOrErr.getError())
      report_error(opts::InputDataFilename, EC);
    DR.reset(ReaderOrErr.get().release());
    if (opts::DumpData) {
      DR->dump();
      return EXIT_SUCCESS;
    }
  }

  // Attempt to open the binary.
  ErrorOr<OwningBinary<Binary>> BinaryOrErr = createBinary(opts::InputFilename);
  if (std::error_code EC = BinaryOrErr.getError())
    report_error(opts::InputFilename, EC);
  Binary &Binary = *BinaryOrErr.get().getBinary();

  if (ELFObjectFileBase *e = dyn_cast<ELFObjectFileBase>(&Binary)) {
    OptimizeFile(e, *DR.get());
  } else {
    report_error(opts::InputFilename, object_error::invalid_file_type);
  }

  return EXIT_SUCCESS;
}
