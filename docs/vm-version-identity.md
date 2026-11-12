# VM version identity

The image reads the VM version in these places. Pharo 14 only, unless noted.

- `ProcessTest>>testStackOverflow`: skips when the first word of
  `Smalltalk vm interpreterSourceVersion` compares less than 'v12'.
- `VirtualMachine>>vmVersion`: calls `primitiveVMVersion` and, if that
  fails, parses `interpreterSourceVersion` after its first character.
  It splits the text at `+`, then at `.`.
- `VirtualManagerTest>>testVersionIsInVersionString`: the text of
  `Smalltalk vm version` must contain the `major.minor.patch` of `vmVersion`.
- `VirtualMachine>>interpreterSourceDate`: splits the string at `Date: `.
  `DiskStore class>>checkVMVersion` needs a date after 2019-05-01
  (Pharo 13 and 14).
- `StSystemReporterPresenter>>reportVM:`, `VirtualMachine>>version`: display
  only (Pharo 13 and 14).
- `ByteArray>>generateUUIDInPlace` error text: display only.

## Values

- `primitiveVMVersion` answers `#(12 0 6)`.
- `interpreterSourceVersion` (primitive and attribute 1009) answers
  `v12.0.6+iospharo - Commit: iospharo - Date: 2026-04-06 12:00:00 +0000`.
- Attribute 1006 (build date), 1007 (interpreter class), 1008 (cogit
  class) answer `iospharo` strings. Before, all three answered the same
  text, so `Smalltalk vm version` printed it twice.

The suffix `+iospharo` keeps the identity of this VM visible. Stock Cog uses
the same position for its build metadata (`v12.0.6-beta+0.bebdbaca8`).
The VM claims v12.0.6 because it implements the v12 stack-overflow protocol
(`docs/stack-overflow-unwind-fix.md`). Change the numbers in `Primitives.cpp`
(`kVMVersion*`, `kVMSourceVersionString`) only when the VM follows a newer
protocol.
