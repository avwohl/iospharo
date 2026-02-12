/*
 * iosparoApp.swift
 *
 * Main SwiftUI app entry point for the iOS Pharo client.
 *
 * Event handling strategy:
 *   - Mouse position: UIHoverGestureRecognizer on PharoMTKView (Mac Catalyst)
 *   - Button clicks: touchesBegan/Ended on PharoMTKView (both platforms)
 *   - Scroll: UIPanGestureRecognizer (2-finger) on PharoMTKView (Mac Catalyst)
 *   - Keyboard: pressesBegan/Ended on PharoMTKView (both platforms)
 * All events handled via UIKit on PharoMTKView — no CGEventTap needed.
 */

import SwiftUI

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        // Redirect stderr to a file so we can capture VM fprintf output
        freopen("/tmp/iospharo-stderr.log", "w", stderr)
        NSLog("[APP] didFinishLaunching - stderr redirected to /tmp/iospharo-stderr.log")
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

