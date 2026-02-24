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

    }

    #if targetEnvironment(macCatalyst)
    private var waitStartTime: Date?
    private var bufferReadyTime: Date?

    private func waitForThemeReady(completion: @escaping () -> Void) {
        if waitStartTime == nil { waitStartTime = Date() }
        let elapsed = Date().timeIntervalSince(waitStartTime!)

        // Poll the display buffer for actual rendered content
        let bufferReady = checkBufferHasContent()

        if bufferReady && bufferReadyTime == nil {
            bufferReadyTime = Date()
            NSLog("[TEST] Buffer has content after %.1fs! Waiting for full desktop render...", elapsed)
        }

        // Once buffer has content, wait 5 more seconds for desktop to fully render
        if let readyTime = bufferReadyTime {
            let sinceReady = Date().timeIntervalSince(readyTime)
            if sinceReady >= 5.0 {
                NSLog("[TEST] Desktop ready after %.1fs (%.1fs since first content). Injecting.", elapsed, sinceReady)
                completion()
                return
            }
        }

        if elapsed > 90.0 {
            // Hard timeout
            NSLog("[TEST] TIMEOUT: No content after %.1fs. Injecting anyway.", elapsed)
            completion()
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                self.waitForThemeReady(completion: completion)
            }
        }
    }

    /// Check if display buffer has substantial rendered content
    private func checkBufferHasContent() -> Bool {
        guard let bridge = bridge else { return false }
        let (pixels, width, height, _) = bridge.getDisplayBufferInfo()
        guard let bits = pixels, width > 0, height > 0 else { return false }

        // Sample ~1000 pixels and count non-zero
        let total = width * height
        let step = max(1, total / 1000)
        var nonZero = 0
        var idx = 0
        while idx < total {
            if bits[idx] != 0 { nonZero += 1 }
            idx += step
        }
        // Consider buffer "ready" when >50% of sampled pixels are non-zero
        return nonZero > 500
    }

    private func injectMenuTest() {
        guard let bridge = bridge else {
            NSLog("[TEST] injectMenuTest: bridge is nil")
            return
        }

        NSLog("[TEST] === Click-click menu test ===")

        // Take "before" screenshot
        saveBufferScreenshot(tag: "01-before")

        var t: Double = 1.0

        // Step 1: Click "Browse" menu label (click-click: mouseDown+mouseUp)
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Step 1: Click Browse at (113,8)")
            let pos = CGPoint(x: 113, y: 8)
            bridge.sendMouseMoved(to: pos, modifiers: 0)
            bridge.sendTouchDown(at: pos, buttons: IOS_RED_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                bridge.sendTouchUp(at: pos, buttons: IOS_RED_BUTTON)
            }
        }
        t += 1.5

        // Step 2: Screenshot — should show Browse dropdown
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "02-browse-dropdown")
        }
        t += 0.5

        // Step 3: Move mouse from menu bar down to first dropdown item
        // Simulate hover moving from (113,8) → (113,28) → (113,40)
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Step 3: Hover to dropdown item (113,30)")
            bridge.sendMouseMoved(to: CGPoint(x: 113, y: 15), modifiers: 0)
        }
        t += 0.1
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendMouseMoved(to: CGPoint(x: 113, y: 22), modifiers: 0)
        }
        t += 0.1
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendMouseMoved(to: CGPoint(x: 113, y: 30), modifiers: 0)
        }
        t += 0.1
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendMouseMoved(to: CGPoint(x: 113, y: 38), modifiers: 0)
        }
        t += 0.5

        // Step 4: Screenshot — should show item highlighted
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "03-hover-item")
        }
        t += 0.5

        // Step 5: Click the first dropdown item "System Browser" (~113, 38)
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Step 5: Click dropdown item at (113,38)")
            let pos = CGPoint(x: 113, y: 38)
            bridge.sendMouseMoved(to: pos, modifiers: 0)
            bridge.sendTouchDown(at: pos, buttons: IOS_RED_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                bridge.sendTouchUp(at: pos, buttons: IOS_RED_BUTTON)
            }
        }
        t += 3.0

        // Step 6: Screenshot — should show System Browser opened
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "04-after-click-item")
        }
        t += 0.5

        // Step 7: Also test drag pattern for comparison
        // Click and hold Browse, drag to second item, release
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Step 7: Drag test — mouseDown Browse, drag to item, mouseUp")
            let start = CGPoint(x: 190, y: 8) // Debug menu
            bridge.sendMouseMoved(to: start, modifiers: 0)
            bridge.sendTouchDown(at: start, buttons: IOS_RED_BUTTON)
        }
        t += 0.3
        // Drag down slowly
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendTouchMoved(to: CGPoint(x: 190, y: 15), buttons: IOS_RED_BUTTON)
        }
        t += 0.1
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendTouchMoved(to: CGPoint(x: 190, y: 25), buttons: IOS_RED_BUTTON)
        }
        t += 0.1
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendTouchMoved(to: CGPoint(x: 190, y: 35), buttons: IOS_RED_BUTTON)
        }
        t += 0.5
        // Screenshot while held
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "05-drag-held")
        }
        t += 0.5
        // Release on item
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Step 7b: Release on drag item at (190,35)")
            bridge.sendTouchUp(at: CGPoint(x: 190, y: 35), buttons: IOS_RED_BUTTON)
        }
        t += 3.0
        // Final screenshot
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "06-after-drag")
            NSLog("[TEST] === Test complete ===")
        }
    }

    private func injectAllMenusTest() {
        guard let bridge = bridge else {
            NSLog("[TEST] injectAllMenusTest: bridge is nil")
            return
        }

        NSLog("[TEST] === All menus test ===")

        // First close the Welcome window — X button in title bar at approx (831, 68)
        var t: Double = 0.5
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Closing Welcome window — click X at (831,68)")
            let closePos = CGPoint(x: 831, y: 68)
            bridge.sendMouseMoved(to: closePos, modifiers: 0)
            bridge.sendTouchDown(at: closePos, buttons: IOS_RED_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                bridge.sendTouchUp(at: closePos, buttons: IOS_RED_BUTTON)
            }
        }
        t += 3.0

        // Menu bar item X positions (Pharo coordinates, y≈8 for menu bar)
        let menus: [(name: String, x: Int)] = [
            ("Pharo",   30),
            ("Browse",  80),
            ("Debug",   130),
            ("Sources", 185),
            ("System",  240),
            ("Library", 295),
            ("Windows", 352),
            ("Help",    407),
        ]

        let menuY = 8
        let awayPos = CGPoint(x: 500, y: 400) // Empty desktop to click away

        // Take "before" screenshot
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "00-before")
        }
        t += 0.5

        for (i, menu) in menus.enumerated() {
            let pos = CGPoint(x: menu.x, y: menuY)
            let tag = String(format: "%02d-%@", i + 1, menu.name)

            // Click the menu label
            DispatchQueue.main.asyncAfter(deadline: .now() + t) {
                NSLog("[TEST] Click %@ menu at (%d,%d)", menu.name, menu.x, menuY)
                bridge.sendMouseMoved(to: pos, modifiers: 0)
                bridge.sendTouchDown(at: pos, buttons: IOS_RED_BUTTON)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                    bridge.sendTouchUp(at: pos, buttons: IOS_RED_BUTTON)
                }
            }
            t += 3.0  // Give Pharo time to process event and render dropdown

            // Screenshot the dropdown
            DispatchQueue.main.asyncAfter(deadline: .now() + t) {
                self.saveBufferScreenshot(tag: tag + "-dropdown")
            }
            t += 0.5

            // Click away to close the dropdown
            DispatchQueue.main.asyncAfter(deadline: .now() + t) {
                bridge.sendMouseMoved(to: awayPos, modifiers: 0)
                bridge.sendTouchDown(at: awayPos, buttons: IOS_RED_BUTTON)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                    bridge.sendTouchUp(at: awayPos, buttons: IOS_RED_BUTTON)
                }
            }
            t += 2.0
        }

        // World menu test: left-click on empty desktop area (below Welcome window)
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] Click empty desktop for World menu at (900,650)")
            let pos = CGPoint(x: 900, y: 650)
            bridge.sendMouseMoved(to: pos, modifiers: 0)
            bridge.sendTouchDown(at: pos, buttons: IOS_RED_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                bridge.sendTouchUp(at: pos, buttons: IOS_RED_BUTTON)
            }
        }
        t += 3.0

        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            self.saveBufferScreenshot(tag: "09-world-menu")
        }
        t += 0.5

        // Click away to close world menu
        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            bridge.sendMouseMoved(to: CGPoint(x: 800, y: 200), modifiers: 0)
            bridge.sendTouchDown(at: CGPoint(x: 800, y: 200), buttons: IOS_RED_BUTTON)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                bridge.sendTouchUp(at: CGPoint(x: 800, y: 200), buttons: IOS_RED_BUTTON)
            }
        }
        t += 1.0

        DispatchQueue.main.asyncAfter(deadline: .now() + t) {
            NSLog("[TEST] === All menus test complete ===")
            self.saveBufferScreenshot(tag: "10-final")
        }
    }

    private func saveBufferScreenshot(tag: String) {
        guard let bridge = bridge else { return }
        let (pixels, width, height, _) = bridge.getDisplayBufferInfo()
        guard let bits = pixels, width > 0, height > 0 else {
            NSLog("[TEST-SCREENSHOT] No buffer for tag=%@", tag)
            return
        }

        let bytesPerRow = width * 4
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        guard let context = CGContext(
            data: UnsafeMutableRawPointer(bits),
            width: width, height: height,
            bitsPerComponent: 8, bytesPerRow: bytesPerRow,
            space: colorSpace, bitmapInfo: bitmapInfo.rawValue
        ), let cgImage = context.makeImage() else {
            NSLog("[TEST-SCREENSHOT] Failed CGContext for tag=%@", tag)
            return
        }

        let image = UIImage(cgImage: cgImage)
        if let data = image.pngData() {
            let path = "/tmp/iospharo-test-\(tag).png"
            try? data.write(to: URL(fileURLWithPath: path))
            NSLog("[TEST-SCREENSHOT] Saved %dx%d to %@", width, height, path as NSString)
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
