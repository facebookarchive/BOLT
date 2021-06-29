//===--- Utils.cpp - Common helper functions ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Common helper functions.
//
//===----------------------------------------------------------------------===//

#include "Utils.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace bolt {

void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << "BOLT-ERROR: '" << Message << "': " << EC.message() << ".\n";
  exit(1);
}

void report_error(StringRef Message, Error E) {
  assert(E);
  errs() << "BOLT-ERROR: '" << Message << "': " << toString(std::move(E))
         << ".\n";
  exit(1);
}

void check_error(std::error_code EC, StringRef Message) {
  if (!EC)
    return;
  report_error(Message, EC);
}

void check_error(Error E, Twine Message) {
  if (!E)
    return;
  handleAllErrors(std::move(E), [&](const llvm::ErrorInfoBase &EIB) {
    llvm::errs() << "BOLT-ERROR: '" << Message << "': " << EIB.message()
                 << '\n';
    exit(1);
  });
}

std::string getEscapedName(const StringRef &Name) {
  std::string Output = Name.str();
  for (size_t I = 0; I < Output.size(); ++I) {
    if (Output[I] == ' ' || Output[I] == '\\')
      Output.insert(I++, 1, '\\');
  }

  return Output;
}

std::string getUnescapedName(const StringRef &Name) {
  std::string Output = Name.str();
  for (size_t I = 0; I < Output.size(); ++I) {
    if (Output[I] == '\\')
      Output.erase(I++, 1);
  }

  return Output;
}

} // namespace bolt
} // namespace llvm
