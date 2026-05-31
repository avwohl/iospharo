#!/bin/bash
# Generate the canonical ordered SUnit class-name list using a stock Pharo VM
# (which has a working filesystem, unlike our VM's batch-range parse path).
#
#   scripts/gen_sunit_class_list.sh [image]   -> writes /tmp/sunit_all_class_names.txt
#
# Order mirrors the in-image runner: the curated test_classes.txt order first,
# then every remaining non-abstract TestCase subclass alphabetically.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-/tmp/harness/Pharo-jit.image}"
PHARO="${PHARO:-/tmp/harness/pharo}"
OUT="${OUT:-/tmp/sunit_all_class_names.txt}"
cp "$ROOT/scripts/pharo-headless-test/test_classes.txt" /tmp/sunit_test_classes.txt 2>/dev/null

timeout 120 "$PHARO" --headless "$IMG" eval \
"| seen order curated |
 order := OrderedCollection new. seen := Set new.
 curated := '/tmp/sunit_test_classes.txt' asFileReference exists
   ifTrue: ['/tmp/sunit_test_classes.txt' asFileReference contents lines
              collect: [:l | l trimBoth] thenSelect: [:l | l notEmpty]]
   ifFalse: [#()].
 curated do: [:n | (Smalltalk globals at: n asSymbol ifAbsent: [nil]) ifNotNil: [:c |
    (seen includes: n) ifFalse: [seen add: n. order add: n]]].
 ((TestCase allSubclasses reject: [:c | c isAbstract]) asSortedCollection: [:a :b | a name <= b name])
   do: [:c | (seen includes: c name) ifFalse: [seen add: c name. order add: c name]].
 '$OUT' asFileReference writeStreamDo: [:s | order do: [:n | s nextPutAll: n; lf]].
 Transcript showln: 'WROTE ', order size printString"
rc=$?
echo "[gen] exit=$rc count=$(wc -l < "$OUT" 2>/dev/null | tr -d ' ')"
