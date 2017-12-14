//===-- ProfileYAMLMapping.h - mappings for BOLT profile --------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implement mapping between binary function profile and YAML representation.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_PROFILEYAMLMAPPING_H
#define LLVM_TOOLS_LLVM_BOLT_PROFILEYAMLMAPPING_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/YAMLTraits.h"
#include <vector>

namespace llvm {
namespace yaml {

namespace bolt {
struct CallSiteInfo {
  llvm::yaml::Hex32 Offset{0};
  uint32_t DestId{0};
  uint32_t EntryDiscriminator{0}; // multiple entry discriminator
  uint64_t Count{0};
  uint64_t Mispreds{0};

  bool operator==(const CallSiteInfo &Other) const {
    return Offset == Other.Offset &&
           DestId == Other.DestId &&
           EntryDiscriminator == Other.EntryDiscriminator;
  }
};
}

template <> struct MappingTraits<bolt::CallSiteInfo> {
  static void mapping(IO &YamlIO, bolt::CallSiteInfo &CSI) {
    YamlIO.mapRequired("off", CSI.Offset);
    YamlIO.mapRequired("fid", CSI.DestId);
    YamlIO.mapOptional("disc", CSI.EntryDiscriminator, (uint32_t)0);
    YamlIO.mapRequired("cnt", CSI.Count);
    YamlIO.mapOptional("mis", CSI.Mispreds, (uint64_t)0);
  }

  static const bool flow = true;
};

namespace bolt {
struct SuccessorInfo {
  uint32_t Index{0};
  uint64_t Count{0};
  uint64_t Mispreds{0};

  bool operator==(const SuccessorInfo &Other) const {
    return Index == Other.Index;
  }
};
}

template <> struct MappingTraits<bolt::SuccessorInfo> {
  static void mapping(IO &YamlIO, bolt::SuccessorInfo &SI) {
    YamlIO.mapRequired("bid", SI.Index);
    YamlIO.mapRequired("cnt", SI.Count);
    YamlIO.mapOptional("mis", SI.Mispreds, (uint64_t)0);
  }

  static const bool flow = true;
};

} // end namespace yaml
} // end namespace llvm

LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(llvm::yaml::bolt::CallSiteInfo)
LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(llvm::yaml::bolt::SuccessorInfo)

namespace llvm {
namespace yaml {

namespace bolt {
struct BinaryBasicBlockProfile {
  uint32_t Index{0};
  uint32_t NumInstructions{0};
  llvm::yaml::Hex64 Hash{0};
  uint64_t ExecCount{0};
  std::vector<CallSiteInfo> CallSites;
  std::vector<SuccessorInfo> Successors;

  bool operator==(const BinaryBasicBlockProfile &Other) const {
    return Index == Other.Index;
  }
};
} // namespace bolt

template <> struct MappingTraits<bolt::BinaryBasicBlockProfile> {
  static void mapping(IO &YamlIO, bolt::BinaryBasicBlockProfile &BBP) {
    YamlIO.mapRequired("bid", BBP.Index);
    YamlIO.mapRequired("insns", BBP.NumInstructions);
    YamlIO.mapOptional("exec", BBP.ExecCount, (uint64_t)0);
    YamlIO.mapOptional("calls", BBP.CallSites,
                       std::vector<bolt::CallSiteInfo>());
    YamlIO.mapOptional("succ", BBP.Successors,
                       std::vector<bolt::SuccessorInfo>());
  }
};

} // end namespace yaml
} // end namespace llvm

LLVM_YAML_IS_SEQUENCE_VECTOR(llvm::yaml::bolt::BinaryBasicBlockProfile)

namespace llvm {
namespace yaml {

namespace bolt {
struct BinaryFunctionProfile {
  std::string Name;
  uint32_t NumBasicBlocks;
  uint32_t Id;
  llvm::yaml::Hex64 Hash;
  uint64_t ExecCount;
  std::vector<BinaryBasicBlockProfile> Blocks;
  bool Used{false};
};
}

template <> struct MappingTraits<bolt::BinaryFunctionProfile> {
  static void mapping(IO &YamlIO, bolt::BinaryFunctionProfile &BFP) {
    YamlIO.mapRequired("name", BFP.Name);
    YamlIO.mapRequired("fid", BFP.Id);
    YamlIO.mapRequired("hash", BFP.Hash);
    YamlIO.mapRequired("exec", BFP.ExecCount);
    YamlIO.mapRequired("nblocks", BFP.NumBasicBlocks);
    YamlIO.mapOptional("blocks", BFP.Blocks,
                        std::vector<bolt::BinaryBasicBlockProfile>());
  }
};

} // end namespace yaml
} // end namespace llvm

LLVM_YAML_IS_DOCUMENT_LIST_VECTOR(llvm::yaml::bolt::BinaryFunctionProfile)

#endif
