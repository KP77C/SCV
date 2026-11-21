#!/bin/sh
s=$1; shift
/home/claude/ghidra_11.4.2_PUBLIC/support/analyzeHeadless /home/claude/gproj iwxbox -process Default.elf -noanalysis -readOnly -scriptPath /home/claude/iwtools/gs -postScript $s "$@" 2>&1 | sed -n '/> /,$p' | grep -v -E '^INFO  (REPORT|ANALYZING|Using|Opening|Process)' | sed -E 's/^INFO  [A-Za-z]+\.java> //; s/ \(GhidraScript\) *$//'
