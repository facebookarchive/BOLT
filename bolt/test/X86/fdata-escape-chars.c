volatile int var;

void symb_w_whitespace() { var = 1; }

void symb_backslash_b() {
  symb_w_whitespace();
  var = 2;
}

static void static_symb_backslash_b() {
  symb_w_whitespace();
  var = 3;
}

int main() {
  static_symb_backslash_b();
  symb_w_whitespace();
  for (int i = 0; i < 2; ++i)
    symb_backslash_b();
}

// REQUIRES: system-linux

// RUN: %host_cc %cflags -O0 %s -o %t.exe -Wl,-q
// RUN: llvm-objcopy --redefine-syms=%p/Inputs/fdata-escape-chars-syms.txt %t.exe
//
// RUN: llvm-bolt %t.exe -o %t.exe.instrumented -instrument \
// RUN:   -instrumentation-file=%t.fdata
// RUN: %t.exe.instrumented
// RUN: cat %t.fdata | \
// RUN    FileCheck --check-prefix="FDATA_CHECK" %s
// RUN: llvm-bolt %t.exe -o %t.fdata.exe -data %t.fdata -print-finalized | \
// RUN:   FileCheck --check-prefix="INSTR_CHECK" %s
//
// RUN: link_fdata %p/Inputs/fdata-escape-chars.txt %t.exe %t.pre "PREAGR"
// RUN: perf2bolt %t.exe -o %t.pre.fdata -pa -p %t.pre
// RUN: cat %t.pre.fdata | FileCheck --check-prefix="PREAGR_FDATA_CHECK" %s
// RUN: llvm-bolt %t.exe -o %t.pre.fdata.exe -data %t.pre.fdata -print-finalized | \
// RUN:   FileCheck --check-prefix="PREAGR_CHECK" %s

// FDATA_CHECK: 1 symb\ backslash\\ {{([[:xdigit:]]+)}} 1 symb\ whitespace 0 0 2
// FDATA_CHECK: 1 main {{([[:xdigit:]]+)}} 1 symb\ whitespace 0 0 1
// FDATA_CHECK: 1 main {{([[:xdigit:]]+)}} 1 symb\ backslash\\ 0 0 2

// INSTR_CHECK: Binary Function "symb whitespace"
// INSTR_CHECK-DAG: Exec Count  : 4
// INSTR_CHECK: Binary Function "symb backslash\"
// INSTR_CHECK-DAG: Exec Count  : 2
// INSTR_CHECK-DAG: {{([[:xdigit:]]+)}}:   callq   "symb whitespace" # Count: 2
// INSTR_CHECK: Binary Function "static symb backslash\/1(*2)"
// INSTR_CHECK-DAG: Exec Count  : 1
// INSTR_CHECK-DAG: {{([[:xdigit:]]+)}}:   callq   "symb whitespace" # Count: 1
// INSTR_CHECK: Binary Function "main"
// INSTR_CHECK-DAG: Exec Count  : 1
// INSTR_CHECK-DAG: {{([[:xdigit:]]+)}}:   callq   "symb whitespace" # Count: 1
// INSTR_CHECK-DAG: {{([[:xdigit:]]+)}}:   callq   "symb backslash\" # Count: 2

// PREAGR_FDATA_CHECK: 1 symb\ backslash\\ 0 1 symb\ whitespace 0 0 2
// PREAGR_FDATA_CHECK: 1 main 0 1 static\ symb\ backslash\\/1 0 0 1
// PREAGR_FDATA_CHECK: 1 main 0 1 symb\ whitespace 0 0 1
// PREAGR_FDATA_CHECK: 1 main 0 1 symb\ backslash\\ 0 0 2
// PREAGR_FDATA_CHECK: 1 static\ symb\ backslash\\/1 0 1 symb\ whitespace 0 0 1

// PREAGR_CHECK: Binary Function "symb whitespace"
// PREAGR_CHECK-DAG: Exec Count  : 4
// PREAGR_CHECK: Binary Function "symb backslash\"
// PREAGR_CHECK-DAG: Exec Count  : 2
// PREAGR_CHECK: Binary Function "static symb backslash\/1(*2)"
// PREAGR_CHECK-DAG: Exec Count  : 1
