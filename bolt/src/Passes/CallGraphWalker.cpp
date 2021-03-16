//===--- CallGraphWalker.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "CallGraphWalker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Timer.h"

namespace opts {
extern llvm::cl::opt<bool> TimeOpts;
}

namespace llvm {
namespace bolt {

void CallGraphWalker::traverseCG() {
  NamedRegionTimer T1("CG Traversal", "CG Traversal", "CG breakdown",
                      "CG breakdown", opts::TimeOpts);
  std::queue<BinaryFunction *> Queue;
  std::set<BinaryFunction *> InQueue;

  for (auto *Func : TopologicalCGOrder) {
    Queue.push(Func);
    InQueue.insert(Func);
  }

  while (!Queue.empty()) {
    auto *Func = Queue.front();
    Queue.pop();
    InQueue.erase(Func);

    bool Changed{false};
    for (auto Visitor : Visitors) {
      bool CurVisit = Visitor(Func);
      Changed = Changed || CurVisit;
    }

    if (Changed) {
      for (auto CallerID : CG.predecessors(CG.getNodeId(Func))) {
        BinaryFunction *CallerFunc = CG.nodeIdToFunc(CallerID);
        if (InQueue.count(CallerFunc))
          continue;
        Queue.push(CallerFunc);
        InQueue.insert(CallerFunc);
      }
    }
  }
}

void CallGraphWalker::walk() {
  TopologicalCGOrder = CG.buildTraversalOrder();
  traverseCG();
}

}
}
