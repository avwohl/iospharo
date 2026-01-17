/*
 * PharoCanvasView.swift
 *
 * SwiftUI view that wraps MTKView for Metal rendering
 * and handles touch/gesture input for Pharo.
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

        // Debug: log to file that view was created
        if let file = fopen("/tmp/mtkview_setup.log", "a") {
            fputs("[MTKVIEW] setupView called, isUserInteractionEnabled=\(isUserInteractionEnabled)\n", file)
            fclose(file)
        }

        // Note: Removed UIPointerInteraction as it might be consuming click events
        // #if targetEnvironment(macCatalyst)
        // let pointerInteraction = UIPointerInteraction(delegate: self)
        // addInteraction(pointerInteraction)
        // #endif
    }

    // Make view able to become first responder to receive touch events
    override var canBecomeFirstResponder: Bool {
        return true
    }

    // Debug: track if view is receiving hit tests
    private var hitTestCount = 0
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        let result = super.hitTest(point, with: event)
        hitTestCount += 1
        // Log to file
        if hitTestCount <= 100 || hitTestCount % 500 == 1 {
            if let file = fopen("/tmp/mtkview_hittest.log", "a") {
                let resultDesc = result == nil ? "nil" : (result === self ? "self" : String(describing: type(of: result!)))
                fputs("[HIT] #\(hitTestCount) hitTest at \(point) -> \(resultDesc)\n", file)
                fclose(file)
            }
        }
        return result
    }

    // Debug: track point events
    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        let result = super.point(inside: point, with: event)
        if result {
            NSLog("[POINT] point(inside: \(point)) -> \(result)")
        }
        return result
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        // Become first responder when added to window
        if window != nil {
            DispatchQueue.main.async {
                self.becomeFirstResponder()
                print("[VIEW] becameFirstResponder: \(self.isFirstResponder)")
            }
        }
    }

    // MARK: - Touch Handling
    // On Mac Catalyst, mouse events arrive as indirect pointer touches (type=3)
    // We handle all mouse input here - gesture recognizers are disabled for Mac Catalyst

    #if targetEnvironment(macCatalyst)
    // Track current button for Mac Catalyst (determined from UIEvent.buttonMask)
    private var currentButton: Int = IOS_RED_BUTTON
    // Track if we received a touchesBegan - right-clicks skip touchesBegan
    private var hadTouchesBegan: Bool = false
    #endif

    // Debug: log to file
    private static var touchLogFile: UnsafeMutablePointer<FILE>? = {
        fopen("/tmp/swift_touch.log", "w")
    }()

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        // Debug: check if bridge is nil - log to file
        let bridgeStatus = bridge != nil ? "set" : "nil"
        if let file = Self.touchLogFile {
            let msg = "[TOUCH-DEBUG] touchesBegan called, bridge=\(bridgeStatus)\n"
            fputs(msg, file)
            fflush(file)
        }
        guard let touch = touches.first, let bridge = bridge else {
            if let file = Self.touchLogFile {
                fputs("[TOUCH-DEBUG] Early return: bridge is nil\n", file)
                fflush(file)
            }
            return
        }
        let point = touch.location(in: self)
        if let file = Self.touchLogFile {
            fputs("[TOUCH-DEBUG] Processing touch at \(point)\n", file)
            fflush(file)
        }

        #if targetEnvironment(macCatalyst)
        // Mark that we received a touchesBegan (right-clicks skip this)
        hadTouchesBegan = true

        // Debug: log all event info for right-click debugging
        if #available(macCatalyst 13.4, *) {
            let mask = event?.buttonMask ?? []
            let mods = event?.modifierFlags ?? []
            NSLog("[TOUCH] touchesBegan: buttonMask=\(mask.rawValue) modifiers=\(mods.rawValue) touchType=\(touch.type.rawValue)")
        }

        // On Mac Catalyst, check UIEvent.buttonMask to determine which button
        let buttons = buttonMaskToPharo(event)
        currentButton = buttons
        #else
        let buttons = IOS_RED_BUTTON
        #endif

        // IMPORTANT: Send a MOVE event first to position the hand cursor
        // This matches what test_platform does - the hand must be at the click
        // position BEFORE the down event, or Morphic won't find the target correctly
        bridge.sendMouseMoved(to: point, modifiers: 0)

        bridge.sendTouchDown(at: point, buttons: buttons)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)

        #if targetEnvironment(macCatalyst)
        let buttons = currentButton
        #else
        let buttons = IOS_RED_BUTTON
        #endif

        bridge.sendTouchMoved(to: point, buttons: buttons)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)

        #if targetEnvironment(macCatalyst)
        // Debug: log event info
        if #available(macCatalyst 13.4, *) {
            let mask = event?.buttonMask ?? []
            NSLog("[TOUCH] touchesEnded: buttonMask=\(mask.rawValue) hadBegan=\(hadTouchesBegan) at \(point)")
        }

        // IMPORTANT: Right-clicks in Mac Catalyst skip touchesBegan entirely!
        // They only trigger touchesEnded. Detect this and handle as right-click.
        if !hadTouchesBegan {
            NSLog("[TOUCH] Detected right-click (touchesEnded without touchesBegan) at \(point)")
            // Send move + down + up for right-click (blue button)
            bridge.sendMouseMoved(to: point, modifiers: 0)
            bridge.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
            bridge.sendTouchUp(at: point)
            return
        }

        // Reset tracking for next click
        hadTouchesBegan = false
        #endif

        bridge.sendTouchUp(at: point)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchUp(at: point)
    }

    #if targetEnvironment(macCatalyst)
    /// Convert UIEvent button mask and modifiers to Pharo button code
    private func buttonMaskToPharo(_ event: UIEvent?) -> Int {
        // Pharo button codes: red(left)=4, yellow(middle)=2, blue(right)=1
        guard let event = event else { return IOS_RED_BUTTON }

        if #available(macCatalyst 13.4, *) {
            let mask = event.buttonMask

            // Check for secondary (right) button
            if mask.contains(.secondary) {
                return IOS_BLUE_BUTTON  // Right click -> blue button (context menu)
            }

            // Check for middle button
            if mask.rawValue & 0x4 != 0 {
                return IOS_YELLOW_BUTTON  // Middle click -> yellow button
            }

            // Check for control-click (Mac convention for right-click)
            let modifiers = event.modifierFlags
            if modifiers.contains(.control) {
                NSLog("[EVENT] Control-click detected, treating as right-click")
                return IOS_BLUE_BUTTON  // Control-click -> right click
            }
        }
        return IOS_RED_BUTTON  // Default to left click
    }
    #endif

    #if targetEnvironment(macCatalyst)
    // MARK: - Mac Catalyst Mouse Button Handling

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        // Handle mouse button presses on Mac Catalyst
        for press in presses {
            if let key = press.key {
                print("[PRESS] key: \(key.keyCode)")
            }
        }
        super.pressesBegan(presses, with: event)
    }
    #endif
}

// Note: UIPointerInteractionDelegate removed - was potentially consuming click events
// #if targetEnvironment(macCatalyst)
// extension PharoMTKView: UIPointerInteractionDelegate {
//     func pointerInteraction(_ interaction: UIPointerInteraction, regionFor request: UIPointerRegionRequest, defaultRegion: UIPointerRegion) -> UIPointerRegion? {
//         return defaultRegion
//     }
//     func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
//         return UIPointerStyle(effect: .automatic(UITargetedPreview(view: self)))
//     }
// }
// #endif

// MARK: - View Controller for proper event handling

/// Custom UIView that logs hitTest calls
class DebugContainerView: UIView {
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        let result = super.hitTest(point, with: event)
        if let file = fopen("/tmp/container_hittest.log", "a") {
            let resultDesc = result == nil ? "nil" : String(describing: type(of: result!))
            fputs("[CONTAINER HIT] point=\(point) -> \(resultDesc)\n", file)
            fclose(file)
        }
        return result
    }
}

/// UIViewController that hosts the PharoMTKView for proper event handling
class PharoCanvasViewController: UIViewController {
    var mtkView: PharoMTKView!
    var renderer: MetalRenderer?
    weak var bridge: PharoBridge?

    override func loadView() {
        // Use our custom container view instead of default UIView
        view = DebugContainerView()
        view.backgroundColor = .clear
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        // Debug: log to file
        if let file = fopen("/tmp/vc_lifecycle.log", "a") {
            fputs("[VC] viewDidLoad called, bridge=\(bridge != nil ? "set" : "nil")\n", file)
            fclose(file)
        }

        // Create and configure MTKView
        mtkView = PharoMTKView()
        mtkView.bridge = bridge
        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.isPaused = false
        mtkView.enableSetNeedsDisplay = false
        mtkView.preferredFramesPerSecond = 60

        if let file = fopen("/tmp/vc_lifecycle.log", "a") {
            fputs("[VC] mtkView created, bridge on mtkView=\(mtkView.bridge != nil ? "set" : "nil")\n", file)
            fclose(file)
        }

        view.addSubview(mtkView)

        // Constrain to fill parent
        NSLayoutConstraint.activate([
            mtkView.topAnchor.constraint(equalTo: view.topAnchor),
            mtkView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
        ])

        // Create Metal renderer
        if let bridge = bridge {
            renderer = MetalRenderer(metalView: mtkView, bridge: bridge)
        }

        // Add iOS multi-touch gesture recognizers
        setupGestureRecognizers()

        NSLog("[VC] PharoCanvasViewController viewDidLoad complete")
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        mtkView.becomeFirstResponder()

        // Debug: log to file
        if let file = fopen("/tmp/vc_lifecycle.log", "a") {
            fputs("[VC] viewDidAppear called\n", file)
            fputs("[VC] mtkView.isFirstResponder=\(mtkView.isFirstResponder)\n", file)
            fputs("[VC] view.frame=\(view.frame)\n", file)
            fputs("[VC] mtkView.frame=\(mtkView.frame)\n", file)
            fputs("[VC] mtkView.isUserInteractionEnabled=\(mtkView.isUserInteractionEnabled)\n", file)
            fputs("[VC] mtkView.bridge=\(mtkView.bridge != nil ? "set" : "nil")\n", file)
            fputs("[VC] mtkView.window=\(mtkView.window != nil ? "yes" : "nil")\n", file)
            fputs("[VC] view.isUserInteractionEnabled=\(view.isUserInteractionEnabled)\n", file)
            fclose(file)
        }

        NSLog("[VC] viewDidAppear, mtkView isFirstResponder: \(mtkView.isFirstResponder)")
        NSLog("[VC] view.frame: \(view.frame)")
        NSLog("[VC] mtkView.frame: \(mtkView.frame)")
        NSLog("[VC] mtkView.isUserInteractionEnabled: \(mtkView.isUserInteractionEnabled)")
        NSLog("[VC] bridge: \(String(describing: bridge))")
        NSLog("[VC] mtkView.bounds: \(mtkView.bounds)")
        NSLog("[VC] mtkView.window: \(String(describing: mtkView.window))")
        NSLog("[VC] view.window: \(String(describing: view.window))")

        // Log gesture recognizers
        if let gestures = mtkView.gestureRecognizers {
            for (i, g) in gestures.enumerated() {
                NSLog("[VC] mtkView gesture[\(i)]: \(type(of: g)) enabled=\(g.isEnabled)")
            }
        }
    }

    private func setupGestureRecognizers() {
        let targetView = mtkView as UIView

        #if targetEnvironment(macCatalyst)
        // Mac Catalyst: Use direct touch handling for left clicks (touchesBegan/Moved/Ended)
        // But right-clicks need a gesture recognizer with buttonMaskRequired

        // Right-click gesture (secondary button) for world menu
        if #available(macCatalyst 13.4, *) {
            let rightClickGesture = UITapGestureRecognizer(
                target: self,
                action: #selector(handleRightClick(_:))
            )
            rightClickGesture.buttonMaskRequired = .secondary
            rightClickGesture.cancelsTouchesInView = false  // Allow touches to pass through
            targetView.addGestureRecognizer(rightClickGesture)
            NSLog("[VC] Mac Catalyst: added right-click gesture recognizer")
        }

        // Scroll gesture for trackpad scrolling
        let scrollGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleScroll(_:))
        )
        scrollGesture.minimumNumberOfTouches = 2
        scrollGesture.maximumNumberOfTouches = 2
        scrollGesture.allowedScrollTypesMask = .continuous
        scrollGesture.cancelsTouchesInView = false  // Allow touches to pass through
        targetView.addGestureRecognizer(scrollGesture)
        NSLog("[VC] Mac Catalyst: using direct touch handling + scroll gesture")

        #else
        // iOS: Use gesture recognizers for touch input

        // Single tap gesture
        let singleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleSingleTap(_:))
        )
        singleTapGesture.numberOfTapsRequired = 1
        targetView.addGestureRecognizer(singleTapGesture)

        // Double tap gesture
        let doubleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleDoubleTap(_:))
        )
        doubleTapGesture.numberOfTapsRequired = 2
        targetView.addGestureRecognizer(doubleTapGesture)

        // Pan gesture for drags
        let panGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handlePan(_:))
        )
        panGesture.minimumNumberOfTouches = 1
        panGesture.maximumNumberOfTouches = 1
        targetView.addGestureRecognizer(panGesture)

        // Two-finger pan gesture (scroll)
        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        targetView.addGestureRecognizer(twoFingerPanGesture)

        // Long press gesture (right click on iOS)
        let longPressGesture = UILongPressGestureRecognizer(
            target: self,
            action: #selector(handleLongPress(_:))
        )
        longPressGesture.minimumPressDuration = 0.5
        targetView.addGestureRecognizer(longPressGesture)

        // Two-finger tap (middle click)
        let twoFingerTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        targetView.addGestureRecognizer(twoFingerTapGesture)

        // Pinch gesture (zoom)
        let pinchGesture = UIPinchGestureRecognizer(
            target: self,
            action: #selector(handlePinch(_:))
        )
        targetView.addGestureRecognizer(pinchGesture)
        #endif  // !targetEnvironment(macCatalyst)

        NSLog("[VC] Setup %d gesture recognizers on view", targetView.gestureRecognizers?.count ?? 0)
    }

    // MARK: - Mac Catalyst Handlers

    #if targetEnvironment(macCatalyst)
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

    /// Handle right-click (secondary button) for world menu
    @objc func handleRightClick(_ gesture: UITapGestureRecognizer) {
        NSLog("[RIGHT-CLICK] gesture state=\(gesture.state.rawValue)")
        guard let bridge = bridge else {
            NSLog("[RIGHT-CLICK] No bridge!")
            return
        }
        let point = gesture.location(in: mtkView)
        NSLog("[RIGHT-CLICK] at \(point) state=\(gesture.state.rawValue)")

        // Send move to position hand, then blue button (right-click) down/up
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
        bridge.sendTouchUp(at: point)
    }
    #endif

    // MARK: - Gesture Handlers

    @objc func handleSingleTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else {
            NSLog("[TAP] No bridge!")
            return
        }
        let point = gesture.location(in: mtkView)  // Get location relative to MTKView
        NSLog("[TAP] Single tap at \(point)")
        // Send move first to position hand cursor, then down/up
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        bridge.sendTouchUp(at: point)
    }

    @objc func handlePan(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)

        switch gesture.state {
        case .began:
            NSLog("[PAN] began at \(point)")
            bridge.sendMouseMoved(to: point, modifiers: 0)  // Position hand first
            bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_RED_BUTTON)
        case .ended, .cancelled:
            NSLog("[PAN] ended at \(point)")
            bridge.sendTouchUp(at: point)
        default:
            break
        }
    }

    @objc func handleDoubleTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        NSLog("[DOUBLE TAP] at \(point)")

        // Position hand first
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
            NSLog("[LONG PRESS] began at \(point)")
            bridge.sendMouseMoved(to: point, modifiers: 0)  // Position hand first
            bridge.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
            bridge.hapticFeedback(style: .medium)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_BLUE_BUTTON)
        case .ended, .cancelled:
            bridge.sendTouchUp(at: point)
        default:
            break
        }
    }

    @objc func handleTwoFingerTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        NSLog("[TWO FINGER TAP] at \(point)")

        // Position hand first
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)
        bridge.sendTouchUp(at: point)
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

// MARK: - SwiftUI Wrapper (UIViewControllerRepresentable)

/// SwiftUI wrapper using UIViewControllerRepresentable for proper event handling
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
