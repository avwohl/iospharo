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
        if window != nil {
            DispatchQueue.main.async {
                self.becomeFirstResponder()
            }
        }
    }

    // MARK: - Touch Handling

    private var currentButton: Int = IOS_RED_BUTTON
    var suppressNextTouchCancel = false
    var suppressNextTouchEnd = false

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        let buttons = buttonMaskToPharo(event)
        currentButton = buttons
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: buttons)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchMoved(to: point, buttons: currentButton)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
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
            let modifiers = Int32(modifierFlagsToPharo(key.modifierFlags))

            // Try character from key first
            if let char = key.characters.first, let scalar = char.unicodeScalars.first, scalar.value > 0 {
                let charCode = Int32(scalar.value)
                vm_postKeyEvent(0, charCode, 0, modifiers)  // down
                vm_postKeyEvent(2, charCode, 0, modifiers)  // stroke
            } else {
                // Special key without printable character (arrows, function keys, etc.)
                let charCode = specialKeyCharCode(key.keyCode)
                if charCode > 0 {
                    vm_postKeyEvent(0, charCode, 0, modifiers)  // down
                    vm_postKeyEvent(2, charCode, 0, modifiers)  // stroke
                }
            }
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard bridge != nil else {
            super.pressesEnded(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }
            let modifiers = Int32(modifierFlagsToPharo(key.modifierFlags))

            if let char = key.characters.first, let scalar = char.unicodeScalars.first, scalar.value > 0 {
                vm_postKeyEvent(1, Int32(scalar.value), 0, modifiers)  // up
            } else {
                let charCode = specialKeyCharCode(key.keyCode)
                if charCode > 0 {
                    vm_postKeyEvent(1, charCode, 0, modifiers)  // up
                }
            }
        }
    }

    /// Map UIKeyModifierFlags to Pharo modifier mask
    private func modifierFlagsToPharo(_ flags: UIKeyModifierFlags) -> Int {
        var mods = 0
        if flags.contains(.shift) { mods |= IOS_SHIFT_KEY }
        if flags.contains(.control) { mods |= IOS_CTRL_KEY }
        if flags.contains(.alternate) { mods |= IOS_ALT_KEY }
        if flags.contains(.command) { mods |= IOS_CMD_KEY }
        return mods
    }

    /// Map UIKeyboardHIDUsage to Pharo charCode for special keys
    private func specialKeyCharCode(_ keyCode: UIKeyboardHIDUsage) -> Int32 {
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

    // MARK: - Button Mapping

    func buttonMaskToPharo(_ event: UIEvent?) -> Int {
        guard let event = event else { return IOS_RED_BUTTON }
        #if targetEnvironment(macCatalyst)
        if #available(macCatalyst 13.4, *) {
            let mask = event.buttonMask
            if mask.contains(.secondary) { return IOS_YELLOW_BUTTON }
            if mask.rawValue & 0x4 != 0 { return IOS_BLUE_BUTTON }
        }
        #endif
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

    override func loadView() {
        view = UIView()
        view.backgroundColor = .black
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        mtkView = PharoMTKView()
        mtkView.bridge = bridge
        gPharoMTKView = mtkView
        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.isPaused = false
        mtkView.enableSetNeedsDisplay = false
        // 30fps: presentsWithTransaction blocks main thread each frame
        mtkView.preferredFramesPerSecond = 30

        view.addSubview(mtkView)
        NSLayoutConstraint.activate([
            mtkView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            mtkView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
        ])

        if let bridge = bridge {
            renderer = MetalRenderer(metalView: mtkView, bridge: bridge)
        }

        setupGestureRecognizers()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        mtkView.becomeFirstResponder()

    }

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
        longPressGesture.cancelsTouchesInView = false
        targetView.addGestureRecognizer(longPressGesture)

        let twoFingerTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        targetView.addGestureRecognizer(twoFingerTapGesture)

        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        twoFingerPanGesture.cancelsTouchesInView = false
        targetView.addGestureRecognizer(twoFingerPanGesture)

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

    @objc func handleSingleTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        let button: Int
        if bridge.middleClickActive {
            button = IOS_BLUE_BUTTON
            bridge.middleClickActive = false
        } else {
            button = IOS_RED_BUTTON
        }
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: button)
        bridge.sendTouchUp(at: point, buttons: button)
    }

    @objc func handlePan(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        switch gesture.state {
        case .began:
            bridge.sendMouseMoved(to: point, modifiers: 0)
            bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_RED_BUTTON)
        case .ended, .cancelled:
            bridge.sendTouchUp(at: point)
        default:
            break
        }
    }

    @objc func handleDoubleTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        bridge.sendTouchUp(at: point)
        bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        bridge.sendTouchUp(at: point)
    }

    @objc func handleLongPress(_ gesture: UILongPressGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        switch gesture.state {
        case .began:
            // Suppress the direct touch handler's button-up so it doesn't
            // send a competing RED button-up after our YELLOW button-down.
            mtkView.suppressNextTouchEnd = true
            bridge.sendMouseMoved(to: point, modifiers: 0)
            bridge.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)
            bridge.hapticFeedback(style: .medium)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_YELLOW_BUTTON)
        case .ended, .cancelled:
            bridge.sendTouchUp(at: point, buttons: IOS_YELLOW_BUTTON)
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

    @objc func handleTwoFingerPan(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        let translation = gesture.translation(in: mtkView)
        switch gesture.state {
        case .began, .changed:
            let deltaX = Int(translation.x)
            let deltaY = Int(-translation.y)
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

    func updateDisplayTexture() {
        renderer?.updateDisplayTexture()
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

    func insertText(_ text: String) {
        for char in text {
            guard let scalar = char.unicodeScalars.first else { continue }
            // Map LF (10) to CR (13) — Pharo uses CR for return key
            let charCode = scalar.value == 10 ? Int32(13) : Int32(scalar.value)
            vm_postKeyEvent(0, charCode, 0, 0)  // down
            vm_postKeyEvent(2, charCode, 0, 0)  // stroke
            vm_postKeyEvent(1, charCode, 0, 0)  // up
        }
    }

    func deleteBackward() {
        vm_postKeyEvent(0, 8, 8, 0)  // down (backspace)
        vm_postKeyEvent(2, 8, 8, 0)  // stroke
        vm_postKeyEvent(1, 8, 8, 0)  // up
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
        if bridge.displayNeedsUpdate {
            uiViewController.updateDisplayTexture()
        }
    }
}

// MARK: - Preview

#Preview {
    PharoCanvasView(bridge: PharoBridge.shared)
        .edgesIgnoringSafeArea(.all)
}
