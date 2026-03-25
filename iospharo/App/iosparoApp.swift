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
        #if DEBUG
        fputs("[SCENE] willConnectTo — configuring window size\n", stderr)
        fflush(stderr)
        #endif

        // Set window size constraints to prevent full-screen default
        if let sizeRestrictions = windowScene.sizeRestrictions {
            sizeRestrictions.minimumSize = CGSize(width: 800, height: 600)
            sizeRestrictions.maximumSize = CGSize(width: 2560, height: 1600)
        }

        // Request a reasonable initial window size
        if #available(macCatalyst 16.0, *) {
            let geometryPrefs = UIWindowScene.GeometryPreferences.Mac(systemFrame: CGRect(x: 100, y: 100, width: 1024, height: 768))
            windowScene.requestGeometryUpdate(geometryPrefs) { _ in }
        }

        // Move traffic light buttons above content so they don't overlap
        // the Pharo menu bar. Use a standard toolbar to create title bar space.
        if let titlebar = windowScene.titlebar {
            titlebar.titleVisibility = .hidden
            titlebar.toolbarStyle = .automatic
        }
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        // When the last window closes on Mac Catalyst, quit the app
        let remaining = UIApplication.shared.connectedScenes.filter { $0 != scene }
        if remaining.isEmpty {
            PharoBridge.shared.stop()
            exit(0)
        }
    }
}
#endif

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        #if DEBUG
        NSLog("[APP] didFinishLaunching")
        #endif
        return true
    }

    #if targetEnvironment(macCatalyst)
    func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let config = UISceneConfiguration(name: nil, sessionRole: connectingSceneSession.role)
        config.delegateClass = SceneDelegate.self
        #if DEBUG
        fputs("[APP] configurationForConnecting scene — using SceneDelegate\n", stderr)
        fflush(stderr)
        #endif
        return config
    }
    #endif

    func applicationWillTerminate(_ application: UIApplication) {
        #if DEBUG
        NSLog("[APP] applicationWillTerminate - stopping VM")
        #endif
        // Call stop() synchronously — we're already on the main thread.
        // The previous Task + Thread.sleep approach deadlocked because
        // sleeping the main thread prevented the Task from executing.
        MainActor.assumeIsolated {
            PharoBridge.shared.stop()
        }
    }
}

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
                .onOpenURL { url in
                    // Handle .image files opened from Files app or other apps
                    if url.pathExtension == "image" {
                        imageManager.importImage(from: url)
                    }
                }
        }
        .commands {
            // Enable Quit menu item (Cmd+Q) on Mac Catalyst
            CommandGroup(replacing: .appTermination) {
                Button("Quit Pharo Smalltalk") {
                    PharoBridge.shared.stop()
                    exit(0)
                }
                .keyboardShortcut("q")
            }
        }
    }
}
