/*
 * iosparoApp.swift
 *
 * Main SwiftUI app entry point for the iOS Pharo client.
 */

import SwiftUI

@main
struct iosparoApp: App {

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
