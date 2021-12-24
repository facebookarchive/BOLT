//===-- Heatmap.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "Heatmap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>

#define DEBUG_TYPE "bolt-heatmap"

using namespace llvm;

namespace opts {

extern cl::SubCommand HeatmapCommand;

static cl::opt<unsigned>
BucketsPerLine("line-size",
  cl::desc("number of entries per line (default 256)"),
  cl::init(256),
  cl::Optional,
  cl::sub(HeatmapCommand));

}

namespace llvm {
namespace bolt {

void Heatmap::registerAddressRange(uint64_t StartAddress, uint64_t EndAddress,
                                   uint64_t Count) {
  if (ignoreAddress(StartAddress)) {
    ++NumSkippedRanges;
    return;
  }

  if (StartAddress > EndAddress ||
      EndAddress - StartAddress > 64 * 1024) {
    DEBUG(dbgs() << "invalid range : 0x" << Twine::utohexstr(StartAddress)
                 << " -> 0x" << Twine::utohexstr(EndAddress) << '\n');
    ++NumSkippedRanges;
    return;
  }

  for (uint64_t Bucket = StartAddress / BucketSize;
       Bucket <= EndAddress / BucketSize; ++Bucket) {
    Map[Bucket] += Count;
  }
}

void Heatmap::print(StringRef FileName) const {
  std::error_code EC;
  raw_fd_ostream OS(FileName, EC, sys::fs::OpenFlags::F_None);
  if (EC) {
    errs() << "error opening output file: " << EC.message() << '\n';
    exit(1);
  }
  print(OS);
}

void Heatmap::print(raw_ostream &OS) const {
  const char FillChar = '.';

  const auto DefaultColor = raw_ostream::WHITE;
  auto changeColor = [&](raw_ostream::Colors Color) -> void {
    static auto CurrentColor = raw_ostream::BLACK;
    if (CurrentColor == Color)
      return;
    OS.changeColor(Color);
    CurrentColor = Color;
  };

  const uint64_t BytesPerLine = opts::BucketsPerLine * BucketSize;

  // Calculate the max value for scaling.
  uint64_t MaxValue = 0;
  for (auto &Entry : Map) {
    MaxValue = std::max<uint64_t>(MaxValue, Entry.second);
  }

  // Print start of the line and fill it with an empty space right before
  // the Address.
  auto startLine = [&](uint64_t Address, bool Empty = false) {
    changeColor(DefaultColor);
    const auto LineAddress = Address / BytesPerLine * BytesPerLine;

    if (MaxAddress > 0xffffffff)
      OS << format("0x%016" PRIx64 ": ", LineAddress);
    else
      OS << format("0x%08" PRIx64 ": ", LineAddress);

    if (Empty)
      Address = LineAddress + BytesPerLine;
    for (auto Fill = LineAddress; Fill < Address; Fill += BucketSize) {
      OS << FillChar;
    }
  };

  // Finish line after \p Address was printed.
  auto finishLine = [&](uint64_t Address) {
    const auto End = alignTo(Address + 1, BytesPerLine);
    for (auto Fill = Address + BucketSize; Fill < End; Fill += BucketSize)
      OS << FillChar;
    OS << '\n';
  };

  // Fill empty space in (Start, End) range.
  auto fillRange = [&](uint64_t Start, uint64_t End) {
    if ((Start / BytesPerLine) == (End / BytesPerLine)) {
      for (auto Fill = Start + BucketSize; Fill < End; Fill += BucketSize) {
        changeColor(DefaultColor);
        OS << FillChar;
      }
      return;
    }

    changeColor(DefaultColor);
    finishLine(Start);
    Start = alignTo(Start, BytesPerLine);

    uint64_t NumEmptyLines = (End - Start) / BytesPerLine;

    if (NumEmptyLines > 32) {
      OS << '\n';
    } else {
      while (NumEmptyLines--) {
        startLine(Start, /*Empty=*/true);
        OS << '\n';
        Start += BytesPerLine;
      }
    }

    startLine(End);
  };

  static raw_ostream::Colors Colors[] = {
    raw_ostream::WHITE,
    raw_ostream::WHITE,
    raw_ostream::CYAN,
    raw_ostream::GREEN,
    raw_ostream::YELLOW,
    raw_ostream::RED
  };
  constexpr size_t NumRanges = sizeof(Colors) / sizeof(Colors[0]);

  uint64_t Range[NumRanges];
  for (uint64_t I = 0; I < NumRanges; ++I)
    Range[I] = std::max(I + 1,
                        (uint64_t) std::pow((double) MaxValue,
                                            (double) (I + 1) / NumRanges));
  Range[NumRanges - 1] = std::max((uint64_t) NumRanges, MaxValue);

  // Print scaled value
  auto printValue = [&](uint64_t Value, bool ResetColor = false) {
    assert(Value && "should only print positive values");
    for (unsigned I = 0; I < sizeof(Range) / sizeof(Range[0]); ++I) {
      if (Value <= Range[I]) {
        changeColor(Colors[I]);
        break;
      }
    }
    if (Value <= Range[0]) {
      OS << 'o';
    } else {
      OS << 'O';
    }
    if (ResetColor)
      changeColor(DefaultColor);
  };

  // Print against black background
  OS.changeColor(raw_ostream::BLACK, /*Bold=*/false, /*Background=*/true);
  changeColor(DefaultColor);

  // Print map legend
  OS << "Legend:\n";
  uint64_t PrevValue = 0;
  for (unsigned I = 0; I < sizeof(Range) / sizeof(Range[0]); ++I) {
    const auto Value = Range[I];
    OS << "  ";
    printValue(Value, true);
    OS << " : (" << PrevValue << ", " << Value << "]\n";
    PrevValue = Value;
  }

  // Pos - character position from right in hex form.
  auto printHeader = [&](unsigned Pos) {
    OS << "            ";
    if (MaxAddress > 0xffffffff)
      OS << "        ";
    unsigned PrevValue = unsigned(-1);
    for (unsigned I = 0; I < BytesPerLine; I += BucketSize) {
      const auto Value = (I & ((1 << Pos * 4) - 1)) >> (Pos - 1) * 4;
      if (Value != PrevValue) {
        OS << Twine::utohexstr(Value);
        PrevValue = Value;
      } else {
        OS << ' ';
      }
    }
    OS << '\n';
  };
  for (unsigned I = 5; I > 0; --I)
    printHeader(I);

  uint64_t PrevAddress = 0;
  for (auto MI = Map.begin(), ME = Map.end(); MI != ME; ++MI) {
    auto &Entry = *MI;
    uint64_t Address = Entry.first * BucketSize;

    if (PrevAddress) {
      fillRange(PrevAddress, Address);
    } else {
      startLine(Address);
    }

    printValue(Entry.second);

    PrevAddress = Address;
  }

  if (PrevAddress) {
    changeColor(DefaultColor);
    finishLine(PrevAddress);
  }
}

} // namespace bolt
} // namespace llvm
