#!/usr/bin/env bash
# M3 gate: story files compiled through the full LLVM pipeline ($LLVM=2,
# the default) must behave identically to classic ones ($LLVM=0). The
# bytes are intentionally different, so compare glulxe transcripts.
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
    echo "skip  all (glulxe not found; enter the devshell or nix build .#glulxe)"
    exit 0
fi
fail=0

check() {
    local name=$1 input=$2; shift 2
    local flags=("$@")
    if ! $I6 "${flags[@]}" '$LLVM=0' "$name.inf" "$name.classic.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (classic compile failed)"; fail=1; return
    fi
    if ! $I6 "${flags[@]}" '$LLVM=2' "$name.inf" "$name.llvm.ulx" >/dev/null 2>&1; then
        echo "FAIL  $name (llvm compile failed)"; fail=1; return
    fi
    timeout 30 $GLULXE "$name.classic.ulx" < "$input" > "$name.classic.log" 2>&1
    timeout 30 $GLULXE "$name.llvm.ulx" < "$input" > "$name.llvm.log" 2>&1
    if cmp -s "$name.classic.log" "$name.llvm.log"; then
        echo "ok    $name"
    else
        echo "FAIL  $name (transcripts differ; see $name.classic.log / $name.llvm.log)"
        fail=1
    fi
}

check m3      /dev/null -G
check zregion /dev/null -G
if [ -d lib ]; then
    check cloak cloak.walk -G +include_path=lib +language_name=english
else
    echo "skip  cloak (tests/lib not present)"
fi

exit $fail
