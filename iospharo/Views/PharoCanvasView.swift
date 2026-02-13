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
        // Test injection disabled — use manual interaction for testing
        // waitForThemeReady {
        //     NSLog("[TEST] Theme ready, injecting test events")
        //     self.injectMenuTest()
        // }
        #endif
    }

    #if targetEnvironment(macCatalyst)
    private var waitStartTime: Date?

    private func waitForThemeReady(completion: @escaping () -> Void) {
        if waitStartTime == nil { waitStartTime = Date() }
        let elapsed = Date().timeIntervalSince(waitStartTime!)

        if ffi_isFirstExposedDelivered() {
            NSLog("[TEST] EXPOSED delivered after %.1fs! Waiting 2s for theme...", elapsed)
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
                completion()
            }
        } else if elapsed > 10.0 {
            // Timeout: inject anyway — EXPOSED might not fire if P40 died
            NSLog("[TEST] TIMEOUT: EXPOSED not delivered after %.1fs. Injecting anyway.", elapsed)
            completion()
        } else {
            let state = ffi_isSDLEventPollingActive() ? "polling active" : "waiting for polling"
            NSLog("[TEST] %.1fs: %@", elapsed, state as NSString)
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

        NSLog("[TEST] === Starting event injection test ===")
        NSLog("[TEST] Test plan: right-click at (900,620) for world menu, then STOP (no dismiss)")

        // Right-click on empty desktop for world menu
        // Stay within initial 976x665 bounds.
        let desktop = CGPoint(x: 900, y: 620)

        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            NSLog("[TEST] Right-click on desktop at (%d,%d) for world menu", Int(desktop.x), Int(desktop.y))
            bridge.sendMouseMoved(to: desktop, modifiers: 0)
            bridge.sendTouchDown(at: desktop, buttons: IOS_YELLOW_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                bridge.sendTouchUp(at: desktop, buttons: IOS_YELLOW_BUTTON)
            }
        }

        // No further events — leave world menu open for visual inspection
        DispatchQueue.main.asyncAfter(deadline: .now() + 10.0) {
            NSLog("[TEST] === Test complete. World menu should be visible. ===")
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
