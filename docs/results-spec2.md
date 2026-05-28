# Spec2 SUnit results

Run on `/tmp/harness/Pharo-gfx.image` (clean Pharo 13 + SUnitRunner +
FakeGUI shims).  Class filter: 204 Sp-prefixed `TestCase` subclasses
from `/tmp/spec2_test_classes.txt`.

## Run 1 — 2026-05-28 14:53 → 14:59 (wall-cap)

Raw files: `docs/results/spec2_run1.txt`,
`docs/results/spec2_run1_detail.txt`.

       counter         value
       PASS            363
       FAIL            19
       ERROR           318
       TIMEOUT         2
       reached         716 of 3505 tests (20%)
       wall-cap        yes (1800 s)
       last test       SpInformUserDialogTest>>testInformUserDuringExecutesItsBlock (TIMEOUT)

## Where the errors come from

~50% of executed tests ERRORed.  Spot-check shows almost all are the
same family of UI-adapter failures:

* `SpButtonAdapterTest`, `SpCheckboxAdapterTest`, `SpCodeAdapterTest`
  — "ChangingHelpAffectTheWidget", "ChangingLabelAffectTheWidget",
  "EnabledAffectWidget", "VisibilityWithBlock…", etc.  Each adapter
  test wants to open a real Morphic window and inspect the rendered
  widget; the FakeGUI shim doesn't render so the widget query fails.
* `SpApplicationTest>>testCloseDialogWindowRemovesItFromWindowCollection`
  — same root cause; no real window manager.
* `SpAthensAdapterTest` — needs the Athens canvas backend (Cairo
  plugin not loaded; our VM falls back to morphic-Athens which
  doesn't bind to a window without a display).

These are NOT VM bugs.  They're real UI tests that need the missing
parts of the SDL2 / OSWindow path that our headless harness cannot
provide.

## Where the FAILs come from

19 FAILs is a more interesting set — they're tests that ran enough
to produce an assertion mismatch rather than a missing widget.
Examples:

* `SpApplicationWithToolbarTest>>testOpen`
* `SpApplicationWithToolbarTest>>testExample`
* `SpClassMethodBrowserTest>>testExample`

These would be the right next step if Spec2 becomes a focus.

## Next runs

* **Skip adapter test classes** — filter out *AdapterTest* in the
  class filter, re-run, watch the FAIL/ERROR ratio.  Should give a
  much truer picture of what works.
* **Batch the remaining 80%** — re-run with batch 717-3505 to get
  full coverage of S-Z tests (Splitter, Tab, Tree, TextInput, etc.).
