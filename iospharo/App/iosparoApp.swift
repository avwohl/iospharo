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

#if targetEnvironment(macCatalyst)
/// Scene delegate to configure window size on Mac Catalyst
class SceneDelegate: NSObject, UIWindowSceneDelegate {
    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        fputs("[SCENE] willConnectTo — configuring window size\n", stderr)
        fflush(stderr)

        // Set window size constraints to prevent full-screen default
        if let sizeRestrictions = windowScene.sizeRestrictions {
            sizeRestrictions.minimumSize = CGSize(width: 800, height: 600)
            sizeRestrictions.maximumSize = CGSize(width: 2560, height: 1600)
        }

        // Request a reasonable initial window size
        if #available(macCatalyst 16.0, *) {
            let geometryPrefs = UIWindowScene.GeometryPreferences.Mac(systemFrame: CGRect(x: 100, y: 100, width: 1024, height: 768))
            windowScene.requestGeometryUpdate(geometryPrefs) { error in
                fputs("[SCENE] geometry update error: \(error.localizedDescription)\n", stderr)
                fflush(stderr)
            }
        }
    }
}
#endif

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        // Note: stderr is already redirected to /tmp/iospharo-stderr.log by C++ PlatformBridge.
        // Do NOT freopen here — it would truncate the VM init output.
        NSLog("[APP] didFinishLaunching")
        return true
    }

    #if targetEnvironment(macCatalyst)
    func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: nil, sessionRole: connectingSceneSession.role)
        config.delegateClass = SceneDelegate.self
        fputs("[APP] configurationForConnecting scene — using SceneDelegate\n", stderr)
        fflush(stderr)
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

@main
struct iosparoApp: App {

    @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var bridge = PharoBridge.shared
    @StateObject private var imageManager = ImageManager()

    var body: some Scene {
        return WindowGroup {
            ContentView()
                .environmentObject(bridge)
                .environmentObject(imageManager)
                .onOpenURL { url in
                    // Handle .image files opened from Files app or other apps
                    if url.pathExtension == "image" {
                        imageManager.importImage(from: url)
                    }
                }
        }
    }
}
