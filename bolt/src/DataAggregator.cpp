//===-- DataAggregator.cpp - Perf data aggregator ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions reads profile data written by perf record,
// aggregate it and then write it back to an output file.
//
//===----------------------------------------------------------------------===//

#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "BoltAddressTranslation.h"
#include "DataAggregator.h"
#include "ExecutableFileMemoryManager.h"
#include "Heatmap.h"
#include "Utils.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Options.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Timer.h"
#include <map>
#include <sstream>
#include <unordered_map>

#include <unistd.h>

#define DEBUG_TYPE "aggregator"

using namespace llvm;
using namespace bolt;

namespace opts {

extern cl::OptionCategory AggregatorCategory;
extern bool HeatmapMode;
extern bool LinuxKernelMode;
extern cl::SubCommand HeatmapCommand;
extern cl::opt<bool> AggregateOnly;
extern cl::opt<std::string> OutputFilename;

static cl::opt<bool>
BasicAggregation("nl",
  cl::desc("aggregate basic samples (without LBR info)"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

static cl::opt<bool>
FilterMemProfile("filter-mem-profile",
  cl::desc("if processing a memory profile, filter out stack or heap accesses "
           "that won't be useful for BOLT to reduce profile file size"),
  cl::init(true),
  cl::cat(AggregatorCategory));

static cl::opt<unsigned long long>
FilterPID("pid",
  cl::desc("only use samples from process with specified PID"),
  cl::init(0),
  cl::Optional,
  cl::cat(AggregatorCategory));

static cl::opt<unsigned>
HeatmapBlock("block-size",
  cl::desc("size of a heat map block in bytes (default 64)"),
  cl::init(64),
  cl::sub(HeatmapCommand));

static cl::opt<std::string>
HeatmapFile("o",
  cl::init("-"),
  cl::desc("heatmap output file (default stdout)"),
  cl::Optional,
  cl::sub(HeatmapCommand));

static cl::opt<unsigned long long>
HeatmapMaxAddress("max-address",
  cl::init(0xffffffff),
  cl::desc("maximum address considered valid for heatmap (default 4GB)"),
  cl::Optional,
  cl::sub(HeatmapCommand));

static cl::opt<unsigned long long>
HeatmapMinAddress("min-address",
  cl::init(0x0),
  cl::desc("minimum address considered valid for heatmap (default 0)"),
  cl::Optional,
  cl::sub(HeatmapCommand));

static cl::opt<bool>
IgnoreBuildID("ignore-build-id",
  cl::desc("continue even if build-ids in input binary and perf.data mismatch"),
  cl::init(false),
  cl::cat(AggregatorCategory));

static cl::opt<bool>
IgnoreInterruptLBR("ignore-interrupt-lbr",
  cl::desc("ignore kernel interrupt LBR that happens asynchronously"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

static cl::opt<unsigned long long>
MaxSamples("max-samples",
  cl::init(-1ULL),
  cl::desc("maximum number of samples to read from LBR profile"),
  cl::Optional,
  cl::Hidden,
  cl::cat(AggregatorCategory));

static cl::opt<bool>
ReadPreAggregated("pa",
  cl::desc("skip perf and read data from a pre-aggregated file format"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

static cl::opt<bool>
TimeAggregator("time-aggr",
  cl::desc("time BOLT aggregator"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

static cl::opt<bool>
UseEventPC("use-event-pc",
  cl::desc("use event PC in combination with LBR sampling"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

static cl::opt<bool>
WriteAutoFDOData("autofdo",
  cl::desc("generate autofdo textual data instead of bolt data"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(AggregatorCategory));

}

namespace {

const char TimerGroupName[] = "aggregator";
const char TimerGroupDesc[] = "Aggregator";

}

DataAggregator::~DataAggregator() {
  deleteTempFiles();
}

namespace {
void deleteTempFile(const std::string &FileName) {
  if (auto Errc = sys::fs::remove(FileName.c_str())) {
    errs() << "PERF2BOLT: failed to delete temporary file "
           << FileName << " with error " << Errc.message() << "\n";
  }
}
}

void DataAggregator::deleteTempFiles() {
  for (auto &FileName : TempFiles) {
    deleteTempFile(FileName);
  }
  TempFiles.clear();
}

void DataAggregator::findPerfExecutable() {
  auto PerfExecutable = sys::Process::FindInEnvPath("PATH", "perf");
  if (!PerfExecutable) {
    outs() << "PERF2BOLT: No perf executable found!\n";
    exit(1);
  }
  PerfPath = *PerfExecutable;
}

void DataAggregator::start() {
  outs() << "PERF2BOLT: Starting data aggregation job for " << Filename
         << "\n";

  // Don't launch perf for pre-aggregated files
  if (opts::ReadPreAggregated)
    return;

  findPerfExecutable();

  if (opts::BasicAggregation) {
    launchPerfProcess("events without LBR",
                      MainEventsPPI,
                      "script -F pid,event,ip",
                      /*Wait = */false);
  } else {
    launchPerfProcess("branch events",
                      MainEventsPPI,
                      "script -F pid,ip,brstack",
                      /*Wait = */false);
  }

  // Note: we launch script for mem events regardless of the option, as the
  //       command fails fairly fast if mem events were not collected.
  launchPerfProcess("mem events",
                    MemEventsPPI,
                    "script -F pid,event,addr,ip",
                    /*Wait = */false);

  launchPerfProcess("process events",
                    MMapEventsPPI,
                    "script --show-mmap-events",
                    /*Wait = */false);

  launchPerfProcess("task events",
                    TaskEventsPPI,
                    "script --show-task-events",
                    /*Wait = */false);
}

void DataAggregator::abort() {
  if (opts::ReadPreAggregated)
    return;

  std::string Error;

  // Kill subprocesses in case they are not finished
  sys::Wait(TaskEventsPPI.PI, 1, false, &Error);
  sys::Wait(MMapEventsPPI.PI, 1, false, &Error);
  sys::Wait(MainEventsPPI.PI, 1, false, &Error);
  sys::Wait(MemEventsPPI.PI, 1, false, &Error);

  deleteTempFiles();

  exit(1);
}

void DataAggregator::launchPerfProcess(StringRef Name, PerfProcessInfo &PPI,
                                       const char *ArgsString, bool Wait) {
  SmallVector<const char*, 4> Argv;

  outs() << "PERF2BOLT: spawning perf job to read " << Name << '\n';
  Argv.push_back(PerfPath.data());

  auto *WritableArgsString = strdup(ArgsString);
  auto *Str = WritableArgsString;
  do {
    Argv.push_back(Str);
    while (*Str && *Str != ' ')
      ++Str;
    if (!*Str)
      break;
    *Str++ = 0;
  } while (true);

  Argv.push_back("-f");
  Argv.push_back("-i");
  Argv.push_back(Filename.c_str());
  Argv.push_back(nullptr);

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "out",
                                               PPI.StdoutPath)) {
    errs() << "PERF2BOLT: failed to create temporary file "
           << PPI.StdoutPath << " with error " << Errc.message()
           << "\n";
    exit(1);
  }
  TempFiles.push_back(PPI.StdoutPath.data());

  if (auto Errc = sys::fs::createTemporaryFile("perf.script", "err",
                                               PPI.StderrPath)) {
    errs() << "PERF2BOLT: failed to create temporary file "
           << PPI.StderrPath << " with error " << Errc.message() << "\n";
    exit(1);
  }
  TempFiles.push_back(PPI.StderrPath.data());

  Optional<StringRef> Redirects[] = {
      llvm::None,                        // Stdin
      StringRef(PPI.StdoutPath.data()),  // Stdout
      StringRef(PPI.StderrPath.data())}; // Stderr

  DEBUG({
      dbgs() << "Launching perf: ";
      for (const char *Arg : Argv)
        dbgs() << Arg << " ";
      dbgs() << " 1> "
             << PPI.StdoutPath.data() << " 2> "
             << PPI.StderrPath.data() << "\n";
    });

  if (Wait) {
    PPI.PI.ReturnCode =
      sys::ExecuteAndWait(PerfPath.data(), Argv.data(), /*envp*/ nullptr,
                          Redirects);
  } else {
    PPI.PI = sys::ExecuteNoWait(PerfPath.data(), Argv.data(), /*envp*/ nullptr,
                                Redirects);
  }

  free(WritableArgsString);
}

void DataAggregator::processFileBuildID(StringRef FileBuildID) {
  PerfProcessInfo BuildIDProcessInfo;
  launchPerfProcess("buildid list",
                    BuildIDProcessInfo,
                    "buildid-list",
                    /*Wait = */true);

  if (BuildIDProcessInfo.PI.ReturnCode != 0) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(BuildIDProcessInfo.StderrPath.data());
    StringRef ErrBuf = (*MB)->getBuffer();

    errs() << "PERF-ERROR: return code " << BuildIDProcessInfo.PI.ReturnCode
           << '\n';
    errs() << ErrBuf;
    return;
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
    MemoryBuffer::getFileOrSTDIN(BuildIDProcessInfo.StdoutPath.data());
  if (std::error_code EC = MB.getError()) {
    errs() << "Cannot open " << BuildIDProcessInfo.StdoutPath.data() << ": "
           << EC.message() << "\n";
    return;
  }

  FileBuf.reset(MB->release());
  ParsingBuf = FileBuf->getBuffer();
  if (ParsingBuf.empty()) {
    errs() << "PERF2BOLT-WARNING: build-id will not be checked because perf "
              "data was recorded without it\n";
    return;
  }

  Col = 0;
  Line = 1;
  auto FileName = getFileNameForBuildID(FileBuildID);
  if (!FileName) {
    errs() << "PERF2BOLT-ERROR: failed to match build-id from perf output. "
              "This indicates the input binary supplied for data aggregation "
              "is not the same recorded by perf when collecting profiling "
              "data, or there were no samples recorded for the binary. "
              "Use -ignore-build-id option to override.\n";
    if (!opts::IgnoreBuildID) {
      abort();
    }
  } else if (*FileName != llvm::sys::path::filename(BC->getFilename())) {
    errs() << "PERF2BOLT-WARNING: build-id matched a different file name\n";
    BuildIDBinaryName = *FileName;
  } else {
    outs() << "PERF2BOLT: matched build-id and file name\n";
  }

  return;
}

bool DataAggregator::checkPerfDataMagic(StringRef FileName) {
  if (opts::ReadPreAggregated)
    return true;

  int FD;
  if (sys::fs::openFileForRead(FileName, FD)) {
    return false;
  }

  char Buf[7] = {0, 0, 0, 0, 0, 0, 0};

  if (::read(FD, Buf, 7) == -1) {
    ::close(FD);
    return false;
  }
  ::close(FD);

  if (strncmp(Buf, "PERFILE", 7) == 0)
    return true;
  return false;
}

void DataAggregator::parsePreAggregated() {
  std::string Error;

  auto MB = MemoryBuffer::getFileOrSTDIN(Filename);
  if (std::error_code EC = MB.getError()) {
    errs() << "PERF2BOLT-ERROR: cannot open " << Filename << ": "
           << EC.message() << "\n";
    exit(1);
  }

  FileBuf.reset(MB->release());
  ParsingBuf = FileBuf->getBuffer();
  Col = 0;
  Line = 1;
  if (parsePreAggregatedLBRSamples()) {
    errs() << "PERF2BOLT: failed to parse samples\n";
    exit(1);
  }
}

std::error_code DataAggregator::writeAutoFDOData(StringRef OutputFilename) {
  outs() << "PERF2BOLT: writing data for autofdo tools...\n";
  NamedRegionTimer T("writeAutoFDO", "Processing branch events",
                     TimerGroupName, TimerGroupDesc, opts::TimeAggregator);

  std::error_code EC;
  raw_fd_ostream OutFile(OutputFilename, EC, sys::fs::OpenFlags::F_None);
  if (EC)
    return EC;

  // Format:
  // number of unique traces
  // from_1-to_1:count_1
  // from_2-to_2:count_2
  // ......
  // from_n-to_n:count_n
  // number of unique sample addresses
  // addr_1:count_1
  // addr_2:count_2
  // ......
  // addr_n:count_n
  // number of unique LBR entries
  // src_1->dst_1:count_1
  // src_2->dst_2:count_2
  // ......
  // src_n->dst_n:count_n

  const uint64_t FirstAllocAddress = this->BC->FirstAllocAddress;

  // AutoFDO addresses are relative to the first allocated loadable program
  // segment
  auto filterAddress = [&FirstAllocAddress](uint64_t Address) -> uint64_t {
    if (Address < FirstAllocAddress)
      return 0;
    return Address - FirstAllocAddress;
  };

  OutFile << FallthroughLBRs.size() << "\n";
  for (const auto &AggrLBR : FallthroughLBRs) {
    auto &Trace = AggrLBR.first;
    auto &Info = AggrLBR.second;
    OutFile << Twine::utohexstr(filterAddress(Trace.From)) << "-"
            << Twine::utohexstr(filterAddress(Trace.To)) << ":"
            << (Info.InternCount + Info.ExternCount) << "\n";
  }

  OutFile << BasicSamples.size() << "\n";
  for (const auto &Sample : BasicSamples) {
    auto PC = Sample.first;
    auto HitCount = Sample.second;
    OutFile << Twine::utohexstr(filterAddress(PC)) << ":" << HitCount << "\n";
  }

  OutFile << BranchLBRs.size() << "\n";
  for (const auto &AggrLBR : BranchLBRs) {
    auto &Trace = AggrLBR.first;
    auto &Info = AggrLBR.second;
    OutFile << Twine::utohexstr(filterAddress(Trace.From)) << "->"
            << Twine::utohexstr(filterAddress(Trace.To)) << ":"
            << Info.TakenCount << "\n";
  }

  outs() << "PERF2BOLT: wrote " << FallthroughLBRs.size() << " unique traces, "
         << BasicSamples.size() << " sample addresses and " << BranchLBRs.size()
         << " unique branches to " << OutputFilename << "\n";

  return std::error_code();
}

void DataAggregator::filterBinaryMMapInfo() {
  if (opts::FilterPID) {
    auto MMapInfoIter = BinaryMMapInfo.find(opts::FilterPID);
    if (MMapInfoIter != BinaryMMapInfo.end()) {
      auto MMap = MMapInfoIter->second;
      BinaryMMapInfo.clear();
      BinaryMMapInfo.insert(std::make_pair(MMap.PID, MMap));
    } else {
      if (errs().has_colors())
        errs().changeColor(raw_ostream::RED);
      errs() << "PERF2BOLT-ERROR: could not find a profile matching PID \""
             << opts::FilterPID << "\"" << " for binary \""
             << BC->getFilename() <<"\".";
      assert(!BinaryMMapInfo.empty() && "No memory map for matching binary");
      errs() << " Profile for the following process is available:\n";
      for (auto &MMI : BinaryMMapInfo) {
        outs() << "  " << MMI.second.PID
               << (MMI.second.Forked ? " (forked)\n" : "\n");
      }
      if (errs().has_colors())
        errs().resetColor();

      exit(1);
    }
  }
}

Error DataAggregator::preprocessProfile(BinaryContext &BC) {
  this->BC = &BC;

  if (opts::ReadPreAggregated) {
    parsePreAggregated();
    return Error::success();
  }

  if (auto FileBuildID = BC.getFileBuildID()) {
    outs() << "BOLT-INFO: binary build-id is:     " << *FileBuildID << "\n";
    processFileBuildID(*FileBuildID);
  } else {
    errs() << "BOLT-WARNING: build-id will not be checked because we could "
              "not read one from input binary\n";
  }

  auto prepareToParse = [&] (StringRef Name, PerfProcessInfo &Process) {
    std::string Error;
    outs() << "PERF2BOLT: waiting for perf " << Name
           << " collection to finish...\n";
    auto PI = sys::Wait(Process.PI, 0, true, &Error);

    if (!Error.empty()) {
      errs() << "PERF-ERROR: " << PerfPath << ": " << Error << "\n";
      deleteTempFiles();
      exit(1);
    }

    if (PI.ReturnCode != 0) {
      ErrorOr<std::unique_ptr<MemoryBuffer>> ErrorMB =
        MemoryBuffer::getFileOrSTDIN(Process.StderrPath.data());
      StringRef ErrBuf = (*ErrorMB)->getBuffer();

      errs() << "PERF-ERROR: return code " << PI.ReturnCode << "\n";
      errs() << ErrBuf;
      deleteTempFiles();
      exit(1);
    }

    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(Process.StdoutPath.data());
    if (std::error_code EC = MB.getError()) {
      errs() << "Cannot open " << Process.StdoutPath.data() << ": "
             << EC.message() << "\n";
      deleteTempFiles();
      exit(1);
    }

    FileBuf.reset(MB->release());
    ParsingBuf = FileBuf->getBuffer();
    Col = 0;
    Line = 1;
  };

  if (opts::LinuxKernelMode) {
    // Current MMap parsing logic does not work with linux kernel.
    // MMap entries for linux kernel uses PERF_RECORD_MMAP
    // format instead of typical PERF_RECORD_MMAP2 format.
    // Since linux kernel address mapping is absolute (same as
    // in the ELF file), we avoid parsing MMap in linux kernel mode.
    // While generating optimized linux kernel binary, we may need
    // to parse MMap entries.

    // In linux kernel mode, we analyze and optimize
    // all linux kernel binary instructions, irrespective
    // of whether they are due to system calls or due to
    // interrupts. Therefore, we cannot ignore interrupt
    // in Linux kernel mode.
    opts::IgnoreInterruptLBR = false;
  } else {
    prepareToParse("mmap events", MMapEventsPPI);
    if (parseMMapEvents()) {
      errs() << "PERF2BOLT: failed to parse mmap events\n";
    }
  }

  prepareToParse("task events", TaskEventsPPI);
  if (parseTaskEvents()) {
    errs() << "PERF2BOLT: failed to parse task events\n";
  }

  filterBinaryMMapInfo();
  prepareToParse("events", MainEventsPPI);

  if (opts::HeatmapMode) {
    if (auto EC = printLBRHeatMap()) {
      errs() << "ERROR: failed to print heat map: " << EC.message() << '\n';
      exit(1);
    }
    exit(0);
  }

  if ((!opts::BasicAggregation && parseBranchEvents()) ||
      (opts::BasicAggregation && parseBasicEvents())) {
    errs() << "PERF2BOLT: failed to parse samples\n";
  }

  // We can finish early if the goal is just to generate data for autofdo
  if (opts::WriteAutoFDOData) {
    if (std::error_code EC = writeAutoFDOData(opts::OutputFilename)) {
      errs() << "Error writing autofdo data to file: " << EC.message() << "\n";
    }
    deleteTempFiles();
    exit(0);
  }

  // Special handling for memory events
  std::string Error;
  auto PI = sys::Wait(MemEventsPPI.PI, 0, true, &Error);
  if (PI.ReturnCode != 0) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
      MemoryBuffer::getFileOrSTDIN(MemEventsPPI.StderrPath.data());
    StringRef ErrBuf = (*MB)->getBuffer();

    deleteTempFiles();

    Regex NoData("Samples for '.*' event do not have ADDR attribute set. "
                 "Cannot print 'addr' field.");
    if (!NoData.match(ErrBuf)) {
      errs() << "PERF-ERROR: return code " << PI.ReturnCode << "\n";
      errs() << ErrBuf;
      exit(1);
    }
    return Error::success();
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MB =
    MemoryBuffer::getFileOrSTDIN(MemEventsPPI.StdoutPath.data());
  if (std::error_code EC = MB.getError()) {
    errs() << "Cannot open " << MemEventsPPI.StdoutPath.data() << ": "
           << EC.message() << "\n";
    deleteTempFiles();
    exit(1);
  }

  FileBuf.reset(MB->release());
  ParsingBuf = FileBuf->getBuffer();
  Col = 0;
  Line = 1;
  if (const auto EC = parseMemEvents()) {
    errs() << "PERF2BOLT: failed to parse memory events: "
           << EC.message() << '\n';
  }

  deleteTempFiles();

  return Error::success();
}

Error DataAggregator::readProfile(BinaryContext &BC) {
  processProfile(BC);

  for (auto &BFI : BC.getBinaryFunctions()) {
    auto &Function = BFI.second;
    convertBranchData(Function);
  }

  if (opts::AggregateOnly) {
    if (auto EC = writeAggregatedFile(opts::OutputFilename)) {
      report_error("cannot create output data file", EC);
    }
  }

  return Error::success();
}

bool DataAggregator::mayHaveProfileData(const BinaryFunction &Function) {
  return Function.hasProfileAvailable();
}

void DataAggregator::processProfile(BinaryContext &BC) {
  if (opts::ReadPreAggregated)
    processPreAggregated();
  else if (opts::BasicAggregation)
    processBasicEvents();
  else
    processBranchEvents();

  processMemEvents();

  // Mark all functions with registered events as having a valid profile.
  for (auto &BFI : BC.getBinaryFunctions()) {
    auto &BF = BFI.second;
    if (getBranchData(BF)) {
      const auto Flags = opts::BasicAggregation ? BinaryFunction::PF_SAMPLE
                                                : BinaryFunction::PF_LBR;
      BF.markProfiled(Flags);
    }
  }

  // Release intermediate storage.
  clear(BranchLBRs);
  clear(FallthroughLBRs);
  clear(AggregatedLBRs);
  clear(BasicSamples);
  clear(MemSamples);
}

BinaryFunction *
DataAggregator::getBinaryFunctionContainingAddress(uint64_t Address) const {
  if (!BC->containsAddress(Address))
    return nullptr;

  // Use shallow search to avoid fetching the parent function, in case
  // BinaryContext linked two functions. When aggregating data and writing the
  // profile, we want to write offsets relative to the closest symbol in the
  // symbol table, not relative to the parent function, to avoid creating
  // profile that is too fragile and depends on the layout of other functions.
  return BC->getBinaryFunctionContainingAddress(Address, /*CheckPastEnd=*/false,
                                                /*UseMaxSize=*/true,
                                                /*Shallow=*/true);
}

StringRef DataAggregator::getLocationName(BinaryFunction &Func,
                                          uint64_t Count) {
  if (!BAT)
    return Func.getOneName();

  const auto *OrigFunc = &Func;
  if (const auto HotAddr = BAT->fetchParentAddress(Func.getAddress())) {
    NumColdSamples += Count;
    auto *HotFunc = getBinaryFunctionContainingAddress(HotAddr);
    if (HotFunc)
      OrigFunc = HotFunc;
  }
  // If it is a local function, prefer the name containing the file name where
  // the local function was declared
  for (auto AlternativeName : OrigFunc->getNames()) {
    size_t FileNameIdx = AlternativeName.find('/');
    // Confirm the alternative name has the pattern Symbol/FileName/1 before
    // using it
    if (FileNameIdx == StringRef::npos ||
        AlternativeName.find('/', FileNameIdx + 1) == StringRef::npos)
      continue;
    return AlternativeName;
  }
  return Func.getOneName();
}

bool DataAggregator::doSample(BinaryFunction &Func, uint64_t Address,
                              uint64_t Count) {
  auto I = NamesToSamples.find(Func.getOneName());
  if (I == NamesToSamples.end()) {
    bool Success;
    StringRef LocName = getLocationName(Func, Count);
    std::tie(I, Success) = NamesToSamples.insert(std::make_pair(
        Func.getOneName(),
        FuncSampleData(LocName, FuncSampleData::ContainerTy())));
  }

  Address -= Func.getAddress();
  if (BAT)
    Address = BAT->translate(Func, Address, /*IsBranchSrc=*/false);

  I->second.bumpCount(Address, Count);
  return true;
}

bool DataAggregator::doIntraBranch(BinaryFunction &Func, uint64_t From,
                                   uint64_t To, uint64_t Count,
                                   uint64_t Mispreds) {
  FuncBranchData *AggrData = getBranchData(Func);
  if (!AggrData) {
    AggrData = &NamesToBranches[Func.getOneName()];
    AggrData->Name = getLocationName(Func, Count);
    setBranchData(Func, AggrData);
  }

  From -= Func.getAddress();
  To -= Func.getAddress();
  DEBUG(dbgs() << "BOLT-DEBUG: bumpBranchCount: " << Func.getPrintName()
               << " @ " << Twine::utohexstr(From) << " -> "
               << Func.getPrintName() << " @ " << Twine::utohexstr(To)
               << '\n');
  if (BAT) {
    From = BAT->translate(Func, From, /*IsBranchSrc=*/true);
    To = BAT->translate(Func, To, /*IsBranchSrc=*/false);
    DEBUG(dbgs() << "BOLT-DEBUG: BAT translation on bumpBranchCount: "
                 << Func.getPrintName() << " @ " << Twine::utohexstr(From)
                 << " -> " << Func.getPrintName() << " @ "
                 << Twine::utohexstr(To) << '\n');
  }

  AggrData->bumpBranchCount(From, To, Count, Mispreds);
  return true;
}

bool DataAggregator::doInterBranch(BinaryFunction *FromFunc,
                                   BinaryFunction *ToFunc, uint64_t From,
                                   uint64_t To, uint64_t Count,
                                   uint64_t Mispreds) {
  FuncBranchData *FromAggrData{nullptr};
  FuncBranchData *ToAggrData{nullptr};
  StringRef SrcFunc;
  StringRef DstFunc;
  if (FromFunc) {
    SrcFunc = getLocationName(*FromFunc, Count);
    FromAggrData = getBranchData(*FromFunc);
    if (!FromAggrData) {
      FromAggrData = &NamesToBranches[FromFunc->getOneName()];
      FromAggrData->Name = SrcFunc;
      setBranchData(*FromFunc, FromAggrData);
    }
    From -= FromFunc->getAddress();
    if (BAT)
      From = BAT->translate(*FromFunc, From, /*IsBranchSrc=*/true);

    recordExit(*FromFunc, From, Mispreds, Count);
  }
  if (ToFunc) {
    DstFunc = getLocationName(*ToFunc, 0);
    ToAggrData = getBranchData(*ToFunc);
    if (!ToAggrData) {
      ToAggrData = &NamesToBranches[ToFunc->getOneName()];
      ToAggrData->Name = DstFunc;
      setBranchData(*ToFunc, ToAggrData);
    }
    To -= ToFunc->getAddress();
    if (BAT)
      To = BAT->translate(*ToFunc, To, /*IsBranchSrc=*/false);

    recordEntry(*ToFunc, To, Mispreds, Count);
  }

  if (FromAggrData)
    FromAggrData->bumpCallCount(From, Location(!DstFunc.empty(), DstFunc, To),
                                Count, Mispreds);
  if (ToAggrData)
    ToAggrData->bumpEntryCount(Location(!SrcFunc.empty(), SrcFunc, From), To,
                               Count, Mispreds);
  return true;
}

bool DataAggregator::doBranch(uint64_t From, uint64_t To, uint64_t Count,
                              uint64_t Mispreds) {
  auto *FromFunc = getBinaryFunctionContainingAddress(From);
  auto *ToFunc = getBinaryFunctionContainingAddress(To);
  if (!FromFunc && !ToFunc)
    return false;

  if (FromFunc == ToFunc) {
    recordBranch(*FromFunc, From - FromFunc->getAddress(),
                 To - FromFunc->getAddress(), Count, Mispreds);
    return doIntraBranch(*FromFunc, From, To, Count, Mispreds);
  }

  return doInterBranch(FromFunc, ToFunc, From, To, Count, Mispreds);
}

bool DataAggregator::doTrace(const LBREntry &First, const LBREntry &Second,
                             uint64_t Count) {
  auto *FromFunc = getBinaryFunctionContainingAddress(First.To);
  auto *ToFunc = getBinaryFunctionContainingAddress(Second.From);
  if (!FromFunc || !ToFunc) {
    DEBUG(
        dbgs() << "Out of range trace starting in " << FromFunc->getPrintName()
               << " @ " << Twine::utohexstr(First.To - FromFunc->getAddress())
               << " and ending in " << ToFunc->getPrintName() << " @ "
               << ToFunc->getPrintName() << " @ "
               << Twine::utohexstr(Second.From - ToFunc->getAddress()) << '\n');
    NumLongRangeTraces += Count;
    return false;
  }
  if (FromFunc != ToFunc) {
    NumInvalidTraces += Count;
    DEBUG(dbgs() << "Invalid trace starting in " << FromFunc->getPrintName()
                 << " @ " << Twine::utohexstr(First.To - FromFunc->getAddress())
                 << " and ending in " << ToFunc->getPrintName() << " @ "
                 << ToFunc->getPrintName() << " @ "
                 << Twine::utohexstr(Second.From - ToFunc->getAddress())
                 << '\n');
    return false;
  }

  auto FTs = BAT ? BAT->getFallthroughsInTrace(*FromFunc, First.To, Second.From)
                 : getFallthroughsInTrace(*FromFunc, First, Second, Count);
  if (!FTs) {
    DEBUG(dbgs() << "Invalid trace starting in " << FromFunc->getPrintName()
                 << " @ " << Twine::utohexstr(First.To - FromFunc->getAddress())
                 << " and ending in " << ToFunc->getPrintName() << " @ "
                 << ToFunc->getPrintName() << " @ "
                 << Twine::utohexstr(Second.From - ToFunc->getAddress())
                 << '\n');
    NumInvalidTraces += Count;
    return false;
  }

  DEBUG(dbgs() << "Processing " << FTs->size() << " fallthroughs for "
               << FromFunc->getPrintName() << ":" << Twine::utohexstr(First.To)
               << " to " << Twine::utohexstr(Second.From) << ".\n");
  for (const auto &Pair : *FTs) {
    doIntraBranch(*FromFunc, Pair.first + FromFunc->getAddress(),
                  Pair.second + FromFunc->getAddress(), Count, false);
  }

  return true;
}

bool DataAggregator::recordTrace(
    BinaryFunction &BF,
    const LBREntry &FirstLBR,
    const LBREntry &SecondLBR,
    uint64_t Count,
    SmallVector<std::pair<uint64_t, uint64_t>, 16> *Branches) const {
  auto &BC = BF.getBinaryContext();

  if (!BF.isSimple())
    return false;

  assert(BF.hasCFG() && "can only record traces in CFG state");

  // Offsets of the trace within this function.
  const auto From = FirstLBR.To - BF.getAddress();
  const auto To = SecondLBR.From - BF.getAddress();

  if (From > To)
    return false;

  auto *FromBB = BF.getBasicBlockContainingOffset(From);
  auto *ToBB = BF.getBasicBlockContainingOffset(To);

  if (!FromBB || !ToBB)
    return false;

  // Adjust FromBB if the first LBR is a return from the last instruction in
  // the previous block (that instruction should be a call).
  if (From == FromBB->getOffset() && !BF.containsAddress(FirstLBR.From) &&
      !FromBB->isEntryPoint() && !FromBB->isLandingPad()) {
    auto *PrevBB = BF.BasicBlocksLayout[FromBB->getIndex() - 1];
    if (PrevBB->getSuccessor(FromBB->getLabel())) {
      const auto *Instr = PrevBB->getLastNonPseudoInstr();
      if (Instr && BC.MIB->isCall(*Instr)) {
        FromBB = PrevBB;
      } else {
        DEBUG(dbgs() << "invalid incoming LBR (no call): " << FirstLBR << '\n');
      }
    } else {
      DEBUG(dbgs() << "invalid incoming LBR: " << FirstLBR << '\n');
    }
  }

  // Fill out information for fall-through edges. The From and To could be
  // within the same basic block, e.g. when two call instructions are in the
  // same block. In this case we skip the processing.
  if (FromBB == ToBB) {
    return true;
  }

  // Process blocks in the original layout order.
  auto *BB = BF.BasicBlocksLayout[FromBB->getIndex()];
  assert(BB == FromBB && "index mismatch");
  while (BB != ToBB) {
    auto *NextBB = BF.BasicBlocksLayout[BB->getIndex() + 1];
    assert((NextBB && NextBB->getOffset() > BB->getOffset()) && "bad layout");

    // Check for bad LBRs.
    if (!BB->getSuccessor(NextBB->getLabel())) {
      DEBUG(dbgs() << "no fall-through for the trace:\n"
                   << "  " << FirstLBR << '\n'
                   << "  " << SecondLBR << '\n');
      return false;
    }

   // Record fall-through jumps
    auto &BI = BB->getBranchInfo(*NextBB);
    BI.Count += Count;

    if (Branches) {
      const auto *Instr = BB->getLastNonPseudoInstr();
      uint64_t Offset{0};
      if (Instr) {
        Offset = BC.MIB->getAnnotationWithDefault<uint32_t>(*Instr, "Offset");
      } else {
        Offset = BB->getOffset();
      }
      Branches->emplace_back(std::make_pair(Offset, NextBB->getOffset()));
    }

    BB = NextBB;
  }

  return true;
}

Optional<SmallVector<std::pair<uint64_t, uint64_t>, 16>>
DataAggregator::getFallthroughsInTrace(BinaryFunction &BF,
                                       const LBREntry &FirstLBR,
                                       const LBREntry &SecondLBR,
                                       uint64_t Count) const {
  SmallVector<std::pair<uint64_t, uint64_t>, 16> Res;

  if (!recordTrace(BF, FirstLBR, SecondLBR, Count, &Res))
    return NoneType();

  return Res;
}

bool DataAggregator::recordEntry(BinaryFunction &BF, uint64_t To, bool Mispred,
                                 uint64_t Count) const {
  if (To > BF.getSize())
    return false;

  if (!BF.hasProfile())
    BF.ExecutionCount = 0;

  BinaryBasicBlock *EntryBB = nullptr;
  if (To == 0) {
    BF.ExecutionCount += Count;
    if (!BF.empty())
      EntryBB = &BF.front();
  } else if (auto *BB = BF.getBasicBlockAtOffset(To)) {
    if (BB->isEntryPoint())
      EntryBB = BB;
  }

  if (EntryBB)
    EntryBB->setExecutionCount(EntryBB->getKnownExecutionCount() + Count);

  return true;
}

bool DataAggregator::recordExit(BinaryFunction &BF, uint64_t From,
                                bool Mispred, uint64_t Count) const {
  if (!BF.isSimple() || From > BF.getSize())
    return false;

  if (!BF.hasProfile())
    BF.ExecutionCount = 0;

  return true;
}

ErrorOr<LBREntry> DataAggregator::parseLBREntry() {
  LBREntry Res;
  auto FromStrRes = parseString('/');
  if (std::error_code EC = FromStrRes.getError())
    return EC;
  StringRef OffsetStr = FromStrRes.get();
  if (OffsetStr.getAsInteger(0, Res.From)) {
    reportError("expected hexadecimal number with From address");
    Diag << "Found: " << OffsetStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }

  auto ToStrRes = parseString('/');
  if (std::error_code EC = ToStrRes.getError())
    return EC;
  OffsetStr = ToStrRes.get();
  if (OffsetStr.getAsInteger(0, Res.To)) {
    reportError("expected hexadecimal number with To address");
    Diag << "Found: " << OffsetStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }

  auto MispredStrRes = parseString('/');
  if (std::error_code EC = MispredStrRes.getError())
    return EC;
  StringRef MispredStr = MispredStrRes.get();
  if (MispredStr.size() != 1 ||
      (MispredStr[0] != 'P' && MispredStr[0] != 'M' && MispredStr[0] != '-')) {
    reportError("expected single char for mispred bit");
    Diag << "Found: " << MispredStr << "\n";
    return make_error_code(llvm::errc::io_error);
  }
  Res.Mispred = MispredStr[0] == 'M';

  static bool MispredWarning = true;;
  if (MispredStr[0] == '-' && MispredWarning) {
    errs() << "PERF2BOLT-WARNING: misprediction bit is missing in profile\n";
    MispredWarning = false;
  }

  auto Rest = parseString(FieldSeparator, true);
  if (std::error_code EC = Rest.getError())
    return EC;
  if (Rest.get().size() < 5) {
    reportError("expected rest of LBR entry");
    Diag << "Found: " << Rest.get() << "\n";
    return make_error_code(llvm::errc::io_error);
  }
  return Res;
}

bool DataAggregator::checkAndConsumeFS() {
  if (ParsingBuf[0] != FieldSeparator) {
    return false;
  }
  ParsingBuf = ParsingBuf.drop_front(1);
  Col += 1;
  return true;
}

void DataAggregator::consumeRestOfLine() {
  auto LineEnd = ParsingBuf.find_first_of('\n');
  if (LineEnd == StringRef::npos) {
    ParsingBuf = StringRef();
    Col = 0;
    Line += 1;
    return;
  }
  ParsingBuf = ParsingBuf.drop_front(LineEnd + 1);
  Col = 0;
  Line += 1;
}

ErrorOr<DataAggregator::PerfBranchSample> DataAggregator::parseBranchSample() {
  PerfBranchSample Res;

  while (checkAndConsumeFS()) {}

  auto PIDRes = parseNumberField(FieldSeparator, true);
  if (std::error_code EC = PIDRes.getError())
    return EC;
  auto MMapInfoIter = BinaryMMapInfo.find(*PIDRes);
  if (!opts::LinuxKernelMode && MMapInfoIter == BinaryMMapInfo.end()) {
    consumeRestOfLine();
    return make_error_code(errc::no_such_process);
  }

  while (checkAndConsumeFS()) {}

  auto PCRes = parseHexField(FieldSeparator, true);
  if (std::error_code EC = PCRes.getError())
    return EC;
  Res.PC = PCRes.get();

  if (checkAndConsumeNewLine())
    return Res;

  while (!checkAndConsumeNewLine()) {
    checkAndConsumeFS();

    auto LBRRes = parseLBREntry();
    if (std::error_code EC = LBRRes.getError())
      return EC;
    auto LBR = LBRRes.get();
    if (ignoreKernelInterrupt(LBR))
      continue;
    if (!BC->HasFixedLoadAddress)
      adjustLBR(LBR, MMapInfoIter->second);
    Res.LBR.push_back(LBR);
  }

  return Res;
}

ErrorOr<DataAggregator::PerfBasicSample> DataAggregator::parseBasicSample() {
  while (checkAndConsumeFS()) {}

  auto PIDRes = parseNumberField(FieldSeparator, true);
  if (std::error_code EC = PIDRes.getError())
    return EC;

  auto MMapInfoIter = BinaryMMapInfo.find(*PIDRes);
  if (MMapInfoIter == BinaryMMapInfo.end()) {
    consumeRestOfLine();
    return PerfBasicSample{StringRef(), 0};
  }

  while (checkAndConsumeFS()) {}

  auto Event = parseString(FieldSeparator);
  if (std::error_code EC = Event.getError())
    return EC;

  while (checkAndConsumeFS()) {}

  auto AddrRes = parseHexField(FieldSeparator, true);
  if (std::error_code EC = AddrRes.getError()) {
    return EC;
  }

  if (!checkAndConsumeNewLine()) {
    reportError("expected end of line");
    return make_error_code(llvm::errc::io_error);
  }

  auto Address = *AddrRes;
  if (!BC->HasFixedLoadAddress)
    adjustAddress(Address, MMapInfoIter->second);

  return PerfBasicSample{Event.get(), Address};
}

ErrorOr<DataAggregator::PerfMemSample> DataAggregator::parseMemSample() {
  PerfMemSample Res{0,0};

  while (checkAndConsumeFS()) {}

  auto PIDRes = parseNumberField(FieldSeparator, true);
  if (std::error_code EC = PIDRes.getError())
    return EC;

  auto MMapInfoIter = BinaryMMapInfo.find(*PIDRes);
  if (MMapInfoIter == BinaryMMapInfo.end()) {
    consumeRestOfLine();
    return Res;
  }

  while (checkAndConsumeFS()) {}

  auto Event = parseString(FieldSeparator);
  if (std::error_code EC = Event.getError())
    return EC;
  if (Event.get().find("mem-loads") == StringRef::npos) {
    consumeRestOfLine();
    return Res;
  }

  while (checkAndConsumeFS()) {}

  auto AddrRes = parseHexField(FieldSeparator);
  if (std::error_code EC = AddrRes.getError()) {
    return EC;
  }

  while (checkAndConsumeFS()) {}

  auto PCRes = parseHexField(FieldSeparator, true);
  if (std::error_code EC = PCRes.getError()) {
    consumeRestOfLine();
    return EC;
  }

  if (!checkAndConsumeNewLine()) {
    reportError("expected end of line");
    return make_error_code(llvm::errc::io_error);
  }

  auto Address = *AddrRes;
  if (!BC->HasFixedLoadAddress)
    adjustAddress(Address, MMapInfoIter->second);

  return PerfMemSample{PCRes.get(), Address};
}

ErrorOr<Location> DataAggregator::parseLocationOrOffset() {
  auto parseOffset = [this]() -> ErrorOr<Location> {
    auto Res = parseHexField(FieldSeparator);
    if (std::error_code EC = Res.getError())
      return EC;
    return Location(Res.get());
  };

  auto Sep = ParsingBuf.find_first_of(" \n");
  if (Sep == StringRef::npos)
    return parseOffset();
  auto LookAhead = ParsingBuf.substr(0, Sep);
  if (LookAhead.find_first_of(":") == StringRef::npos)
    return parseOffset();

  auto BuildID = parseString(':');
  if (std::error_code EC = BuildID.getError())
    return EC;
  auto Offset = parseHexField(FieldSeparator);
  if (std::error_code EC = Offset.getError())
    return EC;
  return Location(true, BuildID.get(), Offset.get());
}

ErrorOr<DataAggregator::AggregatedLBREntry>
DataAggregator::parseAggregatedLBREntry() {
  while (checkAndConsumeFS()) {}

  auto TypeOrErr = parseString(FieldSeparator);
  if (std::error_code EC = TypeOrErr.getError())
    return EC;
  auto Type = AggregatedLBREntry::BRANCH;
  if (TypeOrErr.get() == "B") {
    Type = AggregatedLBREntry::BRANCH;
  } else if (TypeOrErr.get() == "F") {
    Type = AggregatedLBREntry::FT;
  } else if (TypeOrErr.get() == "f") {
    Type = AggregatedLBREntry::FT_EXTERNAL_ORIGIN;
  } else {
    reportError("expected B, F or f");
    return make_error_code(llvm::errc::io_error);
  }

  while (checkAndConsumeFS()) {}
  auto From = parseLocationOrOffset();
  if (std::error_code EC = From.getError())
    return EC;

  while (checkAndConsumeFS()) {}
  auto To = parseLocationOrOffset();
  if (std::error_code EC = To.getError())
    return EC;

  while (checkAndConsumeFS()) {}
  auto Frequency = parseNumberField(FieldSeparator,
                                    Type != AggregatedLBREntry::BRANCH);
  if (std::error_code EC = Frequency.getError())
    return EC;

  uint64_t Mispreds{0};
  if (Type == AggregatedLBREntry::BRANCH) {
    while (checkAndConsumeFS()) {}
    auto MispredsOrErr = parseNumberField(FieldSeparator, true);
    if (std::error_code EC = MispredsOrErr.getError())
      return EC;
    Mispreds = static_cast<uint64_t>(MispredsOrErr.get());
  }

  if (!checkAndConsumeNewLine()) {
    reportError("expected end of line");
    return make_error_code(llvm::errc::io_error);
  }

  return AggregatedLBREntry{From.get(), To.get(),
                            static_cast<uint64_t>(Frequency.get()), Mispreds,
                            Type};
}

bool DataAggregator::hasData() {
  if (ParsingBuf.size() == 0)
    return false;

  return true;
}

bool DataAggregator::ignoreKernelInterrupt(LBREntry &LBR) const {
  return opts::IgnoreInterruptLBR &&
         (LBR.From >= KernelBaseAddr || LBR.To >= KernelBaseAddr);
}

std::error_code DataAggregator::printLBRHeatMap() {
  outs() << "PERF2BOLT: parse branch events...\n";
  NamedRegionTimer T("parseBranch", "Parsing branch events", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);

  if (opts::LinuxKernelMode) {
    opts::HeatmapMaxAddress = 0xffffffffffffffff;
    opts::HeatmapMinAddress = KernelBaseAddr;
  }
  Heatmap HM(opts::HeatmapBlock, opts::HeatmapMinAddress,
             opts::HeatmapMaxAddress);
  uint64_t NumTotalSamples{0};

  while (hasData()) {
    auto SampleRes = parseBranchSample();
    if (auto EC = SampleRes.getError()) {
      if (EC == errc::no_such_process)
        continue;
      return EC;
    }

    auto &Sample = SampleRes.get();

    // LBRs are stored in reverse execution order. NextLBR refers to the next
    // executed branch record.
    const LBREntry *NextLBR{nullptr};
    for (const auto &LBR : Sample.LBR) {
      if (NextLBR) {
        // Record fall-through trace.
        const auto TraceFrom = LBR.To;
        const auto TraceTo = NextLBR->From;
        ++FallthroughLBRs[Trace(TraceFrom, TraceTo)].InternCount;
      }
      NextLBR = &LBR;
    }
    if (!Sample.LBR.empty()) {
      HM.registerAddress(Sample.LBR.front().To);
      HM.registerAddress(Sample.LBR.back().From);
    }
    NumTotalSamples += Sample.LBR.size();
  }

  if (!NumTotalSamples) {
    errs() << "HEATMAP-ERROR: no LBR traces detected in profile. "
              "Cannot build heatmap.\n";
    exit(1);
  }

  outs() << "HEATMAP: read " << NumTotalSamples << " LBR samples\n";
  outs() << "HEATMAP: " << FallthroughLBRs.size() << " unique traces\n";

  outs() << "HEATMAP: building heat map...\n";

  for (const auto &LBR : FallthroughLBRs) {
    const auto &Trace = LBR.first;
    const auto &Info = LBR.second;
    HM.registerAddressRange(Trace.From, Trace.To, Info.InternCount);
  }

  if (HM.getNumInvalidRanges())
    outs() << "HEATMAP: invalid traces: " << HM.getNumInvalidRanges() << '\n';

  if (!HM.size()) {
    errs() << "HEATMAP-ERROR: no valid traces registered\n";
    exit(1);
  }

  HM.print(opts::HeatmapFile);

  return std::error_code();
}

std::error_code DataAggregator::parseBranchEvents() {
  outs() << "PERF2BOLT: parse branch events...\n";
  NamedRegionTimer T("parseBranch", "Parsing branch events", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);

  uint64_t NumTotalSamples{0};
  uint64_t NumEntries{0};
  uint64_t NumSamples{0};
  uint64_t NumSamplesNoLBR{0};
  uint64_t NumTraces{0};
  bool NeedsSkylakeFix{false};

  while (hasData() && NumTotalSamples < opts::MaxSamples) {
    ++NumTotalSamples;

    auto SampleRes = parseBranchSample();
    if (auto EC = SampleRes.getError()) {
      if (EC == errc::no_such_process)
        continue;
      return EC;
    }
    ++NumSamples;

    auto &Sample = SampleRes.get();
    if (opts::WriteAutoFDOData)
      ++BasicSamples[Sample.PC];

    if (Sample.LBR.empty()) {
      ++NumSamplesNoLBR;
      continue;
    }

    NumEntries += Sample.LBR.size();
    if (BAT && NumEntries == 32 && !NeedsSkylakeFix) {
      outs() << "BOLT-WARNING: Using Intel Skylake bug workaround\n";
      NeedsSkylakeFix = true;
    }

    // LBRs are stored in reverse execution order. NextPC refers to the next
    // recorded executed PC.
    uint64_t NextPC = opts::UseEventPC ? Sample.PC : 0;
    uint32_t NumEntry{0};
    for (const auto &LBR : Sample.LBR) {
      ++NumEntry;
      // Hardware bug workaround: Intel Skylake (which has 32 LBR entries)
      // sometimes record entry 32 as an exact copy of entry 31. This will cause
      // us to likely record an invalid trace and generate a stale function for
      // BAT mode (non BAT disassembles the function and is able to ignore this
      // trace at aggregation time). Drop first 2 entries (last two, in
      // chronological order)
      if (NeedsSkylakeFix && NumEntry <= 2)
        continue;
      if (NextPC) {
        // Record fall-through trace.
        const auto TraceFrom = LBR.To;
        const auto TraceTo = NextPC;
        const auto *TraceBF = getBinaryFunctionContainingAddress(TraceFrom);
        if (TraceBF && TraceBF->containsAddress(TraceTo)) {
            auto &Info = FallthroughLBRs[Trace(TraceFrom, TraceTo)];
            if (TraceBF->containsAddress(LBR.From)) {
              ++Info.InternCount;
            } else {
              ++Info.ExternCount;
            }
        } else {
          if (TraceBF && getBinaryFunctionContainingAddress(TraceTo)) {
            DEBUG(dbgs() << "Invalid trace starting in "
                         << TraceBF->getPrintName() << " @ "
                         << Twine::utohexstr(TraceFrom - TraceBF->getAddress())
                         << " and ending @ " << Twine::utohexstr(TraceTo)
                         << '\n');
            ++NumInvalidTraces;
          } else {
            DEBUG(
                dbgs() << "Out of range trace starting in "
                       << (TraceBF ? TraceBF->getPrintName() : "None") << " @ "
                       << Twine::utohexstr(
                              TraceFrom - (TraceBF ? TraceBF->getAddress() : 0))
                       << " and ending in "
                       << (getBinaryFunctionContainingAddress(TraceTo)
                               ? getBinaryFunctionContainingAddress(TraceTo)
                                     ->getPrintName()
                               : "None")
                       << " @ "
                       << Twine::utohexstr(
                              TraceTo -
                              (getBinaryFunctionContainingAddress(TraceTo)
                                   ? getBinaryFunctionContainingAddress(TraceTo)
                                         ->getAddress()
                                   : 0))
                       << '\n');
            ++NumLongRangeTraces;
          }
        }
        ++NumTraces;
      }
      NextPC = LBR.From;

      auto From = LBR.From;
      if (!getBinaryFunctionContainingAddress(From))
        From = 0;
      auto To = LBR.To;
      if (!getBinaryFunctionContainingAddress(To))
        To = 0;
      if (!From && !To)
        continue;
      auto &Info = BranchLBRs[Trace(From, To)];
      ++Info.TakenCount;
      Info.MispredCount += LBR.Mispred;
    }
  }

  for (const auto &LBR : BranchLBRs) {
    const auto &Trace = LBR.first;
    if (auto *BF = getBinaryFunctionContainingAddress(Trace.From))
      BF->setHasProfileAvailable();
    if (auto *BF = getBinaryFunctionContainingAddress(Trace.To))
      BF->setHasProfileAvailable();
  }

  auto printColored = [](raw_ostream &OS, float Percent, float T1, float T2) {
    OS << " (";
    if (OS.has_colors()) {
      if (Percent > T2) {
        OS.changeColor(raw_ostream::RED);
      } else if (Percent > T1) {
        OS.changeColor(raw_ostream::YELLOW);
      } else {
        OS.changeColor(raw_ostream::GREEN);
      }
    }
    OS << format("%.1f%%", Percent);
    if (OS.has_colors())
      OS.resetColor();
    OS << ")";
  };

  outs() << "PERF2BOLT: read " << NumSamples << " samples and "
         << NumEntries << " LBR entries\n";
  if (NumTotalSamples) {
    if (NumSamples && NumSamplesNoLBR == NumSamples) {
      // Note: we don't know if perf2bolt is being used to parse memory samples
      // at this point. In this case, it is OK to parse zero LBRs.
      errs() << "PERF2BOLT-WARNING: all recorded samples for this binary lack "
                "LBR. Record profile with perf record -j any or run perf2bolt "
                "in no-LBR mode with -nl (the performance improvement in -nl "
                "mode may be limited)\n";
    } else {
      const auto IgnoredSamples = NumTotalSamples - NumSamples;
      const auto PercentIgnored = 100.0f * IgnoredSamples / NumTotalSamples;
      outs() << "PERF2BOLT: " << IgnoredSamples << " samples";
      printColored(outs(), PercentIgnored, 20, 50);
      outs() << " were ignored\n";
      if (PercentIgnored > 50.0f) {
        errs() << "PERF2BOLT-WARNING: less than 50% of all recorded samples "
                  "were attributed to the input binary\n";
      }
    }
  }
  outs() << "PERF2BOLT: traces mismatching disassembled function contents: "
         << NumInvalidTraces;
  float Perc{0.0f};
  if (NumTraces > 0) {
    Perc = NumInvalidTraces * 100.0f / NumTraces;
    printColored(outs(), Perc, 5, 10);
  }
  outs() << "\n";
  if (Perc > 10.0f) {
    outs() << "\n !! WARNING !! This high mismatch ratio indicates the input "
              "binary is probably not the same binary used during profiling "
              "collection. The generated data may be ineffective for improving "
              "performance.\n\n";
  }

  outs() << "PERF2BOLT: out of range traces involving unknown regions: "
         << NumLongRangeTraces;
  if (NumTraces > 0) {
    outs() << format(" (%.1f%%)", NumLongRangeTraces * 100.0f / NumTraces);
  }
  outs() << "\n";

  if (NumColdSamples > 0) {
    const auto ColdSamples = NumColdSamples * 100.0f / NumTotalSamples;
    outs() << "PERF2BOLT: " << NumColdSamples
           << format(" (%.1f%%)", ColdSamples)
           << " samples recorded in cold regions of split functions.\n";
    if (ColdSamples > 5.0f) {
      outs()
          << "WARNING: The BOLT-processed binary where samples were collected "
             "likely used bad data or your service observed a large shift in "
             "profile. You may want to audit this.\n";
    }
  }

  return std::error_code();
}

void DataAggregator::processBranchEvents() {
  outs() << "PERF2BOLT: processing branch events...\n";
  NamedRegionTimer T("processBranch", "Processing branch events",
                     TimerGroupName, TimerGroupDesc, opts::TimeAggregator);

  for (const auto &AggrLBR : FallthroughLBRs) {
    auto &Loc = AggrLBR.first;
    auto &Info = AggrLBR.second;
    LBREntry First{Loc.From, Loc.From, false};
    LBREntry Second{Loc.To, Loc.To, false};
    if (Info.InternCount) {
      doTrace(First, Second, Info.InternCount);
    }
    if (Info.ExternCount) {
      First.From = 0;
      doTrace(First, Second, Info.ExternCount);
    }
  }

  for (const auto &AggrLBR : BranchLBRs) {
    auto &Loc = AggrLBR.first;
    auto &Info = AggrLBR.second;
    doBranch(Loc.From, Loc.To, Info.TakenCount, Info.MispredCount);
  }
}

std::error_code DataAggregator::parseBasicEvents() {
  outs() << "PERF2BOLT: parsing basic events (without LBR)...\n";
  NamedRegionTimer T("parseBasic", "Parsing basic events", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);
  while (hasData()) {
    auto Sample = parseBasicSample();
    if (std::error_code EC = Sample.getError())
      return EC;

    if (!Sample->PC)
      continue;

    if (auto *BF = getBinaryFunctionContainingAddress(Sample->PC))
      BF->setHasProfileAvailable();

    ++BasicSamples[Sample->PC];
    EventNames.insert(Sample->EventName);
  }

  return std::error_code();
}

void DataAggregator::processBasicEvents() {
  outs() << "PERF2BOLT: processing basic events (without LBR)...\n";
  NamedRegionTimer T("processBasic", "Processing basic events",
                     TimerGroupName, TimerGroupDesc, opts::TimeAggregator);
  uint64_t OutOfRangeSamples{0};
  uint64_t NumSamples{0};
  for (auto &Sample : BasicSamples) {
    const auto PC = Sample.first;
    const auto HitCount = Sample.second;
    NumSamples += HitCount;
    auto *Func = getBinaryFunctionContainingAddress(PC);
    if (!Func) {
      OutOfRangeSamples += HitCount;
      continue;
    }

    doSample(*Func, PC, HitCount);
  }
  outs() << "PERF2BOLT: read " << NumSamples << " samples\n";

  outs() << "PERF2BOLT: out of range samples recorded in unknown regions: "
         << OutOfRangeSamples;
  float Perc{0.0f};
  if (NumSamples > 0) {
    outs() << " (";
    Perc = OutOfRangeSamples * 100.0f / NumSamples;
    if (outs().has_colors()) {
      if (Perc > 60.0f) {
        outs().changeColor(raw_ostream::RED);
      } else if (Perc > 40.0f) {
        outs().changeColor(raw_ostream::YELLOW);
      } else {
        outs().changeColor(raw_ostream::GREEN);
      }
    }
    outs() << format("%.1f%%", Perc);
    if (outs().has_colors())
      outs().resetColor();
    outs() << ")";
  }
  outs() << "\n";
  if (Perc > 80.0f) {
    outs() << "\n !! WARNING !! This high mismatch ratio indicates the input "
              "binary is probably not the same binary used during profiling "
              "collection. The generated data may be ineffective for improving "
              "performance.\n\n";
  }
}

std::error_code DataAggregator::parseMemEvents() {
  outs() << "PERF2BOLT: parsing memory events...\n";
  NamedRegionTimer T("parseMemEvents", "Parsing mem events", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);
  while (hasData()) {
    auto Sample = parseMemSample();
    if (std::error_code EC = Sample.getError())
      return EC;

    if (auto *BF = getBinaryFunctionContainingAddress(Sample->PC))
      BF->setHasProfileAvailable();

    MemSamples.emplace_back(std::move(Sample.get()));
  }

  return std::error_code();
}

void DataAggregator::processMemEvents() {
  NamedRegionTimer T("ProcessMemEvents", "Processing mem events",
                     TimerGroupName, TimerGroupDesc, opts::TimeAggregator);
  for (const auto &Sample : MemSamples) {
    auto PC = Sample.PC;
    auto Addr = Sample.Addr;
    StringRef FuncName;
    StringRef MemName;

    // Try to resolve symbol for PC
    auto *Func = getBinaryFunctionContainingAddress(PC);
    if (!Func) {
      DEBUG(if (PC != 0) {
        dbgs() << "Skipped mem event: 0x" << Twine::utohexstr(PC) << " => 0x"
               << Twine::utohexstr(Addr) << "\n";
      });
      continue;
    }

    FuncName = Func->getOneName();
    PC -= Func->getAddress();

    // Try to resolve symbol for memory load
    if (auto *BD = BC->getBinaryDataContainingAddress(Addr)) {
      MemName = BD->getName();
      Addr -= BD->getAddress();
    } else if (opts::FilterMemProfile) {
      // Filter out heap/stack accesses
      continue;
    }

    const Location FuncLoc(!FuncName.empty(), FuncName, PC);
    const Location AddrLoc(!MemName.empty(), MemName, Addr);

    auto *MemData = &NamesToMemEvents[FuncName];
    setMemData(*Func, MemData);
    MemData->update(FuncLoc, AddrLoc);
    DEBUG(dbgs() << "Mem event: " << FuncLoc << " = " << AddrLoc << "\n");
  }
}

std::error_code DataAggregator::parsePreAggregatedLBRSamples() {
  outs() << "PERF2BOLT: parsing pre-aggregated profile...\n";
  NamedRegionTimer T("parseAggregated", "Parsing aggregated branch events",
                     TimerGroupName, TimerGroupDesc, opts::TimeAggregator);
  while (hasData()) {
    auto AggrEntry = parseAggregatedLBREntry();
    if (std::error_code EC = AggrEntry.getError())
      return EC;

    if (auto *BF = getBinaryFunctionContainingAddress(AggrEntry->From.Offset))
      BF->setHasProfileAvailable();
    if (auto *BF = getBinaryFunctionContainingAddress(AggrEntry->To.Offset))
      BF->setHasProfileAvailable();

    AggregatedLBRs.emplace_back(std::move(AggrEntry.get()));
  }

  return std::error_code();
}

void DataAggregator::processPreAggregated() {
  outs() << "PERF2BOLT: processing pre-aggregated profile...\n";
  NamedRegionTimer T("processAggregated", "Processing aggregated branch events",
                     TimerGroupName, TimerGroupDesc, opts::TimeAggregator);

  uint64_t NumTraces{0};
  for (const auto &AggrEntry : AggregatedLBRs) {
    switch (AggrEntry.EntryType) {
    case AggregatedLBREntry::BRANCH:
      doBranch(AggrEntry.From.Offset, AggrEntry.To.Offset, AggrEntry.Count,
               AggrEntry.Mispreds);
      break;
    case AggregatedLBREntry::FT:
    case AggregatedLBREntry::FT_EXTERNAL_ORIGIN: {
      LBREntry First{AggrEntry.EntryType == AggregatedLBREntry::FT
                         ? AggrEntry.From.Offset
                         : 0,
                     AggrEntry.From.Offset, false};
      LBREntry Second{AggrEntry.To.Offset, AggrEntry.To.Offset, false};
      doTrace(First, Second, AggrEntry.Count);
      NumTraces += AggrEntry.Count;
      break;
    }
    }
  }

  outs() << "PERF2BOLT: read " << AggregatedLBRs.size()
         << " aggregated LBR entries\n";
  outs() << "PERF2BOLT: traces mismatching disassembled function contents: "
         << NumInvalidTraces;
  float Perc{0.0f};
  if (NumTraces > 0) {
    outs() << " (";
    Perc = NumInvalidTraces * 100.0f / NumTraces;
    if (outs().has_colors()) {
      if (Perc > 10.0f) {
        outs().changeColor(raw_ostream::RED);
      } else if (Perc > 5.0f) {
        outs().changeColor(raw_ostream::YELLOW);
      } else {
        outs().changeColor(raw_ostream::GREEN);
      }
    }
    outs() << format("%.1f%%", Perc);
    if (outs().has_colors())
      outs().resetColor();
    outs() << ")";
  }
  outs() << "\n";
  if (Perc > 10.0f) {
    outs() << "\n !! WARNING !! This high mismatch ratio indicates the input "
              "binary is probably not the same binary used during profiling "
              "collection. The generated data may be ineffective for improving "
              "performance.\n\n";
  }

  outs() << "PERF2BOLT: Out of range traces involving unknown regions: "
         << NumLongRangeTraces;
  if (NumTraces > 0) {
    outs() << format(" (%.1f%%)", NumLongRangeTraces * 100.0f / NumTraces);
  }
  outs() << "\n";
}

Optional<pid_t>
DataAggregator::parseCommExecEvent() {
  auto LineEnd = ParsingBuf.find_first_of("\n");
  if (LineEnd == StringRef::npos) {
    reportError("expected rest of line");
    Diag << "Found: " << ParsingBuf << "\n";
    return NoneType();
  }
  StringRef Line = ParsingBuf.substr(0, LineEnd);

  auto Pos = Line.find("PERF_RECORD_COMM exec");
  if (Pos == StringRef::npos) {
    return NoneType();
  }
  Line = Line.drop_front(Pos);

  // Line:
  //  PERF_RECORD_COMM exec: <name>:<pid>/<tid>"
  auto PIDStr = Line.rsplit(':').second.split('/').first;
  pid_t PID;
  if (PIDStr.getAsInteger(10, PID)) {
    reportError("expected PID");
    Diag << "Found: " << PIDStr << "in '" << Line << "'\n";
    return NoneType();
  }

  return PID;
}

namespace {
Optional<uint64_t> parsePerfTime(const StringRef TimeStr) {
  const auto SecTimeStr = TimeStr.split('.').first;
  const auto USecTimeStr = TimeStr.split('.').second;
  uint64_t SecTime;
  uint64_t USecTime;
  if (SecTimeStr.getAsInteger(10, SecTime) ||
      USecTimeStr.getAsInteger(10, USecTime)) {
    return NoneType();
  }
  return SecTime * 1000000ULL + USecTime;
}
}

Optional<DataAggregator::ForkInfo>
DataAggregator::parseForkEvent() {
  while (checkAndConsumeFS()) {}

  auto LineEnd = ParsingBuf.find_first_of("\n");
  if (LineEnd == StringRef::npos) {
    reportError("expected rest of line");
    Diag << "Found: " << ParsingBuf << "\n";
    return NoneType();
  }
  StringRef Line = ParsingBuf.substr(0, LineEnd);

  auto Pos = Line.find("PERF_RECORD_FORK");
  if (Pos == StringRef::npos) {
    consumeRestOfLine();
    return NoneType();
  }

  ForkInfo FI;

  const auto TimeStr =
    Line.substr(0, Pos).rsplit(':').first.rsplit(FieldSeparator).second;
  if (auto TimeRes = parsePerfTime(TimeStr)) {
    FI.Time = *TimeRes;
  }

  Line = Line.drop_front(Pos);

  // Line:
  //  PERF_RECORD_FORK(<child_pid>:<child_tid>):(<parent_pid>:<parent_tid>)
  const auto ChildPIDStr = Line.split('(').second.split(':').first;
  if (ChildPIDStr.getAsInteger(10, FI.ChildPID)) {
    reportError("expected PID");
    Diag << "Found: " << ChildPIDStr << "in '" << Line << "'\n";
    return NoneType();
  }

  const auto ParentPIDStr = Line.rsplit('(').second.split(':').first;
  if (ParentPIDStr.getAsInteger(10, FI.ParentPID)) {
    reportError("expected PID");
    Diag << "Found: " << ParentPIDStr << "in '" << Line << "'\n";
    return NoneType();
  }

  consumeRestOfLine();

  return FI;
}

ErrorOr<std::pair<StringRef, DataAggregator::MMapInfo>>
DataAggregator::parseMMapEvent() {
  while (checkAndConsumeFS()) {}

  MMapInfo ParsedInfo;

  auto LineEnd = ParsingBuf.find_first_of("\n");
  if (LineEnd == StringRef::npos) {
    reportError("expected rest of line");
    Diag << "Found: " << ParsingBuf << "\n";
    return make_error_code(llvm::errc::io_error);
  }
  StringRef Line = ParsingBuf.substr(0, LineEnd);

  auto Pos = Line.find("PERF_RECORD_MMAP2");
  if (Pos == StringRef::npos) {
    consumeRestOfLine();
    return std::make_pair(StringRef(), ParsedInfo);
  }

  // Line:
  //   {<name> .* <sec>.<usec>: }PERF_RECORD_MMAP2 <pid>/<tid>: .* <file_name>

  const auto TimeStr =
    Line.substr(0, Pos).rsplit(':').first.rsplit(FieldSeparator).second;
  if (auto TimeRes = parsePerfTime(TimeStr)) {
    ParsedInfo.Time = *TimeRes;
  }

  Line = Line.drop_front(Pos);

  // Line:
  //   PERF_RECORD_MMAP2 <pid>/<tid>: [<hexbase>(<hexsize>) .*]: .* <file_name>

  auto FileName = Line.rsplit(FieldSeparator).second;
  if (FileName.startswith("//") || FileName.startswith("[")) {
    consumeRestOfLine();
    return std::make_pair(StringRef(), ParsedInfo);
  }
  FileName = sys::path::filename(FileName);

  const auto PIDStr = Line.split(FieldSeparator).second.split('/').first;
  if (PIDStr.getAsInteger(10, ParsedInfo.PID)) {
    reportError("expected PID");
    Diag << "Found: " << PIDStr << "in '" << Line << "'\n";
    return make_error_code(llvm::errc::io_error);
  }

  const auto BaseAddressStr = Line.split('[').second.split('(').first;
  if (BaseAddressStr.getAsInteger(0, ParsedInfo.BaseAddress)) {
    reportError("expected base address");
    Diag << "Found: " << BaseAddressStr << "in '" << Line << "'\n";
    return make_error_code(llvm::errc::io_error);
  }

  const auto SizeStr = Line.split('(').second.split(')').first;
  if (SizeStr.getAsInteger(0, ParsedInfo.Size)) {
    reportError("expected mmaped size");
    Diag << "Found: " << SizeStr << "in '" << Line << "'\n";
    return make_error_code(llvm::errc::io_error);
  }

  const auto OffsetStr =
      Line.split('@').second.ltrim().split(FieldSeparator).first;
  if (OffsetStr.getAsInteger(0, ParsedInfo.Offset)) {
    reportError("expected mmaped page-aligned offset");
    Diag << "Found: " << OffsetStr << "in '" << Line << "'\n";
    return make_error_code(llvm::errc::io_error);
  }

  consumeRestOfLine();

  return std::make_pair(FileName, ParsedInfo);
}

std::error_code DataAggregator::parseMMapEvents() {
  outs() << "PERF2BOLT: parsing perf-script mmap events output\n";
  NamedRegionTimer T("parseMMapEvents", "Parsing mmap events", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);

  std::multimap<StringRef, MMapInfo> GlobalMMapInfo;
  while (hasData()) {
    auto FileMMapInfoRes = parseMMapEvent();
    if (std::error_code EC = FileMMapInfoRes.getError())
      return EC;

    auto FileMMapInfo = FileMMapInfoRes.get();
    if (FileMMapInfo.second.PID == -1)
      continue;

    // Consider only the first mapping of the file for any given PID
    bool PIDExists = false;
    auto Range = GlobalMMapInfo.equal_range(FileMMapInfo.first);
    for (auto MI = Range.first; MI != Range.second; ++MI) {
      if (MI->second.PID == FileMMapInfo.second.PID) {
        PIDExists = true;
        break;
      }
    }
    if (PIDExists)
      continue;

    GlobalMMapInfo.insert(FileMMapInfo);
  }

  DEBUG(
    dbgs() << "FileName -> mmap info:\n";
    for (const auto &Pair : GlobalMMapInfo) {
      dbgs() << "  " << Pair.first << " : " << Pair.second.PID << " [0x"
             << Twine::utohexstr(Pair.second.BaseAddress) << ", "
             << Twine::utohexstr(Pair.second.Size) << " @ "
             << Twine::utohexstr(Pair.second.Offset) << "]\n";
    }
  );

  auto NameToUse = llvm::sys::path::filename(BC->getFilename());
  if (GlobalMMapInfo.count(NameToUse) == 0 && !BuildIDBinaryName.empty()) {
    errs() << "PERF2BOLT-WARNING: using \"" << BuildIDBinaryName
           << "\" for profile matching\n";
    NameToUse = BuildIDBinaryName;
  }

  auto Range = GlobalMMapInfo.equal_range(NameToUse);
  for (auto I = Range.first; I != Range.second; ++I) {
    if (BC->HasFixedLoadAddress && I->second.BaseAddress) {
      // Check that the binary mapping matches one of the segments.
      bool MatchFound{false};
      for (auto &KV : BC->EFMM->SegmentMapInfo) {
        auto &SegInfo = KV.second;
        const auto MapAddress = alignDown(SegInfo.Address, SegInfo.Alignment);
        if (I->second.BaseAddress == MapAddress) {
          MatchFound = true;
          break;
        }
      }
      if (!MatchFound) {
        errs() << "PERF2BOLT-WARNING: ignoring mapping of " << NameToUse
               << " at 0x" << Twine::utohexstr(I->second.BaseAddress) << '\n';
        continue;
      }
    }

    BinaryMMapInfo.insert(std::make_pair(I->second.PID, I->second));
  }

  if (BinaryMMapInfo.empty()) {
    if (errs().has_colors())
      errs().changeColor(raw_ostream::RED);
    errs() << "PERF2BOLT-ERROR: could not find a profile matching binary \""
           << BC->getFilename() << "\".";
    if (!GlobalMMapInfo.empty()) {
      errs() << " Profile for the following binary name(s) is available:\n";
      for (auto I = GlobalMMapInfo.begin(), IE = GlobalMMapInfo.end(); I != IE;
           I = GlobalMMapInfo.upper_bound(I->first)) {
        errs() << "  " << I->first << '\n';
      }
      errs() << "Please rename the input binary.\n";
    } else {
      errs() << " Failed to extract any binary name from a profile.\n";
    }
    if (errs().has_colors())
      errs().resetColor();

    exit(1);
  }

  return std::error_code();
}

std::error_code DataAggregator::parseTaskEvents() {
  outs() << "PERF2BOLT: parsing perf-script task events output\n";
  NamedRegionTimer T("parseTaskEvents", "Parsing task events", TimerGroupName,
                     TimerGroupDesc, opts::TimeAggregator);

  while (hasData()) {
    if (auto CommInfo = parseCommExecEvent()) {
      // Remove forked child that ran execve
      auto MMapInfoIter = BinaryMMapInfo.find(*CommInfo);
      if (MMapInfoIter != BinaryMMapInfo.end() &&
          MMapInfoIter->second.Forked) {
        BinaryMMapInfo.erase(MMapInfoIter);
      }
      consumeRestOfLine();
      continue;
    }

    auto ForkInfo = parseForkEvent();
    if (!ForkInfo)
      continue;

    if (ForkInfo->ParentPID == ForkInfo->ChildPID)
      continue;

    if (ForkInfo->Time == 0) {
      // Process was forked and mmaped before perf ran. In this case the child
      // should have its own mmap entry unless it was execve'd.
      continue;
    }

    auto MMapInfoIter = BinaryMMapInfo.find(ForkInfo->ParentPID);
    if (MMapInfoIter == BinaryMMapInfo.end())
      continue;

    auto MMapInfo = MMapInfoIter->second;
    MMapInfo.PID = ForkInfo->ChildPID;
    MMapInfo.Forked = true;
    BinaryMMapInfo.insert(std::make_pair(MMapInfo.PID, MMapInfo));
  }

  outs() << "PERF2BOLT: input binary is associated with "
         << BinaryMMapInfo.size() << " PID(s)\n";

  DEBUG(
    for (auto &MMI : BinaryMMapInfo) {
      outs() << "  " << MMI.second.PID << (MMI.second.Forked ? " (forked)" : "")
             << ": (0x" << Twine::utohexstr(MMI.second.BaseAddress)
             << ": 0x" << Twine::utohexstr(MMI.second.Size) << ")\n";
    }
  );

  return std::error_code();
}

Optional<std::pair<StringRef, StringRef>>
DataAggregator::parseNameBuildIDPair() {
  while (checkAndConsumeFS()) {}

  auto BuildIDStr = parseString(FieldSeparator, true);
  if (std::error_code EC = BuildIDStr.getError())
    return NoneType();

  auto NameStr = parseString(FieldSeparator, true);
  if (std::error_code EC = NameStr.getError())
    return NoneType();

  consumeRestOfLine();
  return std::make_pair(NameStr.get(), BuildIDStr.get());
}

Optional<StringRef>
DataAggregator::getFileNameForBuildID(StringRef FileBuildID) {
  while (hasData()) {
    auto IDPair = parseNameBuildIDPair();
    if (!IDPair)
      return NoneType();

    if (IDPair->second.startswith(FileBuildID))
      return sys::path::filename(IDPair->first);
  }
  return NoneType();
}

std::error_code
DataAggregator::writeAggregatedFile(StringRef OutputFilename) const {
  std::error_code EC;
  raw_fd_ostream OutFile(OutputFilename, EC, sys::fs::OpenFlags::F_None);
  if (EC)
    return EC;

  bool WriteMemLocs = false;

  auto writeLocation = [&OutFile,&WriteMemLocs](const Location &Loc) {
    if (WriteMemLocs)
      OutFile << (Loc.IsSymbol ? "4 " : "3 ");
    else
      OutFile << (Loc.IsSymbol ? "1 " : "0 ");
    OutFile << (Loc.Name.empty() ? "[unknown]" : Loc.Name)  << " "
            << Twine::utohexstr(Loc.Offset)
            << FieldSeparator;
  };

  uint64_t BranchValues{0};
  uint64_t MemValues{0};

  if (BAT)
    OutFile << "boltedcollection\n";
  if (opts::BasicAggregation) {
    OutFile << "no_lbr";
    for (const auto &Entry : EventNames) {
      OutFile << " " << Entry.getKey();
    }
    OutFile << "\n";

    for (const auto &Func : NamesToSamples) {
      for (const auto &SI : Func.getValue().Data) {
        writeLocation(SI.Loc);
        OutFile << SI.Hits << "\n";
        ++BranchValues;
      }
    }
  } else {
    for (const auto &Func : NamesToBranches) {
      for (const auto &BI : Func.getValue().Data) {
        writeLocation(BI.From);
        writeLocation(BI.To);
        OutFile << BI.Mispreds << " " << BI.Branches << "\n";
        ++BranchValues;
      }
      for (const auto &BI : Func.getValue().EntryData) {
        // Do not output if source is a known symbol, since this was already
        // accounted for in the source function
        if (BI.From.IsSymbol)
          continue;
        writeLocation(BI.From);
        writeLocation(BI.To);
        OutFile << BI.Mispreds << " " << BI.Branches << "\n";
        ++BranchValues;
      }
    }

    WriteMemLocs = true;
    for (const auto &Func : NamesToMemEvents) {
      for (const auto &MemEvent : Func.getValue().Data) {
        writeLocation(MemEvent.Offset);
        writeLocation(MemEvent.Addr);
        OutFile << MemEvent.Count << "\n";
        ++MemValues;
      }
    }
  }

  outs() << "PERF2BOLT: wrote " << BranchValues << " objects and "
         << MemValues << " memory objects to " << OutputFilename << "\n";

  return std::error_code();
}

void DataAggregator::dump() const {
  DataReader::dump();
}

void DataAggregator::dump(const LBREntry &LBR) const {
  Diag << "From: " << Twine::utohexstr(LBR.From)
       << " To: " << Twine::utohexstr(LBR.To) << " Mispred? " << LBR.Mispred
       << "\n";
}

void DataAggregator::dump(const PerfBranchSample &Sample) const {
  Diag << "Sample LBR entries: " << Sample.LBR.size() << "\n";
  for (const auto &LBR : Sample.LBR) {
    dump(LBR);
  }
}

void DataAggregator::dump(const PerfMemSample &Sample) const {
  Diag << "Sample mem entries: " << Sample.PC << ": " << Sample.Addr << "\n";
}
