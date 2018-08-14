//===-- DataAggregator.h - Perf data aggregator -----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This family of functions reads profile data written by perf record,
// aggregates it and then writes it back to an output file.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_DATA_AGGREGATOR_H
#define LLVM_TOOLS_LLVM_BOLT_DATA_AGGREGATOR_H

#include "DataReader.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include <map>
#include <unordered_map>

namespace llvm {
namespace bolt {

class BinaryFunction;
class BinaryContext;

struct PerfBranchSample {
  SmallVector<LBREntry, 16> LBR;
};

struct PerfBasicSample {
  StringRef EventName;
  uint64_t PC;
};

struct PerfMemSample {
  uint64_t PC;
  uint64_t Addr;
};

/// Used for parsing specific pre-aggregated input files.
struct AggregatedLBREntry {
  enum Type : char { BRANCH = 0, FT, FT_EXTERNAL_ORIGIN };
  Location From;
  Location To;
  uint64_t Count;
  uint64_t Mispreds;
  Type EntryType;
};

/// DataAggregator inherits all parsing logic from DataReader as well as
/// its data structures used to represent aggregated profile data in memory.
///
/// The aggregator works by dispatching two separate perf-script jobs that
/// read perf samples and perf task annotations. Later, we read the output
/// files to extract information about which PID was used for this binary.
/// With the PID, we filter the samples and extract all LBR entries.
///
/// To aggregate LBR entries, we rely on a BinaryFunction map to locate the
/// original function where the event happened. Then, we convert a raw address
/// to an offset relative to the start of this function and aggregate branch
/// information for each function.
///
/// This must be coordinated with RewriteInstance so we have BinaryFunctions in
/// State::Disassembled. After this state, BinaryFunction will drop the
/// instruction map with original addresses we rely on to validate the traces
/// found in the LBR.
///
/// The last step is to write the aggregated data to disk in the output file
/// specified by the user.
class DataAggregator : public DataReader {
  // Perf process spawning bookkeeping
  std::string PerfPath;
  sys::ProcessInfo BranchEventsPI;
  sys::ProcessInfo MemEventsPI;
  sys::ProcessInfo MMapEventsPI;
  SmallVector<char, 256> PerfBranchEventsOutputPath;
  SmallVector<char, 256> PerfBranchEventsErrPath;
  SmallVector<char, 256> PerfMemEventsOutputPath;
  SmallVector<char, 256> PerfMemEventsErrPath;
  SmallVector<char, 256> PerfMMapEventsOutputPath;
  SmallVector<char, 256> PerfMMapEventsErrPath;

  /// Whether aggregator was scheduled to run
  bool Enabled{false};

  /// Input perf.data file
  StringRef PerfDataFilename;

  /// Output file name to write aggregated fdata to
  StringRef OutputFDataName;

  /// Our sampled binary name to look for in perf.data
  std::string BinaryName;

  /// Name of the binary with matching build-id from perf.data if different
  /// from BinaryName.
  std::string BuildIDBinaryName;

  /// Memory map info for a single file
  struct MMapInfo {
    int64_t PID{-1LL};
    uint64_t BaseAddress;
    uint64_t Size;
  };

  /// Per-PID map info for the binary
  std::unordered_map<uint64_t, MMapInfo> BinaryMMapInfo;

  /// References to core BOLT data structures
  BinaryContext *BC{nullptr};
  std::map<uint64_t, BinaryFunction> *BFs{nullptr};

  /// Aggregation statistics
  uint64_t NumInvalidTraces{0};
  uint64_t NumLongRangeTraces{0};

  /// Looks into system PATH for Linux Perf and set up the aggregator to use it
  void findPerfExecutable();

  /// Launch a subprocess to read all perf branch samples and write them to an
  /// output file we will parse later
  bool launchPerfBranchEventsNoWait();

  /// Launch a subprocess to read all perf memory event samples and write them
  /// to an output file we will parse later
  bool launchPerfMemEventsNoWait();

  /// Launch a subprocess to read memory mapping for the binary. We later use
  /// PIDs to filter samples, and memory mapping to adjust addresses.
  bool launchPerfMMapEventsNoWait();

  /// Delete all temporary files created to hold the output generated by spawned
  /// subprocesses during the aggregation job
  void deleteTempFiles();
  void deleteTempFile(StringRef File);

  // Semantic pass helpers
  /// Look up which function contains an address by using out map of
  /// disassembled BinaryFunctions
  BinaryFunction *getBinaryFunctionContainingAddress(uint64_t Address);

  /// Semantic actions - parser hooks to interpret parsed perf samples
  /// Register a sample (non-LBR mode), i.e. a new hit at \p Address
  bool doSample(BinaryFunction &Func, const uint64_t Address);

  /// Register an intraprocedural branch \p Branch.
  bool doIntraBranch(BinaryFunction &Func, uint64_t From, uint64_t To,
                     uint64_t Count, uint64_t Mispreds);

  /// Register an interprocedural branch from \p FromFunc to \p ToFunc with
  /// offsets \p From and \p To, respectively.
  bool doInterBranch(BinaryFunction *FromFunc, BinaryFunction *ToFunc,
                     uint64_t From, uint64_t To, uint64_t Count,
                     uint64_t Mispreds);

  /// Register a \p Branch.
  bool doBranch(uint64_t From, uint64_t To, uint64_t Count, uint64_t Mispreds);

  /// Register a trace between two LBR entries supplied in execution order.
  bool doTrace(const LBREntry &First, const LBREntry &Second,
               uint64_t Count = 1);

  /// Parser helpers
  /// Return false if we exhausted our parser buffer and finished parsing
  /// everything
  bool hasData();

  /// Parse a single perf sample containing a PID associated with a sequence of
  /// LBR entries
  ErrorOr<PerfBranchSample> parseBranchSample();

  /// Parse a single perf sample containing a PID associated with an event name
  /// and a PC
  ErrorOr<PerfBasicSample> parseBasicSample();

  /// Parse a single perf sample containing a PID associated with an IP and
  /// address.
  ErrorOr<PerfMemSample> parseMemSample();

  /// Parse pre-aggregated LBR samples created by an external tool
  ErrorOr<AggregatedLBREntry> parseAggregatedLBREntry();

  /// Parse either buildid:offset or just offset, representing a location in the
  /// binary. Used exclusevely for pre-aggregated LBR samples.
  ErrorOr<Location> parseLocationOrOffset();

  /// Check if a field separator is the next char to parse and, if yes, consume
  /// it and return true
  bool checkAndConsumeFS();

  /// Consume the entire line
  void consumeRestOfLine();

  /// Parse a single LBR entry as output by perf script -Fbrstack
  ErrorOr<LBREntry> parseLBREntry();

  /// Parse the full output generated by perf script to report LBR samples.
  std::error_code parseBranchEvents();

  /// Parse the full output generated by perf script to report non-LBR samples.
  std::error_code parseBasicEvents();

  /// Parse the full output generated by perf script to report memory events.
  std::error_code parseMemEvents();

  /// Parse the full output of pre-aggregated LBR samples generated by
  /// an external tool.
  std::error_code parseAggregatedLBRSamples();

  /// Parse a single line of a PERF_RECORD_MMAP2 event looking for a mapping
  /// between the binary name and its memory layout in a process with a given
  /// PID.
  /// On success return a <FileName, MMapInfo> pair.
  ErrorOr<std::pair<StringRef, MMapInfo>> parseMMapEvent();

  /// Parse the full output generated by `perf script --show-mmap-events`
  /// to generate mapping between binary files and their memory mappings for
  /// all PIDs.
  std::error_code parseMMapEvents();

  /// Parse a single pair of binary full path and associated build-id
  Optional<std::pair<StringRef, StringRef>> parseNameBuildIDPair();

  /// Parse the output generated by "perf buildid-list" to extract build-ids
  /// and return a file name matching a given \p FileBuildID.
  Optional<StringRef> getFileNameForBuildID(StringRef FileBuildID);

  /// Coordinate reading and parsing of pre-aggregated file
  ///
  /// The regular perf2bolt aggregation job is to read perf output directly.
  /// However, if the data is coming from a database instead of perf, one could
  /// write a query to produce a pre-aggregated file. This function deals with
  /// this case.
  ///
  /// The pre-aggregated file contains aggregated LBR data, but without binary
  /// knowledge. BOLT will parse it and, using information from the disassembled
  /// binary, augment it with fall-through edge frequency information. After
  /// this step is finished, this data can be either written to disk to be
  /// consumed by BOLT later, or can be used by BOLT immediately if kept in
  /// memory.
  ///
  /// File format syntax:
  /// {B|F|f} [<start_id>:]<start_offset> [<end_id>:]<end_offset> <count>
  ///       [<mispred_count>]
  ///
  /// B - indicates an aggregated branch
  /// F - an aggregated fall-through
  /// f - an aggregated fall-through with external origin - used to disambiguate
  ///       between a return hitting a basic block head and a regular internal
  ///       jump to the block
  ///
  /// <start_id> - build id of the object containing the start address. We can
  /// skip it for the main binary and use "X" for an unknown object. This will
  /// save some space and facilitate human parsing.
  ///
  /// <start_offset> - hex offset from the object base load address (0 for the
  /// main executable unless it's PIE) to the start address.
  ///
  /// <end_id>, <end_offset> - same for the end address.
  ///
  /// <count> - total aggregated count of the branch or a fall-through.
  ///
  /// <mispred_count> - the number of times the branch was mispredicted.
  /// Omitted for fall-throughs.
  ///
  /// Example:
  /// F 41be50 41be50 3
  /// F 41be90 41be90 4
  /// B 4b1942 39b57f0 3 0
  /// B 4b196f 4b19e0 2 0
  bool processPreAggregated();

  /// If \p Address falls into to the binary address space based on memory
  /// mapping info \p MMI, then adjust it for further processing by subtracting
  /// the base load address. External addresses, i.e. addresses that do not
  /// correspond to the binary allocated address space, are adjusted to avoid
  /// conflicts.
  void adjustAddress(uint64_t &Address, const MMapInfo &MMI) const {
    if (Address >= MMI.BaseAddress && Address < MMI.BaseAddress + MMI.Size) {
      Address -= MMI.BaseAddress;
    } else if (Address < MMI.Size) {
      // Make sure the address is not treated as belonging to the binary.
      Address = (-1ULL);
    }
  }

  /// Adjust addresses in \p LBR entry.
  void adjustLBR(LBREntry &LBR, const MMapInfo &MMI) const {
    adjustAddress(LBR.From, MMI);
    adjustAddress(LBR.To, MMI);
  }

public:
  DataAggregator(raw_ostream &Diag, StringRef BinaryName)
      : DataReader(Diag), BinaryName(llvm::sys::path::filename(BinaryName)) {}

  /// Set the file name to save aggregate data to
  void setOutputFDataName(StringRef Name) { OutputFDataName = Name; }

  /// Start an aggregation job asynchronously. Call "aggregate" to finish it
  /// with a list of disassembled functions.
  void start(StringRef PerfDataFilename);

  /// True if DataAggregator has asynchronously been started and an aggregation
  /// job is in progress
  bool started() const { return Enabled; }

  /// Force all subprocesses to stop and cancel aggregation
  void abort();

  /// Dump data structures into a file readable by llvm-bolt
  std::error_code writeAggregatedFile() const;

  /// Join child subprocesses and finalize aggregation populating data
  /// structures
  bool aggregate(BinaryContext &BC, std::map<uint64_t, BinaryFunction> &BFs);

  /// Check whether \p FileName is a perf.data file
  static bool checkPerfDataMagic(StringRef FileName);

  /// If we have a build-id available for the input file, use it to assist
  /// matching profile to a binary.
  ///
  /// If the binary name changed after profile collection, use build-id
  /// to get the proper name in perf data when build-ids are available.
  /// If \p FileBuildID has no match, then issue an error and exit.
  void processFileBuildID(StringRef FileBuildID);

  /// Debugging dump methods
  void dump() const;
  void dump(const LBREntry &LBR) const;
  void dump(const PerfBranchSample &Sample) const;
  void dump(const PerfMemSample &Sample) const;
};
}
}

#endif
