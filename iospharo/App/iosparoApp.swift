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
        fputs("[APP] didFinishLaunching via fputs\n", stderr)
        fflush(stderr)
        return true
    }


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
        fputs("[APP] iosparoApp.body accessed\n", stderr)
        fflush(stderr)
        return WindowGroup {
            ContentView()
                .environmentObject(bridge)
                .environmentObject(imageManager)
                .onAppear {
                    fputs("[APP] ContentView.onAppear\n", stderr)
                    fflush(stderr)
                }
        }
    }
}

