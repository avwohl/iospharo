/*
 * PharoCanvasView.swift
 *
 * SwiftUI view that wraps MTKView for Metal rendering
 * and handles touch/gesture input for Pharo.
 */

import SwiftUI
import MetalKit

/// SwiftUI wrapper for the Metal-based Pharo display
struct PharoCanvasView: UIViewRepresentable {

    @ObservedObject var bridge: PharoBridge

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeUIView(context: Context) -> MTKView {
        let mtkView = MTKView()

        // Create Metal renderer
        context.coordinator.renderer = MetalRenderer(metalView: mtkView, bridge: bridge)

        // Configure view
        mtkView.isPaused = false
        mtkView.enableSetNeedsDisplay = false
        mtkView.preferredFramesPerSecond = 60

        // Enable user interaction
        mtkView.isUserInteractionEnabled = true
        mtkView.isMultipleTouchEnabled = true

        // Add gesture recognizers
        setupGestureRecognizers(mtkView, coordinator: context.coordinator)

        return mtkView
    }

    func updateUIView(_ uiView: MTKView, context: Context) {
        // Update renderer when bridge state changes
        if bridge.displayNeedsUpdate {
            context.coordinator.renderer?.updateDisplayTexture()
        }
    }

    private func setupGestureRecognizers(_ view: MTKView, coordinator: Coordinator) {
        // Tap gesture (single click)
        let tapGesture = UITapGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handleTap(_:))
        )
        view.addGestureRecognizer(tapGesture)

        // Double tap gesture (double click)
        let doubleTapGesture = UITapGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handleDoubleTap(_:))
        )
        doubleTapGesture.numberOfTapsRequired = 2
        view.addGestureRecognizer(doubleTapGesture)

        // Ensure single tap waits for double tap to fail
        tapGesture.require(toFail: doubleTapGesture)

        // Pan gesture (drag/move) - single finger
        let panGesture = UIPanGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handlePan(_:))
        )
        panGesture.minimumNumberOfTouches = 1
        panGesture.maximumNumberOfTouches = 1
        view.addGestureRecognizer(panGesture)

        // Two-finger pan gesture (scroll)
        let twoFingerPanGesture = UIPanGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handleTwoFingerPan(_:))
        )
        twoFingerPanGesture.minimumNumberOfTouches = 2
        twoFingerPanGesture.maximumNumberOfTouches = 2
        view.addGestureRecognizer(twoFingerPanGesture)

        // Long press gesture (right click)
        let longPressGesture = UILongPressGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handleLongPress(_:))
        )
        longPressGesture.minimumPressDuration = 0.5
        view.addGestureRecognizer(longPressGesture)

        // Two-finger tap (middle click)
        let twoFingerTapGesture = UITapGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handleTwoFingerTap(_:))
        )
        twoFingerTapGesture.numberOfTouchesRequired = 2
        view.addGestureRecognizer(twoFingerTapGesture)

        // Pinch gesture (zoom)
        let pinchGesture = UIPinchGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handlePinch(_:))
        )
        view.addGestureRecognizer(pinchGesture)
    }

    // MARK: - Coordinator

    @MainActor
    class Coordinator: NSObject {
        var parent: PharoCanvasView
        var renderer: MetalRenderer?

        init(_ parent: PharoCanvasView) {
            self.parent = parent
        }

        // MARK: - Gesture Handlers

        @objc func handleTap(_ gesture: UITapGestureRecognizer) {
            let point = gesture.location(in: gesture.view)

            // Single tap = red button click
            parent.bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
            parent.bridge.sendTouchUp(at: point)

            // Haptic feedback
            parent.bridge.hapticFeedback(style: .light)
        }

        @objc func handleDoubleTap(_ gesture: UITapGestureRecognizer) {
            let point = gesture.location(in: gesture.view)

            // Double tap = two rapid red button clicks
            parent.bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
            parent.bridge.sendTouchUp(at: point)
            parent.bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
            parent.bridge.sendTouchUp(at: point)
        }

        @objc func handlePan(_ gesture: UIPanGestureRecognizer) {
            let point = gesture.location(in: gesture.view)

            switch gesture.state {
            case .began:
                parent.bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)

            case .changed:
                parent.bridge.sendTouchMoved(to: point, buttons: IOS_RED_BUTTON)

            case .ended, .cancelled:
                parent.bridge.sendTouchUp(at: point)

            default:
                break
            }
        }

        @objc func handleLongPress(_ gesture: UILongPressGestureRecognizer) {
            let point = gesture.location(in: gesture.view)

            switch gesture.state {
            case .began:
                // Long press = blue/right button
                parent.bridge.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
                parent.bridge.hapticFeedback(style: .medium)

            case .changed:
                parent.bridge.sendTouchMoved(to: point, buttons: IOS_BLUE_BUTTON)

            case .ended, .cancelled:
                parent.bridge.sendTouchUp(at: point)

            default:
                break
            }
        }

        @objc func handleTwoFingerTap(_ gesture: UITapGestureRecognizer) {
            let point = gesture.location(in: gesture.view)

            // Two-finger tap = yellow/middle button
            parent.bridge.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)
            parent.bridge.sendTouchUp(at: point)

            parent.bridge.hapticFeedback(style: .light)
        }

        @objc func handleTwoFingerPan(_ gesture: UIPanGestureRecognizer) {
            // Two-finger pan maps to scroll wheel events
            let point = gesture.location(in: gesture.view)
            let translation = gesture.translation(in: gesture.view)

            switch gesture.state {
            case .began, .changed:
                // Convert translation to scroll delta
                // Invert Y for natural scrolling (drag down = scroll up)
                let deltaX = Int(translation.x)
                let deltaY = Int(-translation.y)

                if deltaX != 0 || deltaY != 0 {
                    parent.bridge.sendScrollEvent(at: point, deltaX: deltaX, deltaY: deltaY)
                }

                // Reset translation for incremental updates
                gesture.setTranslation(.zero, in: gesture.view)

            default:
                break
            }
        }

        @objc func handlePinch(_ gesture: UIPinchGestureRecognizer) {
            // Pinch gesture maps to scroll wheel events for zooming
            let point = gesture.location(in: gesture.view)

            switch gesture.state {
            case .began, .changed:
                // Map scale change to vertical scroll delta
                // scale > 1.0 = zoom in (positive delta)
                // scale < 1.0 = zoom out (negative delta)
                let delta = Int((gesture.scale - 1.0) * 120)  // 120 = one "notch" of scroll
                if delta != 0 {
                    // Send scroll event with Cmd modifier for zoom behavior
                    parent.bridge.sendScrollEvent(at: point, deltaX: 0, deltaY: delta, modifiers: IOS_CMD_KEY)
                }
                gesture.scale = 1.0  // Reset for continuous updates

            default:
                break
            }
        }
    }
}

// MARK: - Preview

#Preview {
    PharoCanvasView(bridge: PharoBridge.shared)
        .edgesIgnoringSafeArea(.all)
}
