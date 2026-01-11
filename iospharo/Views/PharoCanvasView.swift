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

        #if targetEnvironment(macCatalyst)
        // Enable pointer interaction for Mac Catalyst cursor customization
        let pointerInteraction = UIPointerInteraction(delegate: self)
        addInteraction(pointerInteraction)
        #endif
    }

    // Make view able to become first responder to receive touch events
    override var canBecomeFirstResponder: Bool {
        return true
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

    // MARK: - Touch Handling (works on iOS and Mac Catalyst)

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        print("[TOUCH] began at \(point)")

        // Determine button based on touch type
        var buttons = IOS_RED_BUTTON
        #if targetEnvironment(macCatalyst)
        if let event = event {
            // Check for right-click (control+click or two-finger tap)
            if event.modifierFlags.contains(.control) {
                buttons = IOS_BLUE_BUTTON
            }
        }
        #endif

        bridge.sendTouchDown(at: point, buttons: buttons)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)

        var buttons = IOS_RED_BUTTON
        #if targetEnvironment(macCatalyst)
        if let event = event, event.modifierFlags.contains(.control) {
            buttons = IOS_BLUE_BUTTON
        }
        #endif

        bridge.sendTouchMoved(to: point, buttons: buttons)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        print("[TOUCH] ended at \(point)")
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

#if targetEnvironment(macCatalyst)
// MARK: - Pointer Interaction Delegate for Mac Catalyst

extension PharoMTKView: UIPointerInteractionDelegate {
    func pointerInteraction(_ interaction: UIPointerInteraction, regionFor request: UIPointerRegionRequest, defaultRegion: UIPointerRegion) -> UIPointerRegion? {
        // Track pointer movement for hover effects
        let point = request.location
        // Could send hover events here if needed
        return defaultRegion
    }

    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
        // Use crosshair cursor for the canvas
        return UIPointerStyle(effect: .automatic(UITargetedPreview(view: self)))
    }
}
#endif

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
    }

    private func setupGestureRecognizers() {
        // NOTE: Add gestures to view (not mtkView) for reliable Mac Catalyst click handling

        // Single tap gesture (primary click) - critical for Mac Catalyst
        let singleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleSingleTap(_:))
        )
        singleTapGesture.numberOfTapsRequired = 1
        view.addGestureRecognizer(singleTapGesture)
        NSLog("[VC] Added single tap gesture to view")

        // Double tap gesture (double click)
        let doubleTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleDoubleTap(_:))
        )
        doubleTapGesture.numberOfTapsRequired = 2
        view.addGestureRecognizer(doubleTapGesture)

        // Single tap requires double tap to fail
        singleTapGesture.require(toFail: doubleTapGesture)

        // Pan gesture for mouse drags
        let panGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handlePan(_:))
        )
        panGesture.minimumNumberOfTouches = 1
        panGesture.maximumNumberOfTouches = 1
        view.addGestureRecognizer(panGesture)

        // Two-finger pan gesture (scroll)
        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        view.addGestureRecognizer(twoFingerPanGesture)

        // Long press gesture (right click on iOS)
        let longPressGesture = UILongPressGestureRecognizer(
            target: self,
            action: #selector(handleLongPress(_:))
        )
        longPressGesture.minimumPressDuration = 0.5
        view.addGestureRecognizer(longPressGesture)

        // Two-finger tap (middle click)
        let twoFingerTapGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        view.addGestureRecognizer(twoFingerTapGesture)

        // Pinch gesture (zoom)
        let pinchGesture = UIPinchGestureRecognizer(
            target: self,
            action: #selector(handlePinch(_:))
        )
        view.addGestureRecognizer(pinchGesture)

        NSLog("[VC] Setup %d gesture recognizers on view", view.gestureRecognizers?.count ?? 0)
    }

    // MARK: - Gesture Handlers

    @objc func handleSingleTap(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)  // Get location relative to MTKView
        NSLog("[SINGLE TAP] at \(point)")

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
