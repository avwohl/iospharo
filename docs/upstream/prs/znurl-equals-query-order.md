# Zinc: ZnUrl>>equals: ignores the order of the query fields

## Local data (not part of the PR text)

- Upstream repo: pharo-project/pharo, base branch Pharo15
- Local branch: pr15/znurl-equals-query-order
- Work tree: /Users/wohl/pharo-work/wt/pharo-znurl-equals-query-order
- Commits: 2c3e2dbaf9 "Zinc: ZnUrl>>equals: ignores the order of the query fields" (fix and test)
- Related issue: pharo-project/pharo#11065 "OrderedDictionary: #= ignores order" (OPEN, related only)
- Source in this project: scripts/image-fixes/34-znurl-query-order.st, docs/upstream/package-repairs.md
- Review notes: round 1 found that #keysAndValuesDo: of ZnMultiValueDictionary expands repeated fields, so URLs with a repeated field were never equal. The fix uses #keysDo:, and a new test for repeated fields covers the case. The PR text has no claim about three third-party packages (the record is only in docs/upstream/package-repairs.md).
- Review notes (round 2): the commit corrects the comment of testEqualsWithRepeatedQueryField. The commit removes the repeated same-order assertion from testEqualsWithQueryFieldsInAnotherOrder. The commit splits the change paragraph into lists. The Pharo 14 claim covers only the builds measured, because we checked the commit only in the Pharo 15 tree.
- Design note: Zinc is also maintained at svenvc/zinc, so a mirror PR to that repository can be necessary. A maintainer can prefer a change of OrderedDictionary>>= instead.

## Pull request text

Two URLs with the same query fields in another order are not equal in Pharo 15, and in the Pharo 14 builds that we measured. They were equal in Pharo 13. This PR changes ZnUrl>>equals: so that the order of the query fields has no effect on URL equality, as in Pharo 13.

Related: #11065

### The defect

Run in a clean Pharo 15 image with the stock VM:

```smalltalk
'https://example.com/pets?start=3&limit=2' asZnUrl =
	'https://example.com/pets?limit=2&start=3' asZnUrl
```

Observed: false. Expected: true (the answer in Pharo 13). Tests of other packages that compare URLs with a query can fail because of this change.

### The cause

ZnUrl>>equals: compares the queries with "self query = url query". The query is a ZnMultiValueDictionary, a subclass of OrderedDictionary. Commit d0cc9e250a (the fix of issue #11065, May 2025) changed OrderedDictionary>>= so that the position of each key is part of the comparison. The equality of two ZnUrl objects changed with that commit. No Zinc test covers the case, and nothing shows that the authors wanted this change.

### The change

ZnUrl>>equals: changes in this way:

- The method compares the size of the two queries, and then the value of each key.
- The method does not compare the order of the fields.
- The method uses #keysDo:. #keysAndValuesDo: of ZnMultiValueDictionary gives one call for each single value of a repeated field.

Other points:

- The values of one repeated field are one collection under one key, so these values keep their order.
- ZnUrl>>hash does not use the query, so equal URLs still have equal hashes.
- OrderedDictionary>>= is not changed.

### Tests

New tests:

- ZnUrlTest>>testEqualsWithQueryFieldsInAnotherOrder checks equal URLs in both field orders and equal hashes. The test checks different values, a different number of fields, and a URL with no query.
- ZnUrlTest>>testEqualsWithRepeatedQueryField checks a repeated field: equal in the same order, equal when other fields move. The test also checks that URLs are not equal when the values are in another order or a value is missing.

Environment: Pharo 15 build 141 b4d2dfd, stock Cog v12.0.6-beta bebdbaca8, macOS arm64. Each test ran in a new VM start, three times.

Proof tests, on an image with only the new tests (before) and on an image with the full change (after):
- ZnUrlTest>>testEqualsWithQueryFieldsInAnotherOrder: FAIL 3 of 3 before, PASS 3 of 3 after.
- ZnUrlTest>>testEqualsWithRepeatedQueryField: FAIL 3 of 3 before, PASS 3 of 3 after.

Regression classes, on the base image and on the image with the change:
- ZnUrlTest: 52 pass before (the two new tests are not there), 54 pass after, no other result.
- ZnMultiValueDictionaryTest: 4 pass before and after.
- OrderedDictionaryTest: 69 pass before and after.
- ZnFileUrlTest: 12 pass before and after.
- ZnClientTest: 49 pass and 1 not pass before and after (testQueryGoogle, a live network test, fails on both). No change.

No regression. Not tested: Windows and Linux.

### Notes

- This change restores the behavior of Pharo 13. The order of the fields has no effect on URL equality. If the maintainers want another fix, OrderedDictionary>>= is the other place.
- Windows and Linux were not tested.

Generated with [Claude Code](https://claude.com/claude-code)
