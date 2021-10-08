set(LLVM_LINK_COMPONENTS Support)

add_llvm_tool(merge-fdata
  merge-fdata.cpp

  DEPENDS
  intrinsics_gen
)

#include_directories(${BOLT_SOURCE_DIR}/include)
