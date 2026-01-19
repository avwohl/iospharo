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

                    // Forward to Pharo VM synchronously (we're already on main thread)
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

        // Always call super to let normal event handling continue
        super.sendEvent(event)
    }
}
#endif

/// Swift wrapper for NSEvent monitoring on Mac Catalyst
enum NSEventMonitorSwift {
    // NSEvent types we care about
    static let leftMouseDown = 1
    static let leftMouseUp = 2
    static let rightMouseDown = 3
    static let rightMouseUp = 4
    static let mouseMoved = 5
    static let leftMouseDragged = 6
    static let rightMouseDragged = 7

    static func checkAppActivation() {
        // Try to access NSApplication's isActive property
        guard let nsAppClass = NSClassFromString("NSApplication"),
              let sharedApp = (nsAppClass as AnyObject).perform(NSSelectorFromString("sharedApplication"))?.takeUnretainedValue() else {
            return
        }

        // Check isActive
        let isActiveSel = NSSelectorFromString("isActive")
        let isActive = sharedApp.perform(isActiveSel) != nil

        // Check currentEvent
        let currentEventSel = NSSelectorFromString("currentEvent")
        let currentEvent = sharedApp.perform(currentEventSel)?.takeUnretainedValue()

        // Try to get mouse location via Core Graphics
        var mouseLocationStr = "unavailable"
        // CGEvent doesn't require special permissions for just getting location
        if let event = CGEvent(source: nil) {
            let location = event.location
            mouseLocationStr = "(x=\(Int(location.x)), y=\(Int(location.y)))"
        }

        if let file = fopen("/tmp/app_activation.log", "a") {
            fputs("[APP-ACTIVATION] isActive=\(isActive) mouseLocation=\(mouseLocationStr) currentEvent=\(String(describing: currentEvent))\n", file)
            fclose(file)
        }
    }

    static func startActivationMonitor() {
        // Check activation status every second (for debugging)
        Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { _ in
            checkAppActivation()
        }
    }

    // State for mouse polling
    private static var lastPolledPosition = CGPoint.zero
    private static var wasLeftButtonDown = false
    private static var wasRightButtonDown = false
    private static var wasMiddleButtonDown = false
    private static var pollCount = 0
    private static var pollLogFile: UnsafeMutablePointer<FILE>? = nil
    private static var eventTap: CFMachPort? = nil
    private static var tapLogFile: UnsafeMutablePointer<FILE>? = nil

    /// Start CGEventTap to capture mouse button events (more reliable than buttonState polling)
    static func startEventTap() {
        tapLogFile = fopen("/tmp/cgeventtap.log", "w")
        if let file = tapLogFile {
            fputs("[TAP] Setting up CGEventTap for mouse button events\n", file)
            fflush(file)
        }

        // Event mask for mouse button events
        let eventMask: CGEventMask = (1 << CGEventType.leftMouseDown.rawValue) |
                                      (1 << CGEventType.leftMouseUp.rawValue) |
                                      (1 << CGEventType.rightMouseDown.rawValue) |
                                      (1 << CGEventType.rightMouseUp.rawValue) |
                                      (1 << CGEventType.otherMouseDown.rawValue) |
                                      (1 << CGEventType.otherMouseUp.rawValue)

        // Create event tap
        guard let tap = CGEvent.tapCreate(
            tap: .cgSessionEventTap,
            place: .headInsertEventTap,
            options: .listenOnly,  // Just listen, don't modify events
            eventsOfInterest: eventMask,
            callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
                // Get hover-tracked position for correct window-local coordinates
                let hoverPos = MouseEventHandler.shared.lastPosition

                if let file = NSEventMonitorSwift.tapLogFile {
                    let loc = event.location
                    fputs("[TAP] Event type=\(type.rawValue) screenPos=(\(Int(loc.x)),\(Int(loc.y))) hoverPos=(\(Int(hoverPos.x)),\(Int(hoverPos.y)))\n", file)
                    fflush(file)
                }

                // Forward button events to Pharo using hover-tracked coordinates
                DispatchQueue.main.async {
                    let point = MouseEventHandler.shared.lastPosition

                    switch type {
                    case .leftMouseDown:
                        if let f = NSEventMonitorSwift.tapLogFile {
                            fputs("[TAP] LEFT DOWN -> Pharo at (\(Int(point.x)),\(Int(point.y)))\n", f)
                            fflush(f)
                        }
                        PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                        PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)

                    case .leftMouseUp:
                        PharoBridge.shared.sendTouchUp(at: point)

                    case .rightMouseDown:
                        if let f = NSEventMonitorSwift.tapLogFile {
                            fputs("[TAP] RIGHT DOWN -> Pharo at (\(Int(point.x)),\(Int(point.y)))\n", f)
                            fflush(f)
                        }
                        PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                        PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)

                    case .rightMouseUp:
                        PharoBridge.shared.sendTouchUp(at: point)

                    case .otherMouseDown:
                        PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                        PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)

                    case .otherMouseUp:
                        PharoBridge.shared.sendTouchUp(at: point)

                    default:
                        break
                    }
                }

                return Unmanaged.passUnretained(event)  // Let the event continue
            },
            userInfo: nil
        ) else {
            if let file = tapLogFile {
                fputs("[TAP] ERROR: Failed to create event tap - accessibility permissions may be needed\n", file)
                fflush(file)
            }
            return
        }

        eventTap = tap

        // Add to run loop
        let runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)

        if let file = tapLogFile {
            fputs("[TAP] Event tap created and enabled successfully\n", file)
            fflush(file)
        }
    }

    static func startMousePolling() {
        pollLogFile = fopen("/tmp/mouse_polling.log", "w")
        if let file = pollLogFile {
            fputs("[POLL] Starting mouse polling via CGEvent\n", file)
            fflush(file)
        }

        // Start event tap for button detection (more reliable than buttonState polling)
        startEventTap()

        // Poll mouse state 60 times per second (for position tracking only now)
        Timer.scheduledTimer(withTimeInterval: 1.0/60.0, repeats: true) { _ in
            pollMouseState()
        }
    }

    private static func pollMouseState() {
        pollCount += 1

        // Get current mouse position using CGEvent
        guard let event = CGEvent(source: nil) else { return }
        let screenPosition = event.location  // CGEvent uses bottom-left origin

        // Get window info
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let window = scene.windows.first else { return }

        // Get actual window position on screen by accessing NSWindow through Objective-C runtime
        var windowScreenFrame = CGRect.zero
        var screenHeight: CGFloat = 1080

        // Try to get NSWindow frame which has actual screen coordinates
        if let nsWindow = (window.value(forKey: "hostWindow") as AnyObject?) as? NSObject,
           nsWindow.responds(to: NSSelectorFromString("frame")) {
            if let frameValue = nsWindow.value(forKey: "frame") as? NSValue {
                windowScreenFrame = frameValue.cgRectValue
            }
            // Also get screen height from NSScreen
            if let nsScreen = nsWindow.value(forKey: "screen") as AnyObject?,
               let screenFrame = (nsScreen as? NSObject)?.value(forKey: "frame") as? NSValue {
                screenHeight = screenFrame.cgRectValue.height
            }
        }

        // If we couldn't get NSWindow frame, fall back to estimation
        if windowScreenFrame == .zero {
            // Estimate: assume window is somewhere on screen with a title bar
            windowScreenFrame = CGRect(x: 0, y: 0, width: window.frame.width, height: window.frame.height)
            screenHeight = window.screen.bounds.height
        }

        // CGEvent Y is from bottom, NSWindow frame Y is also from bottom
        // Convert CGEvent position to window-local coordinates
        // windowScreenFrame.origin is bottom-left of window content area
        let windowX = screenPosition.x - windowScreenFrame.origin.x
        let windowY = screenPosition.y - windowScreenFrame.origin.y  // Both bottom-left origin

        // Now convert to top-left origin for UIKit/Pharo
        let windowYTopLeft = windowScreenFrame.height - windowY

        let windowPosition = CGPoint(x: windowX, y: windowYTopLeft)

        // Check if mouse is inside window (use windowY before top-left conversion for bounds check)
        let isInsideWindow = windowX >= 0 && windowX < windowScreenFrame.width &&
                            windowY >= 0 && windowY < windowScreenFrame.height

        // NOTE: CGEventSource.buttonState() doesn't work reliably on Mac Catalyst
        // Button detection is now handled by CGEventTap in startEventTap()
        // This polling is only for position tracking

        // Debug: always log first 50 polls to see coordinate conversion
        if pollLogFile != nil && pollCount <= 50 {
            fputs("[POLL-DEBUG] #\(pollCount) screen=(\(Int(screenPosition.x)),\(Int(screenPosition.y))) nsWinFrame=(\(Int(windowScreenFrame.origin.x)),\(Int(windowScreenFrame.origin.y)),\(Int(windowScreenFrame.width)),\(Int(windowScreenFrame.height))) localPos=(\(Int(windowX)),\(Int(windowY))) uiPos=(\(Int(windowPosition.x)),\(Int(windowPosition.y))) inside=\(isInsideWindow)\n", pollLogFile!)
            fflush(pollLogFile!)
        }

        // Only process if mouse is inside window
        if isInsideWindow {
            // Log occasionally
            if pollLogFile != nil && (pollCount <= 100 || pollCount % 60 == 0) {
                fputs("[POLL] #\(pollCount) pos=\(Int(windowPosition.x)),\(Int(windowPosition.y))\n", pollLogFile!)
                fflush(pollLogFile!)
            }

            // Update hover-tracked position (used by CGEventTap for button events)
            if windowPosition != lastPolledPosition {
                // Mouse moved - update position only (button events are handled by CGEventTap)
                DispatchQueue.main.async {
                    MouseEventHandler.shared.lastPosition = windowPosition
                    PharoBridge.shared.sendMouseMoved(to: windowPosition, modifiers: 0)
                }
                lastPolledPosition = windowPosition
            }
        }
        // NOTE: Button state tracking removed - CGEventTap handles button detection now
    }

    static func installMonitor() {
        // Install the Objective-C event monitor with our callback
        // IMPORTANT: NSEvent's locationInWindow coordinates are unreliable on Mac Catalyst.
        // We use NSEvent ONLY to detect WHEN buttons are pressed, not WHERE.
        // For coordinates, we use the hover-tracked position from UIKit gesture recognizers.
        NSEventMonitorHelper.installMouseMonitor { (eventType: Int32, x: Double, y: Double, buttonNumber: Int32) in

            if let file = fopen("/tmp/nsevent_swift.log", "a") {
                fputs("[NSEVENT-SWIFT] type=\(eventType) nsEventXY=(\(Int(x)),\(Int(y))) btn=\(buttonNumber)\n", file)
                fclose(file)
            }

            // Forward button events to Pharo VM using hover-tracked coordinates
            DispatchQueue.main.async {
                // Get the correct position from hover tracking (UIKit coordinates, already correct)
                let point = MouseEventHandler.shared.lastPosition

                switch Int(eventType) {
                case leftMouseDown:
                    if let file = fopen("/tmp/nsevent_swift.log", "a") {
                        fputs("[NSEVENT-SWIFT] LEFT DOWN at hover-tracked pos=(\(Int(point.x)),\(Int(point.y)))\n", file)
                        fclose(file)
                    }
                    PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                    PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)

                case leftMouseUp:
                    PharoBridge.shared.sendTouchUp(at: point)

                case rightMouseDown:
                    if let file = fopen("/tmp/nsevent_swift.log", "a") {
                        fputs("[NSEVENT-SWIFT] RIGHT DOWN at hover-tracked pos=(\(Int(point.x)),\(Int(point.y)))\n", file)
                        fclose(file)
                    }
                    PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                    PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)  // Yellow = world menu

                case rightMouseUp:
                    PharoBridge.shared.sendTouchUp(at: point)

                case leftMouseDragged:
                    PharoBridge.shared.sendTouchMoved(to: point, buttons: IOS_RED_BUTTON)

                case rightMouseDragged:
                    PharoBridge.shared.sendTouchMoved(to: point, buttons: IOS_YELLOW_BUTTON)

                case mouseMoved:
                    // Ignore mouseMoved from NSEvent - UIKit gesture recognizers handle hover tracking
                    break

                default:
                    break
                }
            }
        }
    }
}

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        #if targetEnvironment(macCatalyst)
        // Swizzle UIApplication.sendEvent to intercept all events (backup)
        ApplicationSwizzler.swizzleSendEvent()
        setupMouseHandling()
        // NSEvent monitor for button detection (uses hover-tracked coordinates, not NSEvent's)
        NSEventMonitorSwift.installMonitor()
        // CGEvent polling for hover tracking (button detection moved to NSEvent monitor)
        NSEventMonitorSwift.startMousePolling()
        // Start activation monitoring (for debugging)
        NSEventMonitorSwift.startActivationMonitor()
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
            if let file = fopen("/tmp/gcmouse_setup.log", "w") {
                fputs("[GCMOUSE-SETUP] Mouse already connected: \(mouse)\n", file)
                fclose(file)
            }
            configureMouseInput(mouse)
        } else {
            if let file = fopen("/tmp/gcmouse_setup.log", "w") {
                fputs("[GCMOUSE-SETUP] No mouse connected at startup - waiting for GCMouseDidConnect\n", file)
                fclose(file)
            }
        }
        NSLog("[APP] GCMouse event handling set up")
    }

    @objc private func mouseDidConnect(_ notification: Notification) {
        if let file = fopen("/tmp/gcmouse_setup.log", "a") {
            fputs("[GCMOUSE-SETUP] mouseDidConnect notification received\n", file)
            fclose(file)
        }
        if let mouse = notification.object as? GCMouse {
            if let file = fopen("/tmp/gcmouse_setup.log", "a") {
                fputs("[GCMOUSE-SETUP] Configuring mouse: \(mouse)\n", file)
                fclose(file)
            }
            configureMouseInput(mouse)
            NSLog("[APP] Mouse connected: \(mouse)")
        }
    }

    private func configureMouseInput(_ mouse: GCMouse) {
        if let file = fopen("/tmp/gcmouse_setup.log", "a") {
            fputs("[GCMOUSE-SETUP] configureMouseInput called for: \(mouse)\n", file)
            fclose(file)
        }
        guard let input = mouse.mouseInput else {
            if let file = fopen("/tmp/gcmouse_setup.log", "a") {
                fputs("[GCMOUSE-SETUP] ERROR: mouseInput is nil!\n", file)
                fclose(file)
            }
            return
        }
        if let file = fopen("/tmp/gcmouse_setup.log", "a") {
            fputs("[GCMOUSE-SETUP] Got mouseInput, setting up handlers\n", file)
            fclose(file)
        }

        // Left button - use last known position from hover tracking
        input.leftButton.pressedChangedHandler = { [weak self] button, value, pressed in
            let pos = MouseEventHandler.shared.lastPosition
            if let file = fopen("/tmp/gcmouse.log", "a") {
                fputs("[GCMOUSE] leftButton pressed=\(pressed) at \(pos)\n", file)
                fclose(file)
            }

            // Forward to Pharo VM
            DispatchQueue.main.async {
                if pressed {
                    PharoBridge.shared.sendMouseMoved(to: pos, modifiers: 0)
                    PharoBridge.shared.sendTouchDown(at: pos, buttons: Int(IOS_RED_BUTTON))
                } else {
                    PharoBridge.shared.sendTouchUp(at: pos)
                }
            }
        }

        // Right button (yellow button in Pharo = world menu)
        input.rightButton?.pressedChangedHandler = { [weak self] button, value, pressed in
            let pos = MouseEventHandler.shared.lastPosition
            if let file = fopen("/tmp/gcmouse.log", "a") {
                fputs("[GCMOUSE] rightButton pressed=\(pressed) at \(pos)\n", file)
                fclose(file)
            }

            // Forward to Pharo VM (yellow button = 2 = world menu)
            DispatchQueue.main.async {
                if let file = fopen("/tmp/gcmouse.log", "a") {
                    fputs("[GCMOUSE] DISPATCH executing pressed=\(pressed) pos=\(pos)\n", file)
                    fclose(file)
                }
                if pressed {
                    PharoBridge.shared.sendMouseMoved(to: pos, modifiers: 0)
                    PharoBridge.shared.sendTouchDown(at: pos, buttons: 2)  // Yellow button = world menu
                    if let file = fopen("/tmp/gcmouse.log", "a") {
                        fputs("[GCMOUSE] sendTouchDown called with buttons=2\n", file)
                        fclose(file)
                    }
                } else {
                    PharoBridge.shared.sendTouchUp(at: pos)
                }
            }
        }

        // Mouse movement - we don't use this since hover gesture tracks position
        input.mouseMovedHandler = { mouse, deltaX, deltaY in
            // Position tracked via UIHoverGestureRecognizer instead
        }

        NSLog("[APP] Mouse input handlers configured with click forwarding")
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

                    // Forward to Pharo VM synchronously (we're already on main thread)
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

            // Add UIPointerInteraction to the hosting view for position tracking
            if let hostingView = window.subviews.first?.subviews.first?.subviews.first {
                // Add pointer interaction for position tracking
                let pointerInteraction = UIPointerInteraction(delegate: JsonBasedPointerDelegate.shared)
                hostingView.addInteraction(pointerInteraction)

                // Also add press gesture
                let hostingPress = UILongPressGestureRecognizer(
                    target: MouseEventHandler.shared,
                    action: #selector(MouseEventHandler.handleWindowPress(_:))
                )
                hostingPress.minimumPressDuration = 0
                hostingPress.allowableMovement = 10000
                hostingView.addGestureRecognizer(hostingPress)

                if let file = fopen("/tmp/window_setup.log", "a") {
                    fputs("[WINDOW] Added UIPointerInteraction to hosting view: \(hostingView)\n", file)
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

    // Last known mouse position from hover tracking - used by GCMouse click handlers
    var lastPosition: CGPoint = .zero
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

/// UIPointerInteractionDelegate to track pointer position on Mac Catalyst
class JsonBasedPointerDelegate: NSObject, UIPointerInteractionDelegate {
    static let shared = JsonBasedPointerDelegate()

    private var regionRequestCount = 0

    func pointerInteraction(_ interaction: UIPointerInteraction, regionFor request: UIPointerRegionRequest, defaultRegion: UIPointerRegion) -> UIPointerRegion? {
        let point = request.location
        regionRequestCount += 1

        // Log position tracking
        if regionRequestCount <= 500 || regionRequestCount % 100 == 0 {
            if let file = fopen("/tmp/pointer_interaction.log", "a") {
                fputs("[POINTER] #\(regionRequestCount) regionFor at \(point)\n", file)
                fclose(file)
            }
        }

        // Update last known position
        DispatchQueue.main.async {
            MouseEventHandler.shared.lastPosition = point
            // Send mouse move to VM
            PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
        }

        // Return the full view as the active region
        return defaultRegion
    }

    func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? {
        // Use default pointer style (arrow cursor)
        return nil
    }

    func pointerInteraction(_ interaction: UIPointerInteraction, willEnter region: UIPointerRegion, animator: any UIPointerInteractionAnimating) {
        if let file = fopen("/tmp/pointer_interaction.log", "a") {
            fputs("[POINTER] willEnter region\n", file)
            fclose(file)
        }
    }

    func pointerInteraction(_ interaction: UIPointerInteraction, willExit region: UIPointerRegion, animator: any UIPointerInteractionAnimating) {
        if let file = fopen("/tmp/pointer_interaction.log", "a") {
            fputs("[POINTER] willExit region\n", file)
            fclose(file)
        }
    }
}
#endif
