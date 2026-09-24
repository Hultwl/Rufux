#!/bin/bash
# (bsdtar stands in for every tool: all go through the same launcher.)
# Tools found through PATH must be started with their full path as argv[0]. 7-Zip looks
# for its 7z.so plugin next to argv[0]; started as a bare "7z" it looked in the current
# directory instead and every archive failed to open ("Codec Load Error: ./7z.so").
# Usage: test_argv0.sh /path/to/rufux      (exit 77 = skipped)
R=$(readlink -f "$1")
command -v gcc >/dev/null || { echo "skip: gcc missing"; exit 77; }
T=$(mktemp -d); trap 'rm -rf $T' EXIT
mkdir -p $T/bin $T/cwd
cat > $T/fake.c <<'C'
#include <stdio.h>
#include <stdlib.h>
int main(int c, char **v) { FILE *f = fopen(getenv("ARGV0_OUT"), "a"); if (f) { fprintf(f, "%s\n", v[0]); fclose(f); } return 1; }
C
gcc -o $T/bin/7z $T/fake.c || { echo "skip: cannot compile"; exit 77; }
cp $T/bin/7z $T/bin/bsdtar   # ISO extraction tries bsdtar first and falls back to 7z
head -c 4096 /dev/zero > $T/x.iso
# run from an unrelated directory so a "./" lookup could never work by accident
( cd $T/cwd && ARGV0_OUT=$T/argv0 PATH=$T/bin:$PATH "$R" extract $T/x.iso $T/out >/dev/null 2>&1 )
grep -qx "$T/bin/bsdtar" $T/argv0 2>/dev/null && echo "ok argv0" || { echo "FAIL tools started as: $(tr '\n' ' ' < $T/argv0 2>/dev/null) (want $T/bin/bsdtar)"; exit 1; }
