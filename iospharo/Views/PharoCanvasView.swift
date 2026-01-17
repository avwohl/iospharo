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
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        let result = super.hitTest(point, with: event)
        NSLog("[HIT] hitTest at \(point) -> \(result == self ? "self" : String(describing: result))")
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

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else {
            NSLog("[TOUCH] touchesBegan but no touch!")
            return
        }
        let point = touch.location(in: self)
        NSLog("[TOUCH] touchesBegan at \(point), type=\(touch.type.rawValue)")

        guard let bridge = bridge else {
            NSLog("[TOUCH] No bridge!")
            return
        }
        bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchMoved(to: point, buttons: IOS_RED_BUTTON)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else { return }
        let point = touch.location(in: self)
        NSLog("[TOUCH] touchesEnded at \(point)")

        guard let bridge = bridge else { return }
        bridge.sendTouchUp(at: point)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchUp(at: point)
    }

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

/// UIViewController that hosts the PharoMTKView for proper event handling
class PharoCanvasViewController: UIViewController {
    var mtkView: PharoMTKView!
    var renderer: MetalRenderer?
    weak var bridge: PharoBridge?

    override func viewDidLoad() {
        super.viewDidLoad()

        // Create and configure MTKView
        mtkView = PharoMTKView()
        mtkView.bridge = bridge
        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.isPaused = false
        mtkView.enableSetNeedsDisplay = false
        mtkView.preferredFramesPerSecond = 60

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
        NSLog("[VC] viewDidAppear, mtkView isFirstResponder: \(mtkView.isFirstResponder)")
        NSLog("[VC] view.frame: \(view.frame)")
        NSLog("[VC] mtkView.frame: \(mtkView.frame)")
        NSLog("[VC] mtkView.isUserInteractionEnabled: \(mtkView.isUserInteractionEnabled)")
        NSLog("[VC] bridge: \(String(describing: bridge))")

        // Heartbeat timer removed - was too verbose for normal use
    }

    private func setupGestureRecognizers() {
        // Use MTKView directly for gesture recognizers
        let targetView = mtkView as UIView

        // Single tap gesture (primary click)
        let singleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleSingleTap(_:))
        )
        singleTapGesture.numberOfTapsRequired = 1
        #if targetEnvironment(macCatalyst)
        // Mac Catalyst: configure for mouse left-click
        singleTapGesture.buttonMaskRequired = .primary
        singleTapGesture.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        NSLog("[VC] Configured single tap for Mac Catalyst with buttonMask=primary")
        #endif
        targetView.addGestureRecognizer(singleTapGesture)

        // Double tap gesture (double click)
        let doubleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleDoubleTap(_:))
        )
        doubleTapGesture.numberOfTapsRequired = 2
        #if targetEnvironment(macCatalyst)
        doubleTapGesture.buttonMaskRequired = .primary
        doubleTapGesture.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        #endif
        targetView.addGestureRecognizer(doubleTapGesture)

        // Pan gesture for mouse drags
        let panGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handlePan(_:))
        )
        panGesture.minimumNumberOfTouches = 1
        panGesture.maximumNumberOfTouches = 1
        #if targetEnvironment(macCatalyst)
        panGesture.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        #endif
        targetView.addGestureRecognizer(panGesture)

        // Two-finger pan gesture (scroll) - iOS only
        #if !targetEnvironment(macCatalyst)
        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        targetView.addGestureRecognizer(twoFingerPanGesture)
        #endif

        // Right-click gesture for Mac Catalyst
        #if targetEnvironment(macCatalyst)
        let rightClickGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleRightClick(_:))
        )
        rightClickGesture.buttonMaskRequired = .secondary
        rightClickGesture.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        targetView.addGestureRecognizer(rightClickGesture)
        NSLog("[VC] Added right-click gesture for Mac Catalyst")
        #else
        // Long press gesture (right click on iOS)
        let longPressGesture = UILongPressGestureRecognizer(
            target: self,
            action: #selector(handleLongPress(_:))
        )
        longPressGesture.minimumPressDuration = 0.5
        targetView.addGestureRecognizer(longPressGesture)
        #endif

        // Two-finger tap (middle click) - iOS only
        #if !targetEnvironment(macCatalyst)
        let twoFingerTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        targetView.addGestureRecognizer(twoFingerTapGesture)

        // Pinch gesture (zoom) - iOS only
        let pinchGesture = UIPinchGestureRecognizer(
            target: self,
            action: #selector(handlePinch(_:))
        )
        targetView.addGestureRecognizer(pinchGesture)
        #endif

        // Mouse hover for tracking (Mac Catalyst)
        #if targetEnvironment(macCatalyst)
        let hoverGesture = UIHoverGestureRecognizer(
            target: self,
            action: #selector(handleHover(_:))
        )
        targetView.addGestureRecognizer(hoverGesture)
        NSLog("[VC] Added hover gesture for Mac Catalyst")
        #endif

        NSLog("[VC] Setup %d gesture recognizers on view", targetView.gestureRecognizers?.count ?? 0)
    }

    // MARK: - Gesture Handlers

    @objc func handleSingleTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else {
            NSLog("[TAP] No bridge!")
            return
        }
        let point = gesture.location(in: mtkView)  // Get location relative to MTKView
        NSLog("[TAP] Single tap at \(point)")
        bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        bridge.sendTouchUp(at: point)
    }

    @objc func handlePan(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)

        switch gesture.state {
        case .began:
            NSLog("[PAN] began at \(point)")
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

    #if targetEnvironment(macCatalyst)
    @objc func handleRightClick(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        NSLog("[RIGHT CLICK] at \(point)")
        bridge.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
        bridge.sendTouchUp(at: point)
    }

    @objc func handleHover(_ gesture: UIHoverGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)

        switch gesture.state {
        case .began, .changed:
            // Mouse move without buttons
            bridge.sendMouseMoved(to: point)
        default:
            break
        }
    }
    #endif

    @objc func handleTwoFingerTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        NSLog("[TWO FINGER TAP] at \(point)")

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
