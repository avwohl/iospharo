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

// Event logging — writes to stderr (already redirected to /tmp/iospharo-stderr.log by C++)
func pharoEventLog(_ msg: String) {
    fputs("[SWIFT-EVENT] \(msg)\n", stderr)
}

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

    // File-based logging that works with redirected stderr
    func eventLog(_ msg: String) { pharoEventLog(msg) }

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

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        eventLog("[TOUCH] touchesBegan count=\(touches.count) bridge=\(bridge != nil)")
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        let buttons = buttonMaskToPharo(event)
        currentButton = buttons
        eventLog("[TOUCH] down at (\(Int(point.x)),\(Int(point.y))) buttons=\(buttons)")
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: buttons)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchMoved(to: point, buttons: currentButton)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        NSLog("[TOUCH] up at (%d,%d) buttons=%d", Int(point.x), Int(point.y), currentButton)
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
        guard let bridge = bridge else {
            super.pressesBegan(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }
            let charCode = key.characters.first.map { Int($0.asciiValue ?? 0) } ?? 0
            if charCode > 0 {
                bridge.sendKeyDown(Character(UnicodeScalar(charCode)!))
            }
        }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard let bridge = bridge else {
            super.pressesEnded(presses, with: event)
            return
        }
        for press in presses {
            guard let key = press.key else { continue }
            let charCode = key.characters.first.map { Int($0.asciiValue ?? 0) } ?? 0
            if charCode > 0 {
                bridge.sendKeyUp(Character(UnicodeScalar(charCode)!))
            }
        }
    }

    // MARK: - Button Mapping

    func buttonMaskToPharo(_ event: UIEvent?) -> Int {
        #if targetEnvironment(macCatalyst)
        guard let event = event else { return IOS_RED_BUTTON }
        if #available(macCatalyst 13.4, *) {
            let mask = event.buttonMask
            if mask.contains(.secondary) { return IOS_YELLOW_BUTTON }
            if mask.rawValue & 0x4 != 0 { return IOS_BLUE_BUTTON }
            if event.modifierFlags.contains(.control) { return IOS_YELLOW_BUTTON }
        }
        return IOS_RED_BUTTON
        #else
        return IOS_RED_BUTTON
        #endif
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

        #if targetEnvironment(macCatalyst)
        // Test injection disabled — test with manual cliclick instead
        // waitForThemeReady {
        //     NSLog("[TEST] Theme ready, injecting test events")
        //     self.injectMenuTest()
        // }
        #endif
    }

    #if targetEnvironment(macCatalyst)
    private func waitForThemeReady(completion: @escaping () -> Void) {
        // Wait until the first EXPOSED event has been delivered to the image.
        // The EXPOSED event triggers window initialization and theme setup.
        // Without this, rendering the world menu crashes with DNU on
        // SpStyleEnvironmentColorProxy (theme not initialized yet).
        if ffi_isFirstExposedDelivered() {
            NSLog("[TEST] First EXPOSED delivered! Waiting 3s for theme initialization...")
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
                completion()
            }
        } else if ffi_isSDLEventPollingActive() {
            // SDL polling is active but EXPOSED not yet delivered — check again soon
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                self.waitForThemeReady(completion: completion)
            }
        } else {
            NSLog("[TEST] Waiting for SDL event polling...")
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                self.waitForThemeReady(completion: completion)
            }
        }
    }

    private func injectMenuTest() {
        guard let bridge = bridge else {
            NSLog("[TEST] injectMenuTest: bridge is nil")
            return
        }

        // Test order: right-click FIRST (before menu bar), because
        // clicking menu bar sets hand focus that captures all subsequent events.
        //
        // CRITICAL: Initial display Form is 976x665 (from image's saved extent).
        // The SIZE_CHANGED event won't fire until poll#2000 (~35s into run).
        // All click positions MUST be within 976x665 to hit the WorldMorph!

        // Step 1: Move mouse to empty desktop (avoid Welcome window ~130,80 to ~800,560)
        // Stay within initial 976x665 bounds.
        let desktop = CGPoint(x: 900, y: 620)
        NSLog("[TEST] Step 1: Mouse move to empty desktop (%d,%d)", Int(desktop.x), Int(desktop.y))
        bridge.sendMouseMoved(to: desktop, modifiers: 0)

        // Step 2: Right-click on empty desktop for world menu
        // In Pharo 13, PasteUpMorph >> mouseDown: checks yellowButtonPressed
        // (isMenuOpenByLeftClick=true), opens popUpContentsMenu: → invokeWorldMenu:
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            NSLog("[TEST] Step 2a: Right-MouseDOWN at (%d,%d) — world menu", Int(desktop.x), Int(desktop.y))
            bridge.sendTouchDown(at: desktop, buttons: IOS_YELLOW_BUTTON)

            // Hold for 5 seconds (more time for menu to render and texture saves to capture it)
            DispatchQueue.main.asyncAfter(deadline: .now() + 5.0) {
                NSLog("[TEST] Step 2b: Right-MouseUP at (%d,%d) — releasing", Int(desktop.x), Int(desktop.y))
                bridge.sendTouchUp(at: desktop, buttons: IOS_YELLOW_BUTTON)
            }
        }

        // Step 3: After 8s, left-click on desktop to dismiss any menu and release focus
        DispatchQueue.main.asyncAfter(deadline: .now() + 8.0) {
            NSLog("[TEST] Step 3: Left-click to dismiss at (%d,%d)", Int(desktop.x), Int(desktop.y))
            bridge.sendTouchDown(at: desktop, buttons: IOS_RED_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                bridge.sendTouchUp(at: desktop, buttons: IOS_RED_BUTTON)
            }
        }

        // Step 4: After 10s, test menu bar click
        DispatchQueue.main.asyncAfter(deadline: .now() + 10.0) {
            let menuPoint = CGPoint(x: 30, y: 5)
            NSLog("[TEST] Step 4a: Move to menu bar at (%d,%d)", Int(menuPoint.x), Int(menuPoint.y))
            bridge.sendMouseMoved(to: menuPoint, modifiers: 0)

            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                NSLog("[TEST] Step 4b: MouseDOWN at (%d,%d) — holding", Int(menuPoint.x), Int(menuPoint.y))
                bridge.sendTouchDown(at: menuPoint, buttons: IOS_RED_BUTTON)

                // Hold for 3 seconds
                DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
                    NSLog("[TEST] Step 4c: MouseUP at (%d,%d) — releasing", Int(menuPoint.x), Int(menuPoint.y))
                    bridge.sendTouchUp(at: menuPoint, buttons: IOS_RED_BUTTON)
                }
            }
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
        // iOS gesture recognizers

        let singleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleSingleTap(_:))
        )
        singleTapGesture.numberOfTapsRequired = 1
        targetView.addGestureRecognizer(singleTapGesture)

        let doubleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleDoubleTap(_:))
        )
        doubleTapGesture.numberOfTapsRequired = 2
        targetView.addGestureRecognizer(doubleTapGesture)

        let panGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handlePan(_:))
        )
        panGesture.minimumNumberOfTouches = 1
        panGesture.maximumNumberOfTouches = 1
        targetView.addGestureRecognizer(panGesture)

        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        targetView.addGestureRecognizer(twoFingerPanGesture)

        let longPressGesture = UILongPressGestureRecognizer(
            target: self,
            action: #selector(handleLongPress(_:))
        )
        longPressGesture.minimumPressDuration = 0.5
        targetView.addGestureRecognizer(longPressGesture)

        let twoFingerTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        targetView.addGestureRecognizer(twoFingerTapGesture)

        let pinchGesture = UIPinchGestureRecognizer(
            target: self,
            action: #selector(handlePinch(_:))
        )
        targetView.addGestureRecognizer(pinchGesture)
        #endif
    }

    // MARK: - Mac Catalyst Handlers

    #if targetEnvironment(macCatalyst)
    @objc func handleHover(_ gesture: UIHoverGestureRecognizer) {
        guard let bridge = bridge else {
            pharoEventLog("[HOVER] handleHover bridge=NIL")
            return
        }
        let point = gesture.location(in: mtkView)

        switch gesture.state {
        case .began:
            pharoEventLog("[HOVER] began at (\(Int(point.x)),\(Int(point.y)))")
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
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        bridge.sendTouchUp(at: point)
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
            bridge.sendMouseMoved(to: point, modifiers: 0)
            bridge.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
            bridge.hapticFeedback(style: .medium)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_BLUE_BUTTON)
        case .ended, .cancelled:
            bridge.sendTouchUp(at: point, buttons: IOS_BLUE_BUTTON)
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
        // Capture right-click position and send to Pharo as yellow button.
        // Suppress the touchesCancelled that UIKit fires after context menu
        // interaction, which would send a spurious LEFT button UP.
        pharoEventLog("[RIGHT-CLICK] at (\(Int(location.x)),\(Int(location.y)))")
        mtkView.suppressNextTouchCancel = true
        if let bridge = bridge {
            bridge.sendMouseMoved(to: location, modifiers: 0)
            bridge.sendTouchDown(at: location, buttons: IOS_YELLOW_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                bridge.sendTouchUp(at: location, buttons: IOS_YELLOW_BUTTON)
            }
        }
        return nil  // Suppress system context menu
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
