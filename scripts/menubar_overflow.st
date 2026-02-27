"MenubarOverflowPatcher: Priority+ responsive menu bar for narrow screens.
 When the menu bar is too narrow to show all items, the overflow items
 are collected under a '...' dropdown at the right edge.

 Installed via StartupLoader: place this file in the image directory.
 Only activates when items don't fit; no-op on wide screens/desktop.

 MenubarMorph items are MenubarItemMorph (subclass chain:
   MenubarItemMorph < DockingBarMenuItemMorph < MenuItemMorph < StringMorph).
 Menu bar is built in MenubarMorph>>open, resized via displayExtentChanged."!

Object subclass: #MenubarOverflowPatcher
  instanceVariableNames: ''
  classVariableNames: 'Installed OriginalItems'
  package: 'IOSPatches'!

!MenubarOverflowPatcher class methodsFor: 'patching'!
install
  "Hook into MenubarMorph to apply overflow on open and resize.
   Only installs once (guarded by Installed class var)."
  Installed == true ifTrue: [^ self].

  "Verify MenubarMorph exists"
  (Smalltalk globals at: #MenubarMorph ifAbsent: [nil]) ifNil: [^ self].

  "Hook open: save original items after building, then apply overflow"
  MenubarMorph compile: 'open
  super open.
  MenubarOverflowPatcher saveItemsFrom: self.
  MenubarOverflowPatcher applyOverflowTo: self' classified: 'menu'.

  "Hook displayExtentChanged: after resize, rebuild from saved items"
  MenubarMorph compile: 'displayExtentChanged
  super displayExtentChanged.
  MenubarOverflowPatcher rebuildOverflowFor: self' classified: 'layout'.

  Installed := true! !

!MenubarOverflowPatcher class methodsFor: 'overflow'!
saveItemsFrom: aMenuBar
  "Save the original menu items so we can rebuild on resize."
  | items |
  items := aMenuBar submorphs select: [:m | m isKindOf: MenuItemMorph].
  "Store as array of {contents. icon. subMenu. target. selector. arguments. helpText}"
  OriginalItems := items collect: [:item |
    | rec |
    rec := Dictionary new.
    rec at: #contents put: item contents.
    rec at: #icon put: ([item icon] on: Error do: [:e | nil]).
    rec at: #subMenu put: item subMenu.
    rec at: #target put: ([item target] on: Error do: [:e | nil]).
    rec at: #selector put: ([item selector] on: Error do: [:e | nil]).
    rec at: #arguments put: ([item arguments] on: Error do: [:e | #()]).
    rec at: #keyText put: ([item keyText] on: Error do: [:e | nil]).
    rec]! !

!MenubarOverflowPatcher class methodsFor: 'overflow'!
rebuildOverflowFor: aMenuBar
  "Rebuild the menu bar from saved items and apply overflow for new width."
  OriginalItems ifNil: [^ self].

  "Remove all current items"
  (aMenuBar submorphs select: [:m | m isKindOf: MenuItemMorph])
    do: [:m | m delete].

  "Re-add all original items"
  OriginalItems do: [:rec |
    | item |
    item := aMenuBar newMenuItem
      contents: (rec at: #contents);
      subMenu: (rec at: #subMenu);
      yourself.
    (rec at: #icon) ifNotNil: [:i | item icon: i].
    (rec at: #keyText) ifNotNil: [:k | item keyText: k].
    (rec at: #target) ifNotNil: [:t |
      item target: t.
      (rec at: #selector) ifNotNil: [:s | item selector: s].
      (rec at: #arguments) ifNotNil: [:a | item arguments: a]].
    aMenuBar addMorphBack: item].

  "Now apply overflow"
  self applyOverflowTo: aMenuBar! !

!MenubarOverflowPatcher class methodsFor: 'overflow'!
applyOverflowTo: aMenuBar
  "Apply priority+ overflow: show as many items as fit, put the rest under '...'."
  | items overflowButtonWidth availableWidth cumulativeWidth cutoffIndex overflowItems overflowButton subMenu |

  items := aMenuBar submorphs select: [:m | m isKindOf: MenuItemMorph].
  items ifEmpty: [^ self].

  overflowButtonWidth := 44.
  availableWidth := aMenuBar width.

  "Walk items left-to-right accumulating widths"
  cumulativeWidth := 0.
  cutoffIndex := items size. "default: all fit"

  items withIndexDo: [:item :idx |
    cumulativeWidth := cumulativeWidth + item width.
    (cumulativeWidth + overflowButtonWidth > availableWidth and: [cutoffIndex = items size]) ifTrue: [
      cutoffIndex := idx - 1]].

  "All items fit — no overflow needed"
  cutoffIndex >= items size ifTrue: [^ self].
  cutoffIndex < 1 ifTrue: [cutoffIndex := 1]. "always show at least 1 item"

  "Collect overflow items"
  overflowItems := items copyFrom: cutoffIndex + 1 to: items size.

  "Remove overflow items from menu bar"
  overflowItems do: [:item | item delete].

  "Build the overflow submenu"
  subMenu := MenuMorph new.
  overflowItems do: [:item |
    | mi |
    mi := subMenu add: item contents
      target: (item target ifNil: [item])
      selector: (item selector ifNil: [#yourself])
      argumentList: #().
    item subMenu ifNotNil: [:sm | mi subMenu: sm]].

  "Create the '...' overflow button as a MenubarItemMorph"
  overflowButton := aMenuBar newMenuItem
    contents: '...';
    subMenu: subMenu;
    yourself.

  aMenuBar addMorphBack: overflowButton! !

!MenubarOverflowPatcher class methodsFor: 'startup'!
startUp: resuming
  resuming ifFalse: [^ self].
  self install! !

!MenubarOverflowPatcher class methodsFor: 'startup'!
shutDown: quitting! !

"Register for startup at low priority (run after GUI is set up)"
SessionManager default
  register: (ClassSessionHandler forClassNamed: #MenubarOverflowPatcher)
  inCategory: SessionManager default userCategory
  atPriority: 200!
