/*
 * PharoCanvasView.swift
 *
 * SwiftUI view that wraps MTKView for Metal rendering
 * and handles touch/gesture/keyboard input for Pharo.
 *
 * Event handling (both platforms):
 *   - Clicks/touch/drag: touchesBegan/Moved/Ended on PharoMTKView
 *   - Keyboard: pressesBegan/pressesEnded on PharoMTKView
 *
 * Event handling (Mac Catalyst only):
 *   - Hover (position): UIHoverGestureRecognizer (no button pressed)
 *   - Scroll: UIPanGestureRecognizer (2-finger trackpad)
 *   - Mouse clicks become single-finger touches via UIKit translation
 *   - Right-click detected via UIEvent.buttonMask
 *
 * Event handling (iOS only):
 *   - Additional gesture recognizers (long press, pinch, two-finger tap)
 */

import SwiftUI
import MetalKit

/// Global weak reference to the PharoMTKView for text input control from C callbacks
weak var gPharoMTKView: PharoMTKView?

// MARK: - Custom MTKView with Direct Touch Handling

/// Custom MTKView subclass that handles touch and mouse events directly
class PharoMTKView: MTKView {
    weak var bridge: PharoBridge?

    override init(frame frameRect: CGRect, device: MTLDevice?) {
        super.init(frame: frameRect, device: device)
        setupView()
    }

    required init(coder: NSCoder) {
        super.init(coder: coder)
        setupView()
    }

    private func setupView() {
        isUserInteractionEnabled = true
        isMultipleTouchEnabled = true
    }


    override var canBecomeFirstResponder: Bool {
        return true
    }


    override func didMoveToWindow() {
        super.didMoveToWindow()
        #if targetEnvironment(macCatalyst)
        // On Mac Catalyst, becomeFirstResponder captures hardware keyboard
        // events without showing an on-screen keyboard.
        if window != nil {
            DispatchQueue.main.async {
                self.becomeFirstResponder()
            }
        }
        #endif
        // On iOS, don't auto-become first responder — it shows the soft keyboard.
        // The user controls keyboard visibility via the floating toolbar button.
    }

    #if !targetEnvironment(macCatalyst)
    @discardableResult
    override func resignFirstResponder() -> Bool {
        let result = super.resignFirstResponder()
        if result {
            // Give hardware keyboard focus back to the view controller
            var r: UIResponder? = self.next
            while r != nil {
                if let vc = r as? PharoCanvasViewController {
                    vc.becomeFirstResponder()
                    break
                }
                r = r?.next
            }
        }
        return result
    }
    #endif

    // MARK: - Touch Handling

    private var currentButton: Int = IOS_RED_BUTTON
    var suppressNextTouchCancel = false
    var suppressNextTouchEnd = false

    /// Dead zone for tap vs drag disambiguation.
    /// Finger movement smaller than this threshold (in points) during a touch
    /// is suppressed so Pharo sees a clean click, not a drag.  Without this,
    /// the inevitable micro-movement of a fingertip triggers Morphic scroll panes.
    private let dragThreshold: CGFloat = 8
    private var touchOrigin: CGPoint = .zero
    private var isDragging = false

    // MARK: - Keyboard State

    /// Set in pressesBegan when a modified key (Shift+Enter etc.) is handled
    /// there, to suppress the duplicate insertText call from UIKeyInput.
    var handledInPressesBegan = false

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        var buttons = buttonMaskToPharo(event)

        // Virtual Ctrl key: Ctrl+click = right-click in Pharo
        if bridge.ctrlModifierActive {
            buttons = IOS_YELLOW_BUTTON
            // Auto-clear after click — one-shot modifier like a phone keyboard's Shift
            DispatchQueue.main.async {
                bridge.ctrlModifierActive = false
            }
        }

        // Clear stale suppress flags from previous gesture interactions.
        // These flags protect against delayed touchesCancelled/touchesEnded from a
        // completed long-press gesture. By the time a NEW touch begins, any delayed
        // callbacks from the old touch have either been consumed or are irrelevant.
        // Previously these were cleared in handleLongPress .ended, but on iPad UIKit
        // can deliver the delayed touch callback AFTER .ended, bypassing the suppression
        // and sending a spurious button-up that closes popup menus.
        suppressNextTouchEnd = false
        suppressNextTouchCancel = false

        #if DEBUG
        fputs("[TOUCH] began (\(Int(point.x)),\(Int(point.y))) btn=\(buttons)\n", stderr)
        #endif
        currentButton = buttons
        touchOrigin = point
        isDragging = false
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: buttons)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)

        if !isDragging {
            let dx = point.x - touchOrigin.x
            let dy = point.y - touchOrigin.y
            if dx * dx + dy * dy < dragThreshold * dragThreshold {
                return  // Still within dead zone — suppress move to prevent scroll
            }
            isDragging = true
        }
        bridge.sendTouchMoved(to: point, buttons: currentButton)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        #if DEBUG
        let pt = touches.first.map { $0.location(in: self) } ?? .zero
        fputs("[TOUCH] ended (\(Int(pt.x)),\(Int(pt.y))) btn=\(currentButton) suppress=\(suppressNextTouchEnd)\n", stderr)
        #endif
        // When UIContextMenuInteraction handles a right-click, UIKit still fires
        // touchesEnded for the original touch. Skip the spurious RED button-up
        // since contextMenuInteraction already sent the correct YELLOW events.
        if suppressNextTouchEnd {
            suppressNextTouchEnd = false
            return
        }
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchUp(at: point, buttons: currentButton)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        #if DEBUG
        let pt = touches.first.map { $0.location(in: self) } ?? .zero
        fputs("[TOUCH] cancelled (\(Int(pt.x)),\(Int(pt.y))) btn=\(currentButton) suppress=\(suppressNextTouchCancel)\n", stderr)
        #endif
        // When UIContextMenuInteraction handles a right-click, UIKit cancels
        // the touch. Skip the spurious button-up since contextMenuInteraction
        // already sent the correct right-click events to Pharo.
        if suppressNextTouchCancel {
            suppressNextTouchCancel = false
            return
        }
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchUp(at: point, buttons: currentButton)
    }

    // MARK: - Keyboard Handling

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard bridge != nil else {
            super.pressesBegan(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }

            #if !targetEnvironment(macCatalyst)
            // On iOS, UIKeyInput handles regular characters, enter, and backspace.
            // Only process keys here that UIKeyInput can't: special keys (arrows,
            // function keys) and modifier combos (Cmd+D, Ctrl+C, etc.).
            // Without this guard, every key fires BOTH here AND in UIKeyInput,
            // causing enter/backspace to be doubled.
            if !shouldHandleKeyInPresses(key) { continue }
            // Suppress the duplicate insertText that UIKeyInput will also fire
            // for this key (e.g. Shift+Enter → insertText("\n")).
            handledInPressesBegan = true
            #endif

            postKeyDown(key)
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard bridge != nil else {
            super.pressesEnded(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }

            #if !targetEnvironment(macCatalyst)
            if !shouldHandleKeyInPresses(key) { continue }
            #endif

            postKeyUp(key)
        }
        #if !targetEnvironment(macCatalyst)
        handledInPressesBegan = false
        #endif
    }

    /// Whether a key should be processed in pressesBegan/Ended on iOS (vs UIKeyInput)
    private func shouldHandleKeyInPresses(_ key: UIKey) -> Bool {
        let hasCommandOrControl = key.modifierFlags.contains(.command) || key.modifierFlags.contains(.control)
        if hasCommandOrControl { return true }

        // Shift/Option + non-printable keys need modifier info that UIKeyInput
        // strips. Handle them here (e.g. Shift+Enter = Spotter).
        let hasShiftOrOption = key.modifierFlags.contains(.shift) || key.modifierFlags.contains(.alternate)
        if hasShiftOrOption {
            let code = key.keyCode
            if code == .keyboardReturnOrEnter || code == .keyboardTab ||
               code == .keyboardEscape || code == .keyboardDeleteOrBackspace ||
               code == .keyboardDeleteForward {
                return true
            }
        }

        let code = key.keyCode
        return code == .keyboardUpArrow || code == .keyboardDownArrow ||
               code == .keyboardLeftArrow || code == .keyboardRightArrow ||
               code == .keyboardHome || code == .keyboardEnd ||
               code == .keyboardPageUp || code == .keyboardPageDown ||
               code == .keyboardDeleteForward || code == .keyboardEscape
    }

    /// Map UIKeyModifierFlags to Pharo modifier mask
    func modifierFlagsToPharo(_ flags: UIKeyModifierFlags) -> Int {
        var mods = 0
        if flags.contains(.shift) { mods |= IOS_SHIFT_KEY }
        if flags.contains(.control) { mods |= IOS_CTRL_KEY }
        if flags.contains(.alternate) { mods |= IOS_ALT_KEY }
        if flags.contains(.command) { mods |= IOS_CMD_KEY }
        return mods
    }

    /// Map UIKeyboardHIDUsage to Pharo charCode for special keys
    func specialKeyCharCode(_ keyCode: UIKeyboardHIDUsage) -> Int32 {
        switch keyCode {
        case .keyboardReturnOrEnter: return 13
        case .keyboardEscape: return 27
        case .keyboardDeleteOrBackspace: return 8
        case .keyboardTab: return 9
        case .keyboardDeleteForward: return 127
        case .keyboardUpArrow: return 30
        case .keyboardDownArrow: return 31
        case .keyboardLeftArrow: return 28
        case .keyboardRightArrow: return 29
        case .keyboardHome: return 1
        case .keyboardEnd: return 4
        case .keyboardPageUp: return 11
        case .keyboardPageDown: return 12
        default: return 0
        }
    }

    /// Post key down + keystroke events for a UIKey
    func postKeyDown(_ key: UIKey) {
        let modifiers = Int32(modifierFlagsToPharo(key.modifierFlags))
        // Check special keys first — on Mac Catalyst, key.characters for
        // backspace/arrows/etc. may contain Apple Private Use Area chars
        // (U+F700–F8FF) that would be misinterpreted as printable text.
        let special = specialKeyCharCode(key.keyCode)
        if special > 0 {
            vm_postKeyEvent(0, special, 0, modifiers)
            vm_postKeyEvent(2, special, 0, modifiers)
        } else if let char = key.characters.first, let scalar = char.unicodeScalars.first, scalar.value > 0 {
            let charCode = Int32(scalar.value)
            vm_postKeyEvent(0, charCode, 0, modifiers)
            vm_postKeyEvent(2, charCode, 0, modifiers)
        }
    }

    /// Post key up event for a UIKey
    func postKeyUp(_ key: UIKey) {
        let modifiers = Int32(modifierFlagsToPharo(key.modifierFlags))
        let special = specialKeyCharCode(key.keyCode)
        if special > 0 {
            vm_postKeyEvent(1, special, 0, modifiers)
        } else if let char = key.characters.first, let scalar = char.unicodeScalars.first, scalar.value > 0 {
            vm_postKeyEvent(1, Int32(scalar.value), 0, modifiers)
        }
    }

    // MARK: - Button Mapping

    func buttonMaskToPharo(_ event: UIEvent?) -> Int {
        guard let event = event else { return IOS_RED_BUTTON }
        // Check buttonMask for trackpad/mouse right-click (iPad + Mac Catalyst)
        if #available(iOS 13.4, macCatalyst 13.4, *) {
            let mask = event.buttonMask
            if mask.contains(.secondary) { return IOS_YELLOW_BUTTON }
            if mask.rawValue & 0x4 != 0 { return IOS_BLUE_BUTTON }
        }
        // Ctrl+click/tap = right-click on both Mac Catalyst and iPad with hardware keyboard
        if event.modifierFlags.contains(.control) { return IOS_YELLOW_BUTTON }
        return IOS_RED_BUTTON
    }
}

// MARK: - View Controller

class PharoCanvasViewController: UIViewController {
    var mtkView: PharoMTKView!
    var renderer: MetalRenderer?
    weak var bridge: PharoBridge?

    /// Fixed top offset for the MTKView, captured from the initial safe area.
    /// Once set, this never changes — preventing keyboard events from shifting
    /// the Metal view when SwiftUI triggers a layout pass.
    private var topConstraint: NSLayoutConstraint?
    /// Fixed height constraint — prevents keyboard-triggered SwiftUI re-renders
    /// from changing the MTKView height (which would resize the Pharo framebuffer).
    /// Updated only when width changes (real user resize), not for keyboard events.
    private var heightConstraint: NSLayoutConstraint?
    private var layoutFrozen = false
    private var frozenWidth: CGFloat = 0

    override func loadView() {
        view = UIView()
        #if targetEnvironment(macCatalyst)
        view.backgroundColor = .white
        #else
        view.backgroundColor = .black  // Fills behind rounded corners / Dynamic Island
        #endif
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        mtkView = PharoMTKView()
        mtkView.bridge = bridge
        gPharoMTKView = mtkView
        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.isPaused = false
        mtkView.enableSetNeedsDisplay = false
        mtkView.preferredFramesPerSecond = 30

        // Grey background prevents uninitialized GPU memory (random colored pixels)
        // from flashing before Metal's first draw presents the clear color.
        mtkView.backgroundColor = UIColor(white: 0.92, alpha: 1.0)

        // Set up the Metal renderer BEFORE adding to the view hierarchy.
        // This ensures the delegate catches the very first display-link callback
        // instead of exposing uninitialized CAMetalLayer drawable content.
        if let bridge = bridge {
            renderer = MetalRenderer(metalView: mtkView, bridge: bridge)
        }

        view.addSubview(mtkView)
        #if targetEnvironment(macCatalyst)
        // Mac Catalyst: use safe area for top (title bar / traffic lights)
        // but fill to window edges on left/right/bottom (no rounded corners)
        NSLayoutConstraint.activate([
            mtkView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            mtkView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
        ])
        #else
        // iOS: freeze layout after first pass to prevent keyboard events from
        // resizing or shifting the Metal view. SwiftUI's .ignoresSafeArea(.keyboard)
        // SHOULD prevent this, but @Published keyboard state changes trigger
        // re-renders that can re-evaluate the hosting controller's safe area
        // propagation, especially on iPad with floating keyboards.
        //
        // Strategy: fixed top offset + fixed height (no bottom anchor).
        // Width is unconstrained so Stage Manager / orientation changes still work.
        // Height is only updated when width changes (indicates real user resize).
        topConstraint = mtkView.topAnchor.constraint(equalTo: view.topAnchor, constant: 0)
        heightConstraint = mtkView.heightAnchor.constraint(equalToConstant: 0)
        NSLayoutConstraint.activate([
            topConstraint!,
            heightConstraint!,
            mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
        ])
        #endif

        setupGestureRecognizers()

        #if !targetEnvironment(macCatalyst)
        // Use keyboardWillChangeFrame for ALL keyboard state changes (show, hide,
        // dock, undock, float). This is more reliable than keyboardWillShow/Hide
        // which may not fire for floating keyboards on iPad.
        NotificationCenter.default.addObserver(
            forName: UIResponder.keyboardWillChangeFrameNotification, object: nil, queue: .main
        ) { [weak self] notification in
            MainActor.assumeIsolated {
                guard let frame = notification.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect,
                      frame.width > 0 else { return }
                let screen = UIScreen.main.bounds

                // Keyboard is hidden when its top edge is at or below screen bottom
                let isHidden = frame.origin.y >= screen.height - 1

                // Keyboard is docked when it spans full width at the screen bottom
                let isDocked = !isHidden &&
                               frame.width >= screen.width * 0.9 &&
                               frame.maxY >= screen.height - 1

                #if DEBUG
                fputs("[KB] frame=(\(Int(frame.origin.x)),\(Int(frame.origin.y)),\(Int(frame.width)),\(Int(frame.height))) screen=\(Int(screen.width))x\(Int(screen.height)) hidden=\(isHidden) docked=\(isDocked)\n", stderr)
                #endif

                self?.bridge?.keyboardDocked = isDocked
                self?.bridge?.keyboardVisible = !isHidden
            }
        }
        #endif

        #if targetEnvironment(macCatalyst)
        // Cmd+Q fallback: the system menu bar handles Cmd+Q via SwiftUI
        // .commands, but register a UIKeyCommand as backup in case the
        // menu bar doesn't intercept it (e.g. during first-responder edge cases).
        let quitCommand = UIKeyCommand(input: "q", modifierFlags: .command,
                                       action: #selector(handleQuit(_:)))
        quitCommand.title = "Quit Pharo Smalltalk"
        addKeyCommand(quitCommand)
        #endif
    }

    #if targetEnvironment(macCatalyst)
    @objc func handleQuit(_ sender: Any?) {
        PharoBridge.shared.stop()
        exit(0)
    }
    #endif

    #if !targetEnvironment(macCatalyst)
    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let topInset = view.safeAreaInsets.top
        let availableHeight = view.bounds.height - topInset

        if !layoutFrozen {
            // First layout: capture top inset, height, and width, then freeze.
            topConstraint?.constant = topInset
            heightConstraint?.constant = availableHeight
            frozenWidth = view.bounds.width
            layoutFrozen = true
            #if DEBUG
            fputs("[LAYOUT] Froze: top=\(topInset)pt height=\(availableHeight)pt width=\(frozenWidth)pt\n", stderr)
            #endif
        } else if view.bounds.width != frozenWidth {
            // Width changed → real resize (rotation, Stage Manager, split view).
            // Update height to match new available space.
            heightConstraint?.constant = availableHeight
            frozenWidth = view.bounds.width
            #if DEBUG
            fputs("[LAYOUT] Resize: height=\(availableHeight)pt width=\(frozenWidth)pt\n", stderr)
            #endif
        }
        // Width unchanged but height changed → keyboard event, ignore.
    }
    #endif

    override var canBecomeFirstResponder: Bool {
        #if targetEnvironment(macCatalyst)
        return false  // On Mac, MTKView is always first responder
        #else
        return true   // On iOS, VC handles hardware keyboard when soft keyboard is hidden
        #endif
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        #if targetEnvironment(macCatalyst)
        mtkView.becomeFirstResponder()
        #else
        // On iOS, make the VC first responder for hardware keyboard input.
        // The VC doesn't conform to UIKeyInput, so no soft keyboard appears.
        becomeFirstResponder()
        #endif
    }

    // MARK: - Hardware Keyboard (iOS)
    //
    // On iOS, the MTKView's pressesBegan skips regular characters because
    // UIKeyInput.insertText handles them when the soft keyboard is showing.
    // But when the soft keyboard is hidden, the MTKView is not first responder
    // and receives no keyboard events at all. The VC fills this gap: it
    // becomes first responder when the soft keyboard is hidden and handles
    // ALL hardware keyboard events here.

    #if !targetEnvironment(macCatalyst)
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard bridge != nil else {
            super.pressesBegan(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }
            mtkView.postKeyDown(key)
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard bridge != nil else {
            super.pressesEnded(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }
            mtkView.postKeyUp(key)
        }
    }
    #endif

    private func setupGestureRecognizers() {
        let targetView = mtkView as UIView

        #if targetEnvironment(macCatalyst)
        // Mac Catalyst: clicks and drags are handled by touchesBegan/Moved/Ended
        // (mouse events are translated to single-finger touches by UIKit).
        // Only use gesture recognizers for hover (no button) and scroll (2-finger).

        // Hover gesture: tracks mouse position without button press
        let hoverGesture = UIHoverGestureRecognizer(
            target: self,
            action: #selector(handleHover(_:))
        )
        hoverGesture.cancelsTouchesInView = false
        targetView.addGestureRecognizer(hoverGesture)

        // Left-clicks and drags: touchesBegan/Moved/Ended
        // (cancelsTouchesInView=false on hover ensures touches are delivered)

        // Right-click: UIKit intercepts right-clicks for system context menus
        // before gesture recognizers fire. Use UIContextMenuInteraction to
        // suppress the system menu and capture the click for Pharo.
        let contextMenuInteraction = UIContextMenuInteraction(delegate: self)
        targetView.addInteraction(contextMenuInteraction)

        // Scroll gesture: two-finger trackpad scroll
        let scrollGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleScroll(_:))
        )
        scrollGesture.minimumNumberOfTouches = 2
        scrollGesture.maximumNumberOfTouches = 2
        scrollGesture.allowedScrollTypesMask = .continuous
        targetView.addGestureRecognizer(scrollGesture)

        #else
        // iOS: single taps, double taps, and drags are handled by
        // touchesBegan/Moved/Ended on PharoMTKView (same as Mac Catalyst).
        // Only use gesture recognizers for multi-touch and long press.

        let longPressGesture = UILongPressGestureRecognizer(
            target: self,
            action: #selector(handleLongPress(_:))
        )
        longPressGesture.minimumPressDuration = 0.5
        longPressGesture.cancelsTouchesInView = true
        targetView.addGestureRecognizer(longPressGesture)

        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        // Must cancel individual finger touches — otherwise touchesBegan fires
        // RED button-down for each finger, conflicting with the scroll events.
        twoFingerPanGesture.cancelsTouchesInView = true
        targetView.addGestureRecognizer(twoFingerPanGesture)

        let twoFingerTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        // Only fire tap if pan gesture fails (no significant movement)
        twoFingerTapGesture.require(toFail: twoFingerPanGesture)
        targetView.addGestureRecognizer(twoFingerTapGesture)

        let pinchGesture = UIPinchGestureRecognizer(
            target: self,
            action: #selector(handlePinch(_:))
        )
        pinchGesture.cancelsTouchesInView = false
        targetView.addGestureRecognizer(pinchGesture)
        #endif
    }

    // MARK: - Mac Catalyst Handlers

    #if targetEnvironment(macCatalyst)
    @objc func handleHover(_ gesture: UIHoverGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)

        switch gesture.state {
        case .began:
            bridge.sendMouseMoved(to: point, modifiers: 0)
        case .changed:
            bridge.sendMouseMoved(to: point, modifiers: 0)
        default:
            break
        }
    }

    @objc func handleScroll(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        let translation = gesture.translation(in: mtkView)

        switch gesture.state {
        case .began, .changed:
            let deltaX = Int(translation.x)
            let deltaY = Int(-translation.y)  // Invert for natural scrolling
            if deltaX != 0 || deltaY != 0 {
                bridge.sendScrollEvent(at: point, deltaX: deltaX, deltaY: deltaY)
            }
            gesture.setTranslation(.zero, in: mtkView)
        default:
            break
        }
    }

    #endif

    // MARK: - iOS Gesture Handlers

    @objc func handleLongPress(_ gesture: UILongPressGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        #if DEBUG
        fputs("[LONGPRESS] state=\(gesture.state.rawValue) (\(Int(point.x)),\(Int(point.y)))\n", stderr)
        #endif
        switch gesture.state {
        case .began:
            // touchesBegan already sent RED button down. Clean it up before
            // sending YELLOW, otherwise Pharo has conflicting button states.
            bridge.sendTouchUp(at: point, buttons: IOS_RED_BUTTON)
            // Suppress touchesCancelled/touchesEnded from sending another RED up
            mtkView.suppressNextTouchCancel = true
            mtkView.suppressNextTouchEnd = true
            bridge.sendMouseMoved(to: point, modifiers: 0)
            bridge.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)
            bridge.hapticFeedback(style: .medium)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_YELLOW_BUTTON)
        case .ended, .cancelled:
            bridge.sendTouchUp(at: point, buttons: IOS_YELLOW_BUTTON)
            // Don't clear suppress flags here. They will be cleared at the start
            // of the next touchesBegan. On iPad, UIKit can deliver a delayed
            // touchesCancelled/touchesEnded for the original touch AFTER this
            // .ended callback. If we clear the flags here, that delayed callback
            // sends a spurious button-up that closes popup menus.
        default:
            break
        }
    }

    @objc func handleTwoFingerTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)
        bridge.sendTouchUp(at: point, buttons: IOS_YELLOW_BUTTON)
        bridge.hapticFeedback(style: .light)
    }

    /// Track dominant scroll axis to suppress cross-axis noise.
    /// On a small phone screen, a vertical two-finger swipe easily drifts
    /// horizontally, which the Pharo SpMillerColumnPresenter interprets as
    /// a horizontal page-change gesture — resetting scroll position on the
    /// active page.  Once we lock an axis, we zero out the other.
    private var scrollAxisLocked: Bool = false
    private var scrollIsVertical: Bool = true

    @objc func handleTwoFingerPan(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        let translation = gesture.translation(in: mtkView)
        switch gesture.state {
        case .began:
            scrollAxisLocked = false
            fallthrough
        case .changed:
            var deltaX = Int(translation.x)
            var deltaY = Int(-translation.y)

            // Lock to dominant axis after the first significant movement
            if !scrollAxisLocked {
                let absX = abs(deltaX)
                let absY = abs(deltaY)
                if absX > 4 || absY > 4 {
                    scrollIsVertical = absY >= absX
                    scrollAxisLocked = true
                }
            }
            if scrollAxisLocked {
                if scrollIsVertical { deltaX = 0 } else { deltaY = 0 }
            }

            if deltaX != 0 || deltaY != 0 {
                bridge.sendScrollEvent(at: point, deltaX: deltaX, deltaY: deltaY)
            }
            gesture.setTranslation(.zero, in: mtkView)
        default:
            break
        }
    }

    @objc func handlePinch(_ gesture: UIPinchGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        switch gesture.state {
        case .began, .changed:
            let delta = Int((gesture.scale - 1.0) * 120)
            if delta != 0 {
                bridge.sendScrollEvent(at: point, deltaX: 0, deltaY: delta, modifiers: IOS_CMD_KEY)
            }
            gesture.scale = 1.0
        default:
            break
        }
    }

}

// MARK: - UIContextMenuInteractionDelegate (Mac Catalyst right-click)

#if targetEnvironment(macCatalyst)
extension PharoCanvasViewController: UIContextMenuInteractionDelegate {
    func contextMenuInteraction(
        _ interaction: UIContextMenuInteraction,
        configurationForMenuAtLocation location: CGPoint
    ) -> UIContextMenuConfiguration? {
        // Capture right-click position and send to Pharo as yellow button click.
        // In Pharo, yellow-button menus open on mouseDown and close on mouseUp.
        // Since Mac Catalyst right-click is a single event (not hold), we send
        // both down and up with enough delay for the menu to build and render.
        // The menu then stays open in "click" mode for the user to interact with.
        mtkView.suppressNextTouchCancel = true
        mtkView.suppressNextTouchEnd = true
        if let bridge = bridge {
            bridge.sendMouseMoved(to: location, modifiers: 0)
            bridge.sendTouchDown(at: location, buttons: IOS_YELLOW_BUTTON)
            // Delay the button-up enough for Pharo to build and render the menu.
            // 500ms gives the menu builder time to complete. In Pharo, menus that
            // receive both down+up at the same position "stick" open.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                bridge.sendTouchUp(at: location, buttons: IOS_YELLOW_BUTTON)
            }
        }
        return nil  // Suppress system context menu
    }
}
#endif

// MARK: - UIKeyInput (iOS soft keyboard)

#if !targetEnvironment(macCatalyst)
extension PharoMTKView: UIKeyInput {
    var hasText: Bool {
        return true
    }

    // Disable autocorrect/prediction — it buffers characters instead of
    // delivering them to insertText() immediately, making regular typing
    // appear broken. Also disable smart quotes/dashes which mangle code.
    @objc var autocorrectionType: UITextAutocorrectionType { get { .no } set {} }
    @objc var autocapitalizationType: UITextAutocapitalizationType { get { .none } set {} }
    @objc var spellCheckingType: UITextSpellCheckingType { get { .no } set {} }
    @objc var smartQuotesType: UITextSmartQuotesType { get { .no } set {} }
    @objc var smartDashesType: UITextSmartDashesType { get { .no } set {} }
    @objc var smartInsertDeleteType: UITextSmartInsertDeleteType { get { .no } set {} }
    @objc var keyboardType: UIKeyboardType { get { .asciiCapable } set {} }

    func insertText(_ text: String) {
        // If pressesBegan already handled this key (e.g. Shift+Enter for Spotter),
        // suppress the duplicate insertText from UIKeyInput which strips modifiers.
        if handledInPressesBegan {
            handledInPressesBegan = false
            return
        }
        // Include virtual modifier keys when active
        var mods: Int32 = 0
        if bridge?.ctrlModifierActive == true { mods |= Int32(IOS_CTRL_KEY) }
        if bridge?.cmdModifierActive == true { mods |= Int32(IOS_CMD_KEY) }
        // Auto-clear one-shot modifiers after use
        if mods != 0 {
            DispatchQueue.main.async { [weak self] in
                self?.bridge?.ctrlModifierActive = false
                self?.bridge?.cmdModifierActive = false
            }
        }
        for char in text {
            guard let scalar = char.unicodeScalars.first else { continue }
            // Map LF (10) to CR (13) — Pharo uses CR for return key
            let charCode = scalar.value == 10 ? Int32(13) : Int32(scalar.value)
            let isPrintable = charCode >= 32 && charCode != 127
            vm_postKeyEvent(0, charCode, 0, mods)  // down → SDL_KEYDOWN
            if isPrintable {
                vm_postKeyEvent(2, charCode, 0, mods)  // stroke → SDL_TEXTINPUT
            }
            vm_postKeyEvent(1, charCode, 0, mods)  // up → SDL_KEYUP
        }
    }

    func deleteBackward() {
        bridge?.sendRawKey(8, keyCode: 8)
    }
}
#endif

// MARK: - SwiftUI Wrapper (UIViewControllerRepresentable)

struct PharoCanvasView: UIViewControllerRepresentable {

    @ObservedObject var bridge: PharoBridge

    func makeUIViewController(context: Context) -> PharoCanvasViewController {
        let vc = PharoCanvasViewController()
        vc.bridge = bridge
        return vc
    }

    func updateUIViewController(_ uiViewController: PharoCanvasViewController, context: Context) {
    }
}

// MARK: - Preview

#Preview {
    PharoCanvasView(bridge: PharoBridge.shared)
        .ignoresSafeArea()
}
