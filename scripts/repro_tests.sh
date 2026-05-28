#!/bin/bash
# Reproduce specific SUnit tests on OUR VM via eval mode, capturing the actual
# exception, in ISOLATION (one fresh image launch, no SUnitRunner harness).
# Compares JIT-on vs JIT-off to classify: JIT bug (fails only w/ JIT) vs
# VM/interp bug (fails both) vs transient/suite-interaction (passes in isolation).
#
# usage: repro_tests.sh <image> 'Class>>sel' ['Class>>sel' ...]
# Use the CLEAN Pharo.image to avoid FakeGUI/Morphic startup corruption.
set -u
VM="${VM:-/Users/wohl/src/iospharo/build/test_load_image}"
IMG="$1"; shift

pairs=""
for spec in "$@"; do
    pairs="$pairs #(#${spec%%>>*} #${spec##*>>})"
done
mk_expr() { # $1 = output file path
  echo "| s | s := WriteStream on: String new. #( $pairs ) do: [:p | | c r | c := Smalltalk globals at: p first ifAbsent: [nil]. r := c isNil ifTrue: ['NOCLS'] ifFalse: [[(c selector: p second) runCase. 'PASS'] on: Exception do: [:e | 'FAIL ', e class name, ': ', ((e messageText ifNil: ['<nil>']) copyReplaceAll: (String with: Character lf) with: ' ')]]. s nextPutAll: p first; nextPutAll: '>>'; nextPutAll: p second; nextPutAll: ' => '; nextPutAll: r; nextPut: Character lf]. '$1' asFileReference writeStreamDo: [:f | f nextPutAll: s contents]. s contents"
}

rm -f /tmp/repro_jit.txt /tmp/repro_nojit.txt
timeout 220 "$VM" "$IMG" eval "$(mk_expr /tmp/repro_jit.txt)" >/dev/null 2>&1
PHARO_NO_JIT=1 timeout 220 "$VM" "$IMG" eval "$(mk_expr /tmp/repro_nojit.txt)" >/dev/null 2>&1

echo "=== JIT ON ==="; cat /tmp/repro_jit.txt 2>/dev/null || echo "(no output — crash?)"
echo "=== JIT OFF ==="; cat /tmp/repro_nojit.txt 2>/dev/null || echo "(no output — crash?)"
