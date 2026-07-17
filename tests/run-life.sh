#!/usr/bin/env bash
# Life benchmark: compile tests/life.inf both ways ($LLVM=0 classic,
# $LLVM=2 full pipeline), check that the simulations behave identically
# (transcripts must match except the self-reported "Elapsed:" line),
# and report each build's run time.
#
# Run from the tests/ directory. Requires ../inform6-llvm to be built and
# glulxe on the PATH (the nix devshell provides it; ../result/bin/glulxe
# from `nix build .#glulxe` works too).

set -u
cd "$(dirname "$0")"

I6=../inform6-llvm
if command -v glulxe >/dev/null 2>&1; then
    GLULXE=glulxe
elif [ -x ../result/bin/glulxe ]; then
    GLULXE=../result/bin/glulxe
else
    echo "skip  life (glulxe not found; enter the devshell or nix build .#glulxe)"
    exit 0
fi

if ! $I6 -G '$LLVM=0' life.inf life.classic.ulx >/dev/null 2>&1; then
    echo "FAIL  life (classic compile failed)"; exit 1
fi
if ! $I6 -G '$LLVM=2' life.inf life.llvm.ulx >/dev/null 2>&1; then
    echo "FAIL  life (llvm compile failed)"; exit 1
fi

if timeout 120 "$GLULXE" life.classic.ulx < /dev/null > life.classic.log 2>&1; then
    :
else
    status=$?
    echo "FAIL  life (classic interpreter exited $status; see life.classic.log)"
    exit 1
fi
if timeout 120 "$GLULXE" life.llvm.ulx < /dev/null > life.llvm.log 2>&1; then
    :
else
    status=$?
    echo "FAIL  life (llvm interpreter exited $status; see life.llvm.log)"
    exit 1
fi

if ! diff <(grep -v '^Elapsed:' life.classic.log) \
          <(grep -v '^Elapsed:' life.llvm.log) >/dev/null; then
    echo "FAIL  life (simulations differ; see life.classic.log / life.llvm.log)"
    exit 1
fi

echo "ok    life"
echo "      classic: $(grep '^Elapsed:' life.classic.log)"
echo "      llvm:    $(grep '^Elapsed:' life.llvm.log)"
