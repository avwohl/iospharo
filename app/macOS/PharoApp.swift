import SwiftUI

@main
struct PharoApp: App {
    @StateObject private var vmController = VMController()

    init() {
        // Debug: write to file on init
        FileManager.default.createFile(atPath: "/tmp/pharoapp_init.log", contents: "PharoApp.init() called\n".data(using: .utf8))
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(vmController)
        }
        // .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1024, height: 768)
    }
}
