/*
 * iosparoApp.swift
 *
 * Main SwiftUI app entry point for the iOS Pharo client.
 */

import SwiftUI
#if targetEnvironment(macCatalyst)
import GameController  // For GCMouse
#endif

/// App delegate to handle lifecycle events
class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey : Any]? = nil) -> Bool {
        #if targetEnvironment(macCatalyst)
        setupMouseHandling()
        #endif
        return true
    }

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
