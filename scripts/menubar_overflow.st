"MenubarOverflowPatcher: Priority+ responsive menu bar for narrow screens.
 When the menu bar is too narrow to show all items, the overflow items
 are collected under a '...' dropdown at the right edge.

 Installed via StartupLoader: place this file in the image directory.
 Only activates when items don't fit; no-op on wide screens/desktop."!

Object subclass: #MenubarOverflowPatcher
  instanceVariableNames: ''
  classVariableNames: 'Installed'
  package: 'IOSPatches'!

!MenubarOverflowPatcher class methodsFor: 'patching'!
install
  "Compile a replacement MenubarMorph>>updateMenu that calls the overflow logic.
   Only installs once (guarded by Installed class var)."
  Installed == true ifTrue: [^ self].

  "Verify MenubarMorph exists"
  (Smalltalk globals at: #MenubarMorph ifAbsent: [nil]) ifNil: [^ self].

  "Install wrapper: after the standard updateMenu rebuilds items,
   apply overflow to fit narrow screens."
  MenubarMorph compile: 'updateMenuWithOverflow
  "Replacement updateMenu that applies overflow for narrow screens."
  self updateMenuOriginal.
  MenubarOverflowPatcher applyOverflowTo: self' classified: 'menu'.

  "Save original if not already saved"
  (MenubarMorph canUnderstand: #updateMenuOriginal) ifFalse: [
    | originalSource |
    originalSource := (MenubarMorph >> #updateMenu) sourceCode.
    "Rename the selector in the source"
    originalSource := originalSource copyReplaceAll: 'updateMenu' with: 'updateMenuOriginal'.
    MenubarMorph compile: originalSource classified: 'menu'].

  "Now replace updateMenu to call our wrapper"
  MenubarMorph compile: 'updateMenu
  self updateMenuWithOverflow' classified: 'menu'.

  Installed := true! !

!MenubarOverflowPatcher class methodsFor: 'patching'!
applyOverflowTo: aMenuBar
  "Apply priority+ overflow: show as many items as fit, put the rest under '...'."
  | items overflowButtonWidth availableWidth cumulativeWidth cutoffIndex overflowItems overflowButton subMenu |

  items := aMenuBar submorphs select: [:m | m class name = 'ToggleMenuItemMorph'].
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
    | menuItem |
    menuItem := item hasSubMenu
      ifTrue: [
        "For items with submenus, create a menu item that opens the submenu"
        subMenu add: item contents target: item selector: item actionSelector.
        "Transfer the submenu"
        (subMenu menuItems last) subMenu: item subMenu]
      ifFalse: [
        subMenu add: item contents target: item target selector: item actionSelector]].

  "Create the '...' toggle button"
  overflowButton := ToggleMenuItemMorph new
    contents: '...';
    font: items first font;
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
