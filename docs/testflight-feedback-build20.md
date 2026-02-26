# TestFlight Feedback — Build 20

Feedback from Tim (Pharo users group), 2026-02-26.

## FIXED: iOS Soft Keyboard Input Bugs

**Two root causes found and fixed:**

1. **Regular characters not working**: iOS autocorrect/prediction system was
   buffering characters instead of delivering them to `insertText()` immediately.
   Fix: disabled autocorrection, autocapitalization, spell checking, smart
   quotes/dashes via UITextInputTraits on PharoMTKView.

2. **Backspace and Enter doubled**: On iOS, both `pressesBegan` AND UIKeyInput
   fire for the same physical key (enter, backspace). Fix: `pressesBegan` on iOS
   now skips keys that UIKeyInput handles (regular chars, enter, backspace
   without Cmd/Ctrl modifiers). Only special keys (arrows, escape, function keys)
   and modifier combos go through `pressesBegan`.

---

## FIXED: Keyboard Show/Hide Toggle

Added a floating keyboard toggle button (keyboard icon) in the bottom-right
toolbar. Tap to show/hide the soft keyboard. State synced with VM text input
callbacks so the button reflects actual keyboard visibility.

---

## FIXED: Wire Up Floating Mouse Button (Right-Click Modifier)

The floating right-click button now works as a one-shot modifier:
- Tap the button (turns blue/active)
- Next tap on the canvas sends a yellow-button (right-click) event to Pharo
- Button auto-deactivates after the click

Other right-click methods still work:
- Long-press (~0.5s) = right-click
- Two-finger tap = right-click
- Ctrl+click with hardware keyboard = right-click

---

## TODO: Onboarding / Help Overlay

**Priority: Medium** — users don't know how to right-click or evaluate code.

Tim didn't know how to trigger context menus or evaluate expressions. Need
some kind of first-launch help or gesture guide showing:
- Long-press = right-click (context menu)
- Two-finger tap = right-click
- Right-click button = one-shot right-click
- Cmd-D / Cmd-P / Cmd-E with hardware keyboard = Do It / Print It / evaluate
- Keyboard button = toggle soft keyboard

---

## Already Known (not new)

- **Black screen on resize**: Stage Manager resize bug — FIXED in build 22.
- **Settings/dark mode slow**: SwiftUI sheet animation. No actual dark mode
  toggle exists — follows system setting passively.
