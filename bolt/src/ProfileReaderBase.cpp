//===-- ProfileReaderBase.cpp - Base class for profile readers --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Interface to be implemented by all profile readers.
//
//===----------------------------------------------------------------------===//

#include "ProfileReaderBase.h"
#include "BinaryFunction.h"

namespace llvm {
namespace bolt {

bool ProfileReaderBase::mayHaveProfileData(const BinaryFunction &BF) {
  return true;
}

}
}
