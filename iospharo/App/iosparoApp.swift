/*
 * iosparoApp.swift
 *
 * Main SwiftUI app entry point for the iOS Pharo client.
 *
 * Event handling strategy (Mac Catalyst):
 *   - Mouse position: UIHoverGestureRecognizer on PharoCanvasViewController's MTKView
 *   - Button clicks: CGEventTap (session-level, listen-only)
 *   - Scroll: UIPanGestureRecognizer (2-finger) on PharoCanvasViewController's MTKView
 * ONE delivery path per event type — no duplication.
 */

import SwiftUI

#if targetEnvironment(macCatalyst)
/// Manages CGEventTap for mouse button detection on Mac Catalyst.
/// Uses hover-tracked position from MouseEventHandler for coordinates.
enum EventTapManager {
    private static var eventTap: CFMachPort? = nil

    static func start() {
        // Event mask for mouse button events only
        let eventMask: CGEventMask = (1 << CGEventType.leftMouseDown.rawValue) |
                                      (1 << CGEventType.leftMouseUp.rawValue) |
                                      (1 << CGEventType.rightMouseDown.rawValue) |
                                      (1 << CGEventType.rightMouseUp.rawValue) |
                                      (1 << CGEventType.otherMouseDown.rawValue) |
                                      (1 << CGEventType.otherMouseUp.rawValue)

        guard let tap = CGEvent.tapCreate(
            tap: .cgSessionEventTap,
            place: .headInsertEventTap,
            options: .listenOnly,
            eventsOfInterest: eventMask,
            callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
                DispatchQueue.main.async {
                    let point = MouseEventHandler.shared.lastPosition

                    switch type {
                    case .leftMouseDown:
                        PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                        PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_RED_BUTTON)
                    case .leftMouseUp:
                        PharoBridge.shared.sendTouchUp(at: point, buttons: IOS_RED_BUTTON)
                    case .rightMouseDown:
                        PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                        PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_YELLOW_BUTTON)
                    case .rightMouseUp:
                        PharoBridge.shared.sendTouchUp(at: point, buttons: IOS_YELLOW_BUTTON)
                    case .otherMouseDown:
                        PharoBridge.shared.sendMouseMoved(to: point, modifiers: 0)
                        PharoBridge.shared.sendTouchDown(at: point, buttons: IOS_BLUE_BUTTON)
                    case .otherMouseUp:
                        PharoBridge.shared.sendTouchUp(at: point, buttons: IOS_BLUE_BUTTON)
                    default:
                        break
                    }
                }
                return Unmanaged.passUnretained(event)
            },
            userInfo: nil
        ) else {
            NSLog("[EVENT-TAP] Failed to create event tap - accessibility permissions may be needed")
            return
        }

        eventTap = tap
        let runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetCurrent(), runLoopSource, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        NSLog("[EVENT-TAP] CGEventTap created and enabled for button events")
    }
}
#endif

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        #if targetEnvironment(macCatalyst)
        EventTapManager.start()
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
        Thread.sleep(forTimeInterval: 0.1)
    }
}

#if targetEnvironment(macCatalyst)
class SceneDelegate: NSObject, UIWindowSceneDelegate {
    var window: UIWindow?
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
        }
    }
}

#if targetEnvironment(macCatalyst)
/// Singleton to track mouse position from hover gestures.
/// CGEventTap reads lastPosition for button event coordinates.
@MainActor
class MouseEventHandler: NSObject {
    static let shared = MouseEventHandler()
    var lastPosition: CGPoint = .zero
}
#endif
