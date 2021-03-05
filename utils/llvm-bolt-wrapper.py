#!/usr/bin/env python3
import argparse
import subprocess
from typing import *
import tempfile
import copy
import os
import shutil
import sys
import re
import configparser

# USAGE:
# 0. Prepare two BOLT build versions: base and compare
# In base BOLT build directory:
# 1. Rename `llvm-bolt` to `llvm-bolt.real`
# 2. Create a symlink from this script to `llvm-bolt`
# 3. Create `llvm-bolt-wrapper.ini` and fill it using the example below.
#
# This script will compare binaries produced by base and compare BOLT, and
# report elapsed processing time and max RSS.

# read options from config file llvm-bolt-wrapper.ini in script CWD
#
# [config]
# # mandatory
# base_bolt = /full/path/to/llvm-bolt.real
# cmp_bolt = /full/path/to/other/llvm-bolt
# # optional, default to False
# verbose
# keep_tmp
# no_minimize
# run_sequentially
# compare_output
# skip_binary_cmp
# # optional, defaults to timing.log in CWD
# timing_file = timing1.log

cfg = configparser.ConfigParser(allow_no_value = True)
cfgs = cfg.read("llvm-bolt-wrapper.ini")
assert cfgs, f"llvm-bolt-wrapper.ini is not found in {os.getcwd()}"

# set up bolt locations
BASE_BOLT = cfg['config']['base_bolt']
CMP_BOLT = cfg['config']['cmp_bolt']

def get_cfg(key):
    # if key is not present in config, assume False
    if key not in cfg['config']:
        return False
    # if key is present, but has no value, assume True
    if not cfg['config'][key]:
        return True
    # if key has associated value, interpret the value
    return cfg['config'].getboolean(key)

# optional
VERBOSE = get_cfg('verbose')
KEEP_TMP = get_cfg('keep_tmp')
NO_MINIMIZE = get_cfg('no_minimize')
RUN_SEQUENTIALLY = get_cfg('run_sequentially')
COMPARE_OUTPUT = get_cfg('compare_output')
SKIP_BINARY_CMP = get_cfg('skip_binary_cmp')
TIMING_FILE = cfg['config'].get('timing_file', 'timing.log')

# perf2bolt mode
PERF2BOLT_MODE = ['-aggregate-only']

# options to suppress binary differences as much as possible
MINIMIZE_DIFFS = ['-bolt-info=0']

# bolt output options that need to be intercepted
BOLT_OUTPUT_OPTS = {
    '-o': 'BOLT output binary',
    '-w': 'BOLT recorded profile',
}

# regex patterns to exclude the line from log comparison
SKIP_MATCH = [
    'BOLT-INFO: BOLT version',
    '^Args: ',
    '^BOLT-DEBUG:',
    'BOLT-INFO:.*data.*output data',
    'WARNING: reading perf data directly',
]

def run_cmd(cmd):
    if VERBOSE:
        print(cmd)
    return subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)

def run_bolt(bolt_path, bolt_args):
    p2b = os.path.basename(sys.argv[0]) == 'perf2bolt' # perf2bolt mode
    cmd = ['/usr/bin/time', '-f', '%e %M', bolt_path] + bolt_args
    if p2b:
        cmd += PERF2BOLT_MODE
    elif not NO_MINIMIZE:
        cmd += MINIMIZE_DIFFS
    return run_cmd(cmd)

def prepend_dash(args: Mapping[AnyStr, AnyStr]) -> Sequence[AnyStr]:
    '''
    Accepts parsed arguments and returns flat list with dash prepended to
    the option.
    Example: Namespace(o='test.tmp') -> ['-o', 'test.tmp']
    '''
    dashed = [('-'+key,value) for (key,value) in args.items()]
    flattened = list(sum(dashed, ()))
    return flattened

def replace_cmp_path(tmp: AnyStr, args: Mapping[AnyStr, AnyStr]) -> Sequence[AnyStr]:
    '''
    Keeps file names, but replaces the path to a temp folder.
    Example: Namespace(o='abc/test.tmp') -> Namespace(o='/tmp/tmpf9un/test.tmp')
    '''
    new_args = {key: os.path.join(tmp, os.path.basename(value))
                for key, value in args.items()}
    return prepend_dash(new_args)

def preprocess_args(args: argparse.Namespace) -> Mapping[AnyStr, AnyStr]:
    '''
    Drop options that weren't parsed (e.g. -w), convert to a dict
    '''
    return {key: value for key, value in vars(args).items() if value}

def write_to(txt, filename, mode='w'):
    with open(filename, mode) as f:
        f.write(txt)

def wait(proc):
    try:
        out, err = proc.communicate(timeout=9000)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    return out, err

def compare_logs(main, cmp, skip_end=0):
    '''
    Compares logs but allows for certain lines to be excluded from comparison.
    Returns None on success, mismatch otherwise.
    '''
    for lhs, rhs in list(zip(main.splitlines(), cmp.splitlines()))[:-skip_end]:
        if lhs != rhs:
            # check skip patterns
            for skip in SKIP_MATCH:
                # both lines must contain the pattern
                if re.search(skip, lhs) and re.search(skip, rhs):
                    break
            # otherwise return mismatching lines
            else:
                return (lhs, rhs)
    return None

def fmt_cmp(cmp_tuple):
    if not cmp_tuple:
        return ''
    return f'main:\n{cmp_tuple[0]}\ncmp:\n{cmp_tuple[1]}\n'

def compare_headers(lhs, rhs):
    '''
    Compares ELF headers in two binaries.
    '''
    def readelf(binary):
        return subprocess.run(['readelf', '-We', binary],
                              text=True, check=True,
                              capture_output=True)
    readelf_lhs = readelf(lhs)
    readelf_rhs = readelf(rhs)
    return compare_logs(readelf_lhs.stdout, readelf_rhs.stdout)

def parse_cmp_offset(cmp_out):
    '''
    Extracts byte number from cmp output:
    file1 file2 differ: byte X, line Y
    '''
    return int(re.search(r'byte (\d+),', cmp_out).groups()[0])

def report_real_time(binary, main_err, cmp_err):
    '''
    Extracts real time from stderr and appends it to TIMING FILE it as csv:
    "output binary; base bolt; cmp bolt"
    '''
    def get_real_from_stderr(log):
        return '; '.join(log.splitlines()[-1].split())
    main = get_real_from_stderr(main_err)
    cmp = get_real_from_stderr(cmp_err)
    write_to(f"{binary}; {main}; {cmp}\n", TIMING_FILE, 'a')

def main():
    # intercept output arguments
    parser = argparse.ArgumentParser(add_help=False)
    for option, help in BOLT_OUTPUT_OPTS.items():
        parser.add_argument(option, help=help)
    args, unknownargs = parser.parse_known_args()
    args = preprocess_args(args)
    cmp_args = copy.deepcopy(args)
    tmp = tempfile.mkdtemp()
    cmp_args = replace_cmp_path(tmp, cmp_args)

    # reconstruct output arguments: prepend dash
    args = prepend_dash(args)

    # run both BOLT binaries
    main_bolt = run_bolt(BASE_BOLT, unknownargs + args)
    if RUN_SEQUENTIALLY:
        main_out, main_err = wait(main_bolt)
        cmp_bolt = run_bolt(CMP_BOLT, unknownargs + cmp_args)
    else:
        cmp_bolt = run_bolt(CMP_BOLT, unknownargs + cmp_args)
        main_out, main_err = wait(main_bolt)
    cmp_out, cmp_err  = wait(cmp_bolt)

    # compare logs
    out = compare_logs(main_out, cmp_out)
    err = compare_logs(main_err, cmp_err, skip_end=1) # skips the line with time
    if (main_bolt.returncode != cmp_bolt.returncode or
        (COMPARE_OUTPUT and (out or err))):
        print(tmp)
        write_to(main_out, os.path.join(tmp, 'main_bolt.stdout'))
        write_to(cmp_out, os.path.join(tmp, 'cmp_bolt.stdout'))
        write_to(main_err, os.path.join(tmp, 'main_bolt.stderr'))
        write_to(cmp_err, os.path.join(tmp, 'cmp_bolt.stderr'))
        write_to(fmt_cmp(out)+fmt_cmp(err), os.path.join(tmp, 'summary.txt'))
        exit("exitcode or logs mismatch")

    # compare binaries (using cmp)
    main_binary = args[args.index('-o')+1]
    cmp_binary = cmp_args[cmp_args.index('-o')+1]

    # report binary timing as csv: output binary; base bolt real; cmp bolt real
    report_real_time(main_binary, main_err, cmp_err)

    if not SKIP_BINARY_CMP:
        cmp_proc = subprocess.run(['cmp', main_binary, cmp_binary],
                                  capture_output=True, text=True)
        if cmp_proc.returncode:
            # check if output is an ELF file (magic bytes)
            with open(main_binary, 'rb') as f:
                magic = f.read(4)
                if magic != b'\x7fELF':
                    exit("output mismatch")
            # check if ELF headers match
            hdr = compare_headers(main_binary, cmp_binary)
            if hdr:
                print(fmt_cmp(hdr))
                write_to(fmt_cmp(hdr), os.path.join(tmp, 'headers.txt'))
                exit("headers mismatch")
            # check which section has the first mismatch
            mismatch_offset = parse_cmp_offset(cmp_proc.stdout)
            print(mismatch_offset)
            # since headers match, check which section this offset falls into
            exit("binaries mismatch")

    # temp files are only cleaned on success
    if not KEEP_TMP:
        shutil.rmtree(tmp)

    # report stdout and stderr from the main process
    print(main_out, file=sys.stdout)
    print(main_err, file=sys.stderr)
    sys.exit(main_bolt.returncode)

if __name__ == "__main__":
    main()
