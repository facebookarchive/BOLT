//===-- merge-fdata.cpp - Tool for merging profile in fdata format --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// merge-fdata 1.fdata 2.fdata 3.fdata > merged.fdata
//
//===----------------------------------------------------------------------===//

#include "../ProfileYAMLMapping.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Object/Binary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Signals.h"
#include <unordered_map>

using namespace llvm;
using namespace llvm::yaml::bolt;

namespace opts {

cl::OptionCategory MergeFdataCategory("merge-fdata options");

enum SortType : char {
  ST_NONE,
  ST_EXEC_COUNT,      /// Sort based on function execution count.
  ST_TOTAL_BRANCHES,  /// Sort based on all branches in the function.
};

static cl::list<std::string>
InputDataFilenames(
  cl::Positional,
  cl::CommaSeparated,
  cl::desc("<fdata1> [<fdata2>]..."),
  cl::OneOrMore,
  cl::cat(MergeFdataCategory));

static cl::opt<SortType>
PrintFunctionList("print",
  cl::desc("print the list of objects with count to stderr"),
  cl::init(ST_NONE),
  cl::values(clEnumValN(ST_NONE,
      "none",
      "do not print objects/functions"),
    clEnumValN(ST_EXEC_COUNT,
      "exec",
      "print functions sorted by execution count"),
    clEnumValN(ST_TOTAL_BRANCHES,
      "branches",
      "print functions sorted by total branch count")),
  cl::cat(MergeFdataCategory));

static cl::opt<bool>
SuppressMergedDataOutput("q",
  cl::desc("do not print merged data to stdout"),
  cl::init(false),
  cl::Optional,
  cl::cat(MergeFdataCategory));

} // namespace opts

namespace {

static StringRef ToolName;

static void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << ToolName << ": '" << Message << "': " << EC.message() << ".\n";
  exit(1);
}

static void report_error(Twine Message, StringRef CustomError) {
  errs() << ToolName << ": '" << Message << "': " << CustomError << ".\n";
  exit(1);
}

void mergeProfileHeaders(BinaryProfileHeader &MergedHeader,
                         const BinaryProfileHeader &Header) {
  if (MergedHeader.FileName.empty()) {
    MergedHeader.FileName = Header.FileName;
  }
  if (!MergedHeader.FileName.empty() &&
      MergedHeader.FileName != Header.FileName) {
    errs() << "WARNING: merging profile from a binary for "
           << Header.FileName << " into a profile for binary "
           << MergedHeader.FileName << '\n';
  }
  if (MergedHeader.Id.empty()) {
    MergedHeader.Id = Header.Id;
  }
  if (!MergedHeader.Id.empty() && (MergedHeader.Id != Header.Id)) {
    errs() << "WARNING: build-ids in merged profiles do not match\n";
  }

  // Cannot merge samples profile with LBR profile.
  if (!MergedHeader.Flags) {
    MergedHeader.Flags = Header.Flags;
  }
  constexpr auto Mask = llvm::bolt::BinaryFunction::PF_LBR |
                        llvm::bolt::BinaryFunction::PF_SAMPLE;
  if ((MergedHeader.Flags & Mask) != (Header.Flags & Mask)) {
    errs() << "ERROR: cannot merge LBR profile with non-LBR profile\n";
    exit(1);
  }
  MergedHeader.Flags = MergedHeader.Flags | Header.Flags;

  if (!Header.Origin.empty()) {
    if (MergedHeader.Origin.empty()) {
      MergedHeader.Origin = Header.Origin;
    } else if (MergedHeader.Origin != Header.Origin) {
      MergedHeader.Origin += "; " + Header.Origin;
    }
  }

  if (MergedHeader.EventNames.empty()) {
    MergedHeader.EventNames = Header.EventNames;
  }
  if (MergedHeader.EventNames != Header.EventNames) {
    errs() << "WARNING: merging profiles with different sampling events\n";
    MergedHeader.EventNames += "," + Header.EventNames;
  }
}

void mergeBasicBlockProfile(BinaryBasicBlockProfile &MergedBB,
                            BinaryBasicBlockProfile &&BB,
                            const BinaryFunctionProfile &BF) {
  // Verify that the blocks match.
  if (BB.NumInstructions != MergedBB.NumInstructions)
    report_error(BF.Name + " : BB #" + Twine(BB.Index),
                 "number of instructions in block mismatch");
  if (BB.Hash != MergedBB.Hash)
    report_error(BF.Name + " : BB #" + Twine(BB.Index),
                 "basic block hash mismatch");

  // Update the execution count.
  MergedBB.ExecCount += BB.ExecCount;

  // Update the event count.
  MergedBB.EventCount += BB.EventCount;

  // Merge calls sites.
  std::unordered_map<uint32_t, CallSiteInfo *> CSByOffset;
  for (auto &CS : BB.CallSites)
    CSByOffset.emplace(std::make_pair(CS.Offset, &CS));

  for (auto &MergedCS : MergedBB.CallSites) {
    auto CSI = CSByOffset.find(MergedCS.Offset);
    if (CSI == CSByOffset.end())
      continue;
    auto &CS = *CSI->second;
    if (CS != MergedCS)
      continue;

    MergedCS.Count += CS.Count;
    MergedCS.Mispreds += CS.Mispreds;

    CSByOffset.erase(CSI);
  }

  // Append the rest of call sites.
  for (auto CSI : CSByOffset) {
    MergedBB.CallSites.emplace_back(std::move(*CSI.second));
  }

  // Merge successor info.
  std::vector<SuccessorInfo *> SIByIndex(BF.NumBasicBlocks);
  for (auto &SI : BB.Successors) {
    if (SI.Index >= BF.NumBasicBlocks)
      report_error(BF.Name, "bad successor index");
    SIByIndex[SI.Index] = &SI;
  }
  for (auto &MergedSI : MergedBB.Successors) {
    if (!SIByIndex[MergedSI.Index])
      continue;
    auto &SI = *SIByIndex[MergedSI.Index];

    MergedSI.Count += SI.Count;
    MergedSI.Mispreds += SI.Mispreds;

    SIByIndex[MergedSI.Index] = nullptr;
  }
  for (auto *SI : SIByIndex) {
    if (SI) {
      MergedBB.Successors.emplace_back(std::move(*SI));
    }
  }
}

void mergeFunctionProfile(BinaryFunctionProfile &MergedBF,
                          BinaryFunctionProfile &&BF) {
  // Validate that we are merging the correct function.
  if (BF.NumBasicBlocks != MergedBF.NumBasicBlocks)
    report_error(BF.Name, "number of basic blocks mismatch");
  if (BF.Id != MergedBF.Id)
    report_error(BF.Name, "ID mismatch");
  if (BF.Hash != MergedBF.Hash)
    report_error(BF.Name, "hash mismatch");

  // Update the execution count.
  MergedBF.ExecCount += BF.ExecCount;

  // Merge basic blocks profile.
  std::vector<BinaryBasicBlockProfile *> BlockByIndex(BF.NumBasicBlocks);
  for (auto &BB : BF.Blocks) {
    if (BB.Index >= BF.NumBasicBlocks)
      report_error(BF.Name + " : BB #" + Twine(BB.Index),
                   "bad basic block index");
    BlockByIndex[BB.Index] = &BB;
  }
  for (auto &MergedBB : MergedBF.Blocks) {
    if (!BlockByIndex[MergedBB.Index])
      continue;
    auto &BB = *BlockByIndex[MergedBB.Index];

    mergeBasicBlockProfile(MergedBB, std::move(BB), MergedBF);

    // Ignore this block in the future.
    BlockByIndex[MergedBB.Index] = nullptr;
  }

  // Append blocks unique to BF (i.e. those that are not in MergedBF).
  for (auto *BB : BlockByIndex) {
    if (BB) {
      MergedBF.Blocks.emplace_back(std::move(*BB));
    }
  }
}

bool isYAML(const StringRef Filename) {
  auto MB = MemoryBuffer::getFileOrSTDIN(Filename);
  if (std::error_code EC = MB.getError())
    report_error(Filename, EC);
  auto Buffer = MB.get()->getBuffer();
  if (Buffer.startswith("---\n"))
    return true;
  return false;
}

void mergeLegacyProfiles(const cl::list<std::string> &Filenames) {
  errs() << "Using legacy profile format.\n";
  for (auto &Filename : Filenames) {
    if (isYAML(Filename))
      report_error(Filename, "cannot mix YAML and legacy formats");
    auto MB = MemoryBuffer::getFileOrSTDIN(Filename);
    if (std::error_code EC = MB.getError())
      report_error(Filename, EC);
    errs() << "Merging data from " << Filename << "...\n";
    outs() << MB.get()->getBuffer();
  }
  errs() << "Profile from " << Filenames.size() << " files merged.\n";
}

} // anonymous namespace

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.

  cl::HideUnrelatedOptions(opts::MergeFdataCategory);

  cl::ParseCommandLineOptions(argc, argv,
                              "merge multiple fdata into a single file");

  ToolName = argv[0];

  if (!isYAML(opts::InputDataFilenames.front())) {
    mergeLegacyProfiles(opts::InputDataFilenames);
    return 0;
  }

  // Merged header.
  BinaryProfileHeader MergedHeader;
  MergedHeader.Version = 1;

  // Merged information for all functions.
  StringMap<BinaryFunctionProfile> MergedBFs;

  for (auto &InputDataFilename : opts::InputDataFilenames) {
    auto MB = MemoryBuffer::getFileOrSTDIN(InputDataFilename);
    if (std::error_code EC = MB.getError())
      report_error(InputDataFilename, EC);
    yaml::Input YamlInput(MB.get()->getBuffer());

    errs() << "Merging data from " << InputDataFilename << "...\n";

    BinaryProfile BP;
    YamlInput >> BP;
    if (YamlInput.error())
      report_error(InputDataFilename, YamlInput.error());

    // Sanity check.
    if (BP.Header.Version != 1) {
      errs() << "Unable to merge data from profile using version "
             << BP.Header.Version << '\n';
      exit(1);
    }

    // Merge the header.
    mergeProfileHeaders(MergedHeader, BP.Header);

    // Do the function merge.
    for (auto &BF : BP.Functions) {
      if (!MergedBFs.count(BF.Name)) {
        MergedBFs.insert(std::make_pair(BF.Name, BF));
        continue;
      }

      auto &MergedBF = MergedBFs.find(BF.Name)->second;
      mergeFunctionProfile(MergedBF, std::move(BF));
    }
  }

  if (!opts::SuppressMergedDataOutput) {
    yaml::Output YamlOut(outs());

    BinaryProfile MergedProfile;
    MergedProfile.Header = MergedHeader;
    MergedProfile.Functions.resize(MergedBFs.size());
    std::transform(MergedBFs.begin(),
                   MergedBFs.end(),
                   MergedProfile.Functions.begin(),
                   [] (StringMapEntry<BinaryFunctionProfile> &V) {
                     return V.second;
                   });

    // For consistency, sort functions by their IDs.
    std::sort(MergedProfile.Functions.begin(), MergedProfile.Functions.end(),
              [] (const BinaryFunctionProfile &A,
                  const BinaryFunctionProfile &B) {
                return A.Id < B.Id;
              });

    YamlOut << MergedProfile;
  }

  errs() << "Data for " << MergedBFs.size()
         << " unique objects successfully merged.\n";

  if (opts::PrintFunctionList != opts::ST_NONE) {
    // List of function names with execution count.
    std::vector<std::pair<uint64_t, StringRef>> FunctionList(MergedBFs.size());
    using CountFuncType =
      std::function<std::pair<uint64_t, StringRef>(
          const StringMapEntry<BinaryFunctionProfile> &)>;
    CountFuncType ExecCountFunc =
        [](const StringMapEntry<BinaryFunctionProfile> &V) {
      return std::make_pair(V.second.ExecCount,
                            StringRef(V.second.Name));
    };
    CountFuncType BranchCountFunc =
        [](const StringMapEntry<BinaryFunctionProfile> &V) {
      // Return total branch count.
      uint64_t BranchCount = 0;
      for (const auto &BI : V.second.Blocks) {
        for (const auto &SI : BI.Successors) {
          BranchCount += SI.Count;
        }
      }
      return std::make_pair(BranchCount,
                            StringRef(V.second.Name));
    };

    CountFuncType CountFunc = (opts::PrintFunctionList == opts::ST_EXEC_COUNT)
       ? ExecCountFunc
       : BranchCountFunc;
    std::transform(MergedBFs.begin(),
                   MergedBFs.end(),
                   FunctionList.begin(),
                   CountFunc);
    std::stable_sort(FunctionList.rbegin(), FunctionList.rend());
    errs() << "Functions sorted by "
           << (opts::PrintFunctionList == opts::ST_EXEC_COUNT
                ? "execution"
                : "total branch")
           << " count:\n";
    for (auto &FI : FunctionList) {
      errs() << FI.second << " : " << FI.first << '\n';
    }
  }

  return 0;
}
