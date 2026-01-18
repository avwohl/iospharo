/*
 * iosparoApp.swift
 *
 * Main SwiftUI app entry point for the iOS Pharo client.
 */

import SwiftUI
#if targetEnvironment(macCatalyst)
import GameController  // For GCMouse
#endif

#if targetEnvironment(macCatalyst)
/// Custom UIWindow that intercepts all events for Mac Catalyst
class EventCapturingWindow: UIWindow {
    private static var eventLogFile: UnsafeMutablePointer<FILE>? = {
        fopen("/tmp/window_sendEvent.log", "w")
    }()
    private static var eventCount = 0
    private var lastMousePosition: CGPoint = .zero

    override func sendEvent(_ event: UIEvent) {
        EventCapturingWindow.eventCount += 1

        // Log all events
        if let file = EventCapturingWindow.eventLogFile, EventCapturingWindow.eventCount <= 200 {
            fputs("[SEND-EVENT] #\(EventCapturingWindow.eventCount) type=\(event.type.rawValue) subtype=\(event.subtype.rawValue)\n", file)
            fflush(file)
        }

        // Check for touch events (which include pointer events on Mac Catalyst)
        if event.type == .touches {
            if let touches = event.allTouches {
                for touch in touches {
                    let location = touch.location(in: self)
                    let phase = touch.phase

                    if let file = EventCapturingWindow.eventLogFile, EventCapturingWindow.eventCount <= 200 {
                        fputs("[TOUCH-EVENT] phase=\(phase.rawValue) location=\(location) touchType=\(touch.type.rawValue)\n", file)
                        fflush(file)
                    }

                    // Forward to Pharo VM on main actor
                    Task { @MainActor in
                        switch phase {
                        case .began:
                            PharoBridge.shared.sendMouseMoved(to: location, modifiers: 0)
                            PharoBridge.shared.sendTouchDown(at: location, buttons: Int(IOS_RED_BUTTON))
                        case .moved:
                            PharoBridge.shared.sendTouchMoved(to: location, buttons: Int(IOS_RED_BUTTON))
                        case .ended, .cancelled:
                            PharoBridge.shared.sendTouchUp(at: location)
                        default:
                            break
                        }
                    }
                }
            }
        }

        // Always call super to let normal event handling continue
        super.sendEvent(event)
    }
}
#endif

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        #if targetEnvironment(macCatalyst)
        // Swizzle UIApplication.sendEvent to intercept all events
        ApplicationSwizzler.swizzleSendEvent()
        setupMouseHandling()
        #endif
        return true
    }

    #if targetEnvironment(macCatalyst)
    func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: "Default Configuration", sessionRole: connectingSceneSession.role)
        config.delegateClass = SceneDelegate.self
        return config
    }
    #endif

    func applicationWillTerminate(_ application: UIApplication) {
        NSLog("[APP] applicationWillTerminate - stopping VM")
        Task { @MainActor in
            PharoBridge.shared.stop()
        }
        // Give the VM a moment to stop cleanly
        Thread.sleep(forTimeInterval: 0.1)
    }

    #if targetEnvironment(macCatalyst)
    private func setupMouseHandling() {
        // Use GCMouse for Mac Catalyst mouse handling
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(mouseDidConnect(_:)),
            name: .GCMouseDidConnect,
            object: nil
        )

        // Check if a mouse is already connected
        if let mouse = GCMouse.current {
            configureMouseInput(mouse)
        }
        NSLog("[APP] GCMouse event handling set up")
    }

    @objc private func mouseDidConnect(_ notification: Notification) {
        if let mouse = notification.object as? GCMouse {
            configureMouseInput(mouse)
            NSLog("[APP] Mouse connected: \(mouse)")
        }
    }

    private func configureMouseInput(_ mouse: GCMouse) {
        guard let input = mouse.mouseInput else { return }

        // Left button
        input.leftButton.pressedChangedHandler = { button, value, pressed in
            if let file = fopen("/tmp/gcmouse.log", "a") {
                fputs("[GCMOUSE] leftButton pressed=\(pressed) value=\(value)\n", file)
                fclose(file)
            }
            // Note: We don't have position here - GCMouse doesn't track position
            // Position tracking requires UIHoverGestureRecognizer
        }

        // Right button
        input.rightButton?.pressedChangedHandler = { button, value, pressed in
            if let file = fopen("/tmp/gcmouse.log", "a") {
                fputs("[GCMOUSE] rightButton pressed=\(pressed) value=\(value)\n", file)
                fclose(file)
            }
        }

        // Mouse movement
        input.mouseMovedHandler = { mouse, deltaX, deltaY in
            if let file = fopen("/tmp/gcmouse.log", "a") {
                fputs("[GCMOUSE] moved delta=(\(deltaX), \(deltaY))\n", file)
                fclose(file)
            }
        }

        NSLog("[APP] Mouse input handlers configured")
    }
    #endif
}

#if targetEnvironment(macCatalyst)
/// Scene delegate - not used for window creation since SwiftUI manages windows,
/// but kept for potential future use
class SceneDelegate: NSObject, UIWindowSceneDelegate {
    var window: UIWindow?
}

/// Swizzle UIApplication to intercept ALL events
class ApplicationSwizzler {
    static var swizzled = false

    static func swizzleSendEvent() {
        guard !swizzled else { return }
        swizzled = true

        // Try UIApplication.sendEvent instead of UIWindow
        let originalSelector = #selector(UIApplication.sendEvent(_:))
        let swizzledSelector = #selector(UIApplication.swizzled_sendEvent(_:))

        guard let originalMethod = class_getInstanceMethod(UIApplication.self, originalSelector),
              let swizzledMethod = class_getInstanceMethod(UIApplication.self, swizzledSelector) else {
            NSLog("[SWIZZLE] Failed to get UIApplication methods")
            if let file = fopen("/tmp/swizzle.log", "w") {
                fputs("[SWIZZLE] ERROR: Failed to get UIApplication methods\n", file)
                fclose(file)
            }
            return
        }

        method_exchangeImplementations(originalMethod, swizzledMethod)
        NSLog("[SWIZZLE] Successfully swizzled UIApplication.sendEvent")

        if let file = fopen("/tmp/swizzle.log", "w") {
            fputs("[SWIZZLE] Successfully swizzled UIApplication.sendEvent\n", file)
            fputs("[SWIZZLE] Original method: \(originalMethod)\n", file)
            fputs("[SWIZZLE] Swizzled method: \(swizzledMethod)\n", file)
            fclose(file)
        }
    }
}

extension UIApplication {
    private static var eventLogFile: UnsafeMutablePointer<FILE>? = nil
    private static var eventCount = 0
    private static var initialized = false

    @objc func swizzled_sendEvent(_ event: UIEvent) {
        // Initialize log file IMMEDIATELY on first call
        if !UIApplication.initialized {
            UIApplication.initialized = true
            UIApplication.eventLogFile = fopen("/tmp/app_sendEvent.log", "w")
            if let file = UIApplication.eventLogFile {
                fputs("[APP-EVENT] === Swizzled UIApplication.sendEvent is being called! ===\n", file)
                fflush(file)
            }
        }

        UIApplication.eventCount += 1

        // Log all events (limit to avoid huge logs)
        if let file = UIApplication.eventLogFile, UIApplication.eventCount <= 1000 {
            fputs("[APP-EVENT] #\(UIApplication.eventCount) type=\(event.type.rawValue) subtype=\(event.subtype.rawValue)\n", file)
            fflush(file)
        }

        // Handle touch events (which include pointer events on Mac Catalyst)
        if event.type == .touches {
            if let touches = event.allTouches, let window = self.keyWindow {
                for touch in touches {
                    let location = touch.location(in: window)
                    let phase = touch.phase

                    if let file = UIApplication.eventLogFile, UIApplication.eventCount <= 1000 {
                        fputs("[TOUCH-EVENT] phase=\(phase.rawValue) location=\(location) touchType=\(touch.type.rawValue)\n", file)
                        fflush(file)
                    }

                    // Forward to Pharo VM on main actor
                    Task { @MainActor in
                        switch phase {
                        case .began:
                            PharoBridge.shared.sendMouseMoved(to: location, modifiers: 0)
                            PharoBridge.shared.sendTouchDown(at: location, buttons: Int(IOS_RED_BUTTON))
                        case .moved:
                            PharoBridge.shared.sendTouchMoved(to: location, buttons: Int(IOS_RED_BUTTON))
                        case .ended, .cancelled:
                            PharoBridge.shared.sendTouchUp(at: location)
                        default:
                            break
                        }
                    }
                }
            }
        }

        // Call original implementation (which is now swizzled_sendEvent due to exchange)
        swizzled_sendEvent(event)
    }
}
#endif

@main
struct iosparoApp: App {

    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var bridge = PharoBridge.shared
    @StateObject private var imageManager = ImageManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(bridge)
                .environmentObject(imageManager)
                #if targetEnvironment(macCatalyst)
                .onAppear {
                    setupWindowEventHandling()
                }
                #endif
        }
    }

    #if targetEnvironment(macCatalyst)
    private func setupWindowEventHandling() {
        // Delay to ensure window exists
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            guard let windowScene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
                  let window = windowScene.windows.first else {
                if let file = fopen("/tmp/window_setup.log", "a") {
                    fputs("[WINDOW] No window scene found\n", file)
                    fclose(file)
                }
                return
            }

            if let file = fopen("/tmp/window_setup.log", "a") {
                fputs("[WINDOW] Found window: \(window)\n", file)
                fputs("[WINDOW] Root VC: \(String(describing: window.rootViewController))\n", file)
                fclose(file)
            }

            // Log view hierarchy
            if let file = fopen("/tmp/window_setup.log", "a") {
                fputs("[WINDOW] View hierarchy:\n", file)
                Self.logViewHierarchy(window, file: file, indent: 0)
                fclose(file)
            }

            // Try adding gestures directly to the window
            let hoverGesture = UIHoverGestureRecognizer(
                target: MouseEventHandler.shared,
                action: #selector(MouseEventHandler.handleWindowHover(_:))
            )
            window.addGestureRecognizer(hoverGesture)

            // Use UILongPressGestureRecognizer with 0 duration - works better on Mac Catalyst
            let pressGesture = UILongPressGestureRecognizer(
                target: MouseEventHandler.shared,
                action: #selector(MouseEventHandler.handleWindowPress(_:))
            )
            pressGesture.minimumPressDuration = 0  // Fires immediately on press
            pressGesture.allowableMovement = 10000 // Allow movement during press
            window.addGestureRecognizer(pressGesture)

            // Also try adding directly to the hosting view
            if let hostingView = window.subviews.first?.subviews.first?.subviews.first {
                let hostingPress = UILongPressGestureRecognizer(
                    target: MouseEventHandler.shared,
                    action: #selector(MouseEventHandler.handleWindowPress(_:))
                )
                hostingPress.minimumPressDuration = 0
                hostingPress.allowableMovement = 10000
                hostingView.addGestureRecognizer(hostingPress)

                if let file = fopen("/tmp/window_setup.log", "a") {
                    fputs("[WINDOW] Added press gesture to hosting view: \(hostingView)\n", file)
                    fclose(file)
                }
            }

            if let file = fopen("/tmp/window_setup.log", "a") {
                fputs("[WINDOW] Added gesture recognizers to window (not root view)\n", file)
                fputs("[WINDOW] Window gesture count: \(window.gestureRecognizers?.count ?? 0)\n", file)
                fclose(file)
            }
        }
    }

    private static func logViewHierarchy(_ view: UIView, file: UnsafeMutablePointer<FILE>, indent: Int) {
        let prefix = String(repeating: "  ", count: indent)
        let gestures = view.gestureRecognizers?.map { String(describing: type(of: $0)) } ?? []
        let isUserInteraction = view.isUserInteractionEnabled ? "YES" : "NO"
        fputs("\(prefix)\(type(of: view)) frame=\(view.frame) interact=\(isUserInteraction) gestures=\(gestures)\n", file)
        for subview in view.subviews {
            logViewHierarchy(subview, file: file, indent: indent + 1)
        }
    }
    #endif
}

#if targetEnvironment(macCatalyst)
/// Singleton to handle window-level mouse events
@MainActor
class MouseEventHandler: NSObject {
    static let shared = MouseEventHandler()

    private var lastPosition: CGPoint = .zero
    private var isDragging = false

    @objc func handleWindowHover(_ gesture: UIHoverGestureRecognizer) {
        let point = gesture.location(in: gesture.view)

        switch gesture.state {
        case .began, .changed:
            lastPosition = point
            if let file = fopen("/tmp/window_events.log", "a") {
                fputs("[WINDOW-HOVER] at \(point)\n", file)
                fclose(file)
            }
            // Send mouse move to VM
            PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
        default:
            break
        }
    }

    @objc func handleWindowPress(_ gesture: UILongPressGestureRecognizer) {
        let point = gesture.location(in: gesture.view)

        switch gesture.state {
        case .began:
            isDragging = true
            if let file = fopen("/tmp/window_events.log", "a") {
                fputs("[WINDOW-PRESS] began at \(point)\n", file)
                fclose(file)
            }
            PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
            PharoBridge.shared.sendTouchDown(at: point, buttons: Int(IOS_RED_BUTTON))

        case .changed:
            if let file = fopen("/tmp/window_events.log", "a") {
                fputs("[WINDOW-PRESS] moved to \(point)\n", file)
                fclose(file)
            }
            PharoBridge.shared.sendTouchMoved(to: point, buttons: Int(IOS_RED_BUTTON))

        case .ended, .cancelled:
            isDragging = false
            if let file = fopen("/tmp/window_events.log", "a") {
                fputs("[WINDOW-PRESS] ended at \(point)\n", file)
                fclose(file)
            }
            PharoBridge.shared.sendTouchUp(at: point)

        default:
            break
        }
    }
}
#endif
