/*
 * iosparoApp.swift
 *
 * Main SwiftUI app entry point for the iOS Pharo client.
 */

import SwiftUI

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func applicationWillTerminate(_ application: UIApplication) {
        NSLog("[APP] applicationWillTerminate - stopping VM")
        Task { @MainActor in
            PharoBridge.shared.stop()
        }
        // Give the VM a moment to stop cleanly
        Thread.sleep(forTimeInterval: 0.1)
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
        }
    }
}
