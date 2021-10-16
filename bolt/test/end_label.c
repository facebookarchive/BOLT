// Check that the end of section label was updated

#include <stdio.h>

int main() {
    extern char __etext;
    printf("%p\n", &__etext);
}

// REQUIRES: system-linux

// RUN: %clang %cflags -Wl,-q %s -o %t.exe
// RUN: llvm-bolt %t.exe -o %t.bolt -use-old-text=0 -lite=0
// RUN: printf "%x" $(($(llvm-objdump -h -j .text %t.bolt | grep ".text" | \
// RUN:   awk '{ printf "0x%%s+0x%%s", $3, $4; }'))) &> %t.section
// RUN: printf "%x" $(($(llvm-nm %t.bolt | grep __etext | \
// RUN:   awk '{ printf "0x%%s", $1; }'))) &> %t.etext
// RUN: diff %t.section %t.etext
