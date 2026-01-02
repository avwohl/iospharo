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

        // Pan gesture (drag/move)
        let panGesture = UIPanGestureRecognizer(
            target: coordinator,
            action: #selector(Coordinator.handlePan(_:))
        )
        view.addGestureRecognizer(panGesture)

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

        @objc func handlePinch(_ gesture: UIPinchGestureRecognizer) {
            // Pinch can be used for zooming the Pharo world
            // For now, we'll map it to scroll wheel events
            let point = gesture.location(in: gesture.view)

            switch gesture.state {
            case .began, .changed:
                // Map scale to scroll delta
                let delta = Int((gesture.scale - 1.0) * 10)
                if delta != 0 {
                    // Send as modifier + scroll (Cmd+scroll for zoom in many Pharo tools)
                    parent.bridge.sendTouchDown(at: point, buttons: IOS_RED_BUTTON, modifiers: IOS_CMD_KEY)
                    parent.bridge.sendTouchUp(at: point, modifiers: IOS_CMD_KEY)
                }
                gesture.scale = 1.0

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
