/*
 * PharoCanvasView.swift
 *
 * SwiftUI view that wraps MTKView for Metal rendering
 * and handles touch/gesture/keyboard input for Pharo.
 *
 * Event handling (both platforms):
 *   - Clicks/touch: touchesBegan/Moved/Ended on PharoMTKView
 *   - Keyboard: pressesBegan/pressesEnded on PharoMTKView
 *
 * Event handling (Mac Catalyst only):
 *   - Hover (position): UIHoverGestureRecognizer on PharoMTKView
 *   - Scroll: UIPanGestureRecognizer (2-finger) on PharoMTKView
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

    // MARK: - Hit Testing

    // hitTest and touchesBegan are not called for mouse on Mac Catalyst.
    // Mouse clicks are handled via UITapGestureRecognizer (see setupGestureRecognizers).
    // touchesBegan IS called for real trackpad touches on iOS.

    // MARK: - Touch Handling

    private var currentButton: Int = IOS_RED_BUTTON

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
        guard let touch = touches.first, let bridge = bridge else { return }
        let point = touch.location(in: self)
        bridge.sendTouchUp(at: point, buttons: currentButton)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
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

    private func buttonMaskToPharo(_ event: UIEvent?) -> Int {
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
        view.backgroundColor = .clear
    }

    override func viewDidLoad() {
        super.viewDidLoad()

        mtkView = PharoMTKView()
        mtkView.bridge = bridge
        mtkView.translatesAutoresizingMaskIntoConstraints = false
        mtkView.isPaused = false
        mtkView.enableSetNeedsDisplay = false
        mtkView.preferredFramesPerSecond = 60

        view.addSubview(mtkView)
        NSLayoutConstraint.activate([
            mtkView.topAnchor.constraint(equalTo: view.topAnchor),
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
        // Hover gesture: tracks mouse position
        let hoverGesture = UIHoverGestureRecognizer(
            target: self,
            action: #selector(handleHover(_:))
        )
        targetView.addGestureRecognizer(hoverGesture)

        // Click gesture: single tap for mouse click
        let clickGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleCatalystClick(_:))
        )
        clickGesture.numberOfTapsRequired = 1
        targetView.addGestureRecognizer(clickGesture)

        // Double-click gesture
        let doubleClickGesture = UITapGestureRecognizer(
            target: self,
            action: #selector(handleCatalystDoubleClick(_:))
        )
        doubleClickGesture.numberOfTapsRequired = 2
        targetView.addGestureRecognizer(doubleClickGesture)
        clickGesture.require(toFail: doubleClickGesture)

        // Drag gesture: single-finger pan for mouse drag
        let dragGesture = UIPanGestureRecognizer(
            target: self,
            action: #selector(handleCatalystDrag(_:))
        )
        dragGesture.minimumNumberOfTouches = 1
        dragGesture.maximumNumberOfTouches = 1
        targetView.addGestureRecognizer(dragGesture)

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
            NSLog("[HOVER] handleHover bridge=NIL")
            return
        }
        let point = gesture.location(in: mtkView)

        switch gesture.state {
        case .began:
            NSLog("[HOVER] began at (%d,%d)", Int(point.x), Int(point.y))
            bridge.sendMouseMoved(to: point, modifiers: 0)
        case .changed:
            bridge.sendMouseMoved(to: point, modifiers: 0)
        default:
            break
        }
    }

    @objc func handleCatalystClick(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        let buttons = buttonMaskFromGesture(gesture)
        NSLog("[CLICK] at (%d,%d) buttons=%d", Int(point.x), Int(point.y), buttons)
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: buttons)
        bridge.sendTouchUp(at: point, buttons: buttons)
    }

    @objc func handleCatalystDoubleClick(_ gesture: UITapGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        let buttons = buttonMaskFromGesture(gesture)
        NSLog("[DBLCLICK] at (%d,%d) buttons=%d", Int(point.x), Int(point.y), buttons)
        bridge.sendMouseMoved(to: point, modifiers: 0)
        bridge.sendTouchDown(at: point, buttons: buttons)
        bridge.sendTouchUp(at: point, buttons: buttons)
        bridge.sendTouchDown(at: point, buttons: buttons)
        bridge.sendTouchUp(at: point, buttons: buttons)
    }

    @objc func handleCatalystDrag(_ gesture: UIPanGestureRecognizer) {
        guard let bridge = bridge else { return }
        let point = gesture.location(in: mtkView)
        switch gesture.state {
        case .began:
            NSLog("[DRAG] began at (%d,%d)", Int(point.x), Int(point.y))
            bridge.sendMouseMoved(to: point, modifiers: 0)
            bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
        case .changed:
            bridge.sendTouchMoved(to: point, buttons: IOS_RED_BUTTON)
        case .ended, .cancelled:
            NSLog("[DRAG] ended at (%d,%d)", Int(point.x), Int(point.y))
            bridge.sendTouchUp(at: point, buttons: IOS_RED_BUTTON)
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

    private func buttonMaskFromGesture(_ gesture: UIGestureRecognizer) -> Int {
        // On Mac Catalyst, check the button mask of the underlying event
        if #available(macCatalyst 13.4, *) {
            if let event = gesture.value(forKey: "_touchEvent") as? UIEvent {
                let mask = event.buttonMask
                if mask.contains(.secondary) { return IOS_YELLOW_BUTTON }
                if mask.rawValue & 0x4 != 0 { return IOS_BLUE_BUTTON }
                if event.modifierFlags.contains(.control) { return IOS_YELLOW_BUTTON }
            }
        }
        return IOS_RED_BUTTON
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
