import SwiftUI

@main
struct PharoApp: App {
    @StateObject private var vmController = VMController()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(vmController)
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1024, height: 768)
    }
}
