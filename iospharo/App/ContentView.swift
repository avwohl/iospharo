/*
 * ContentView.swift
 *
 * Main content view for the iOS Pharo client.
 * Shows download progress, start button, or the Pharo canvas.
 */

import SwiftUI

// MARK: - Content View

struct ContentView: View {

    @EnvironmentObject var bridge: PharoBridge
    @EnvironmentObject var imageManager: ImageManager

    @State private var showingSettings = false
    @State private var showingKeyboard = false
    @State private var dragActive = false

    var body: some View {
        ZStack {
            // Background - must not intercept touches
            Color.black.edgesIgnoringSafeArea(.all)
                .allowsHitTesting(false)

            // Main content based on state
            if bridge.isRunning {
                // Pharo is running - show canvas with gesture overlay
                pharoCanvas

            } else if imageManager.isDownloading {
                // Downloading image
                downloadingView
            } else if imageManager.hasImage {
                // Ready to start
                readyView
            } else {
                // No image - show download option
                noImageView
            }

            // Error overlay
            if let error = bridge.errorMessage ?? imageManager.errorMessage {
                errorOverlay(message: error)
            }

            #if targetEnvironment(macCatalyst)
            // Debug: Test button to verify SwiftUI events work
            VStack {
                Spacer()
                HStack {
                    Button("TEST CLICK") {
                        if let file = fopen("/tmp/swiftui_button.log", "a") {
                            fputs("[BUTTON] Test button was clicked!\n", file)
                            fclose(file)
                        }
                        NSLog("[BUTTON] Test button was clicked!")
                    }
                    .padding()
                    .background(Color.red)
                    .foregroundColor(.white)
                    .cornerRadius(8)

                    Button("LEFT CLICK") {
                        // Programmatically send left-click events to VM
                        let testPoint = CGPoint(x: 512, y: 384)
                        if let file = fopen("/tmp/auto_test.log", "a") {
                            fputs("[AUTO] LEFT CLICK at \(testPoint)\n", file)
                            fclose(file)
                        }
                        bridge.sendMouseMoved(to: testPoint, modifiers: 0)
                        bridge.sendTouchDown(at: testPoint, buttons: Int(IOS_RED_BUTTON))
                        bridge.sendTouchUp(at: testPoint)
                    }
                    .padding()
                    .background(Color.green)
                    .foregroundColor(.white)
                    .cornerRadius(8)

                    Button("RIGHT CLICK") {
                        // Programmatically send right-click (yellow button) for world menu
                        let testPoint = CGPoint(x: 512, y: 384)
                        if let file = fopen("/tmp/auto_test.log", "a") {
                            fputs("[AUTO] RIGHT CLICK (buttons=2) at \(testPoint)\n", file)
                            fclose(file)
                        }
                        bridge.sendMouseMoved(to: testPoint, modifiers: 0)
                        bridge.sendTouchDown(at: testPoint, buttons: Int(IOS_YELLOW_BUTTON))
                        bridge.sendTouchUp(at: testPoint)
                    }
                    .padding()
                    .background(Color.orange)
                    .foregroundColor(.white)
                    .cornerRadius(8)

                    Spacer()
                }
                .padding()
            }
            #endif
        }
        // Note: SwiftUI gesture is needed for event routing even if it doesn't fire
        #if targetEnvironment(macCatalyst)
        .contentShape(Rectangle())
        .onHover { hovering in
            // This enables the hover gesture chain
        }
        #endif
        .onAppear {
            imageManager.checkForExistingImage()

            // Auto-start for development if image is available
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                if imageManager.hasImage && !bridge.isRunning {
                    NSLog("[iospharo] Auto-starting with image: %@", imageManager.imagePath ?? "nil")
                    startPharo()
                }
            }

            #if targetEnvironment(macCatalyst)
            // Auto-test: After 5 seconds, send a left-click, then at 6s send a right-click
            DispatchQueue.main.asyncAfter(deadline: .now() + 5.0) {
                if bridge.isRunning {
                    let testPoint = CGPoint(x: 512, y: 384)
                    if let file = fopen("/tmp/auto_test.log", "a") {
                        fputs("[AUTO] 5s: LEFT CLICK at \(testPoint)\n", file)
                        fclose(file)
                    }
                    bridge.sendMouseMoved(to: testPoint, modifiers: 0)
                    bridge.sendTouchDown(at: testPoint, buttons: Int(IOS_RED_BUTTON))
                    bridge.sendTouchUp(at: testPoint)
                }
            }
            // Test RIGHT CLICK at 6 seconds - this should trigger world menu
            DispatchQueue.main.asyncAfter(deadline: .now() + 6.0) {
                if bridge.isRunning {
                    let testPoint = CGPoint(x: 512, y: 384)
                    if let file = fopen("/tmp/auto_test.log", "a") {
                        fputs("[AUTO] 6s: RIGHT CLICK (buttons=2) at \(testPoint) - should show world menu!\n", file)
                        fclose(file)
                    }
                    bridge.sendMouseMoved(to: testPoint, modifiers: 0)
                    bridge.sendTouchDown(at: testPoint, buttons: Int(IOS_YELLOW_BUTTON))
                    bridge.sendTouchUp(at: testPoint)
                }
            }
            #endif
        }
    }

    // MARK: - Views

    private var pharoCanvas: some View {
        // Canvas view for Metal rendering
        // On Mac Catalyst, SwiftUI gestures capture clicks that UIKit misses
        #if targetEnvironment(macCatalyst)
        // Mac Catalyst: Let UIKit handle mouse events directly
        // Right-clicks will be handled by the underlying PharoCanvasView/MetalCanvasView
        // which receives proper mouse events from UIKit
        PharoCanvasView(bridge: bridge)
            .edgesIgnoringSafeArea(.all)
        #else
        PharoCanvasView(bridge: bridge)
            .edgesIgnoringSafeArea(.all)
        #endif
    }

    private var downloadingView: some View {
        VStack(spacing: 20) {
            Image(systemName: "arrow.down.circle")
                .font(.system(size: 60))
                .foregroundColor(.blue)

            Text("Downloading Pharo")
                .font(.title2)
                .foregroundColor(.white)

            ProgressView(value: imageManager.downloadProgress)
                .progressViewStyle(LinearProgressViewStyle(tint: .blue))
                .frame(width: 250)

            Text("\(Int(imageManager.downloadProgress * 100))%")
                .font(.caption)
                .foregroundColor(.gray)

            if let status = imageManager.statusMessage {
                Text(status)
                    .font(.caption)
                    .foregroundColor(.gray)
            }

            Button("Cancel") {
                imageManager.cancelDownload()
            }
            .foregroundColor(.red)
            .padding(.top, 20)
        }
        .padding()
    }

    private var readyView: some View {
        VStack(spacing: 30) {
            Image(systemName: "play.circle.fill")
                .font(.system(size: 80))
                .foregroundColor(.green)

            Text("Pharo Ready")
                .font(.title)
                .foregroundColor(.white)

            if let imageName = imageManager.imageName {
                Text(imageName)
                    .font(.caption)
                    .foregroundColor(.gray)
            }

            Button(action: startPharo) {
                HStack {
                    Image(systemName: "play.fill")
                    Text("Start Pharo")
                }
                .font(.headline)
                .foregroundColor(.white)
                .padding(.horizontal, 40)
                .padding(.vertical, 15)
                .background(Color.green)
                .cornerRadius(10)
            }

            Button("Download Different Image") {
                showingSettings = true
            }
            .font(.caption)
            .foregroundColor(.blue)
            .padding(.top, 20)
        }
        .sheet(isPresented: $showingSettings) {
            SettingsView()
        }
    }

    private var noImageView: some View {
        VStack(spacing: 30) {
            Image(systemName: "photo.badge.arrow.down")
                .font(.system(size: 60))
                .foregroundColor(.gray)

            Text("No Pharo Image")
                .font(.title2)
                .foregroundColor(.white)

            Text("Download a Pharo image to get started")
                .font(.body)
                .foregroundColor(.gray)

            Button(action: { imageManager.downloadDefaultImage() }) {
                HStack {
                    Image(systemName: "arrow.down.circle.fill")
                    Text("Download Pharo 12")
                }
                .font(.headline)
                .foregroundColor(.white)
                .padding(.horizontal, 40)
                .padding(.vertical, 15)
                .background(Color.blue)
                .cornerRadius(10)
            }
        }
    }

    private var toolbarOverlay: some View {
        HStack(spacing: 15) {
            // Keyboard toggle
            Button(action: { showingKeyboard.toggle() }) {
                Image(systemName: showingKeyboard ? "keyboard.fill" : "keyboard")
                    .foregroundColor(.white)
                    .padding(8)
                    .background(Color.black.opacity(0.5))
                    .clipShape(Circle())
            }

            // Settings
            Button(action: { showingSettings = true }) {
                Image(systemName: "gear")
                    .foregroundColor(.white)
                    .padding(8)
                    .background(Color.black.opacity(0.5))
                    .clipShape(Circle())
            }
        }
        .padding()
    }

    private var keyboardBar: some View {
        HStack {
            // Common keys
            ForEach(["Esc", "Tab", "Ctrl", "Alt", "Cmd"], id: \.self) { key in
                Button(key) {
                    sendSpecialKey(key)
                }
                .font(.caption)
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
                .background(Color.gray.opacity(0.3))
                .foregroundColor(.white)
                .cornerRadius(5)
            }

            Spacer()

            // Text input field
            TextField("Type here", text: .constant(""))
                .textFieldStyle(RoundedBorderTextFieldStyle())
                .frame(width: 150)
        }
        .padding()
        .background(Color.black.opacity(0.8))
    }

    private func errorOverlay(message: String) -> some View {
        VStack {
            Spacer()

            HStack {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundColor(.yellow)
                Text(message)
                    .foregroundColor(.white)
            }
            .padding()
            .background(Color.red.opacity(0.8))
            .cornerRadius(10)
            .padding()
        }
    }

    // MARK: - Actions

    private func startPharo() {
        guard let imagePath = imageManager.imagePath else { return }

        if bridge.loadImage(at: imagePath) {
            bridge.start()
        }
    }

    private func sendSpecialKey(_ key: String) {
        switch key {
        case "Esc":
            bridge.sendKeyTyped(Character(UnicodeScalar(27)!))
        case "Tab":
            bridge.sendKeyTyped(Character(UnicodeScalar(9)!))
        case "Ctrl":
            // Toggle ctrl modifier (simplified)
            break
        case "Alt":
            break
        case "Cmd":
            break
        default:
            break
        }
    }
}

// MARK: - Settings View

struct SettingsView: View {

    @EnvironmentObject var imageManager: ImageManager
    @Environment(\.dismiss) var dismiss
    @State private var showingDiagnostics = false

    var body: some View {
        NavigationView {
            List {
                Section("Image Management") {
                    Button("Download Pharo 12") {
                        imageManager.downloadDefaultImage()
                        dismiss()
                    }

                    Button("Download Pharo 11") {
                        imageManager.downloadImage(version: "110")
                        dismiss()
                    }
                }

                Section("Developer Tools") {
                    Button("VM Diagnostics") {
                        showingDiagnostics = true
                    }
                }

                Section("About") {
                    HStack {
                        Text("Device")
                        Spacer()
                        Text(PharoBridge.shared.deviceModel)
                            .foregroundColor(.gray)
                    }

                    HStack {
                        Text("iOS Version")
                        Spacer()
                        Text(PharoBridge.shared.systemVersion)
                            .foregroundColor(.gray)
                    }

                    HStack {
                        Text("VM Version")
                        Spacer()
                        Text("12.0.0-ios")
                            .foregroundColor(.gray)
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showingDiagnostics) {
                DiagnosticsView()
            }
        }
    }
}

// MARK: - Diagnostics View

struct DiagnosticsView: View {
    @Environment(\.dismiss) var dismiss
    @State private var results: [(String, Bool, String)] = []

    var body: some View {
        NavigationView {
            List {
                Section("VM Core Tests") {
                    testRow("Memory Allocation", testMemory)
                    testRow("Integer Tagging", testIntegerTag)
                    testRow("ASLR Info", testASLR)
                }

                #if PHARO_IOS_OOP_WRAPPER
                Section("C++ Oop Tests") {
                    testRow("Space Encoding", testSpaceEncoding)
                    testRow("Pointer Roundtrip", testPointerRoundtrip)
                }
                #else
                Section("Build Mode") {
                    Text("Standard C mode (not C++ Oop)")
                        .foregroundColor(.secondary)
                }
                #endif

                if !results.isEmpty {
                    Section("Results") {
                        ForEach(results, id: \.0) { name, passed, detail in
                            HStack {
                                Image(systemName: passed ? "checkmark.circle.fill" : "xmark.circle.fill")
                                    .foregroundColor(passed ? .green : .red)
                                VStack(alignment: .leading) {
                                    Text(name)
                                    Text(detail)
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle("VM Diagnostics")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func testRow(_ name: String, _ test: @escaping () -> (Bool, String)) -> some View {
        Button(name) {
            let (passed, detail) = test()
            results.append((name, passed, detail))
        }
    }

    private func testMemory() -> (Bool, String) {
        let size = 1024 * 1024
        guard let ptr = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
              ptr != MAP_FAILED else {
            return (false, "mmap failed")
        }
        ptr.storeBytes(of: UInt64(0xDEADBEEF), as: UInt64.self)
        let val = ptr.load(as: UInt64.self)
        munmap(ptr, size)
        return (val == 0xDEADBEEF, "mmap/munmap working")
    }

    private func testIntegerTag() -> (Bool, String) {
        let value: Int64 = 42
        let encoded = (value << 3) | 1
        let decoded = encoded >> 3
        return (decoded == value, "42 -> 0x\(String(encoded, radix: 16)) -> \(decoded)")
    }

    private func testASLR() -> (Bool, String) {
        var stackVar: Int = 0
        let stackAddr = withUnsafePointer(to: &stackVar) { UInt(bitPattern: $0) }
        return (true, "Stack @ 0x\(String(stackAddr, radix: 16).prefix(6))...")
    }

    private func testSpaceEncoding() -> (Bool, String) {
        let addr: UInt64 = 0x100000008
        let encoded = addr | (1 << 1)  // old space
        let space = (encoded >> 1) & 0x3
        return (space == 1, "Old space encoding correct")
    }

    private func testPointerRoundtrip() -> (Bool, String) {
        let size = 1024
        guard let ptr = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
              ptr != MAP_FAILED else {
            return (false, "Alloc failed")
        }
        defer { munmap(ptr, size) }

        let addr = UInt64(UInt(bitPattern: ptr))
        let encoded = addr | (1 << 1)
        let decoded = UnsafeMutableRawPointer(bitPattern: UInt(encoded & ~UInt64(0x7)))

        ptr.storeBytes(of: UInt64(0xCAFEBABE), as: UInt64.self)
        let readBack = decoded?.load(as: UInt64.self) ?? 0
        return (readBack == 0xCAFEBABE, readBack == 0xCAFEBABE ? "Roundtrip OK" : "Corrupted")
    }
}

// MARK: - Preview

#Preview {
    ContentView()
        .environmentObject(PharoBridge.shared)
        .environmentObject(ImageManager())
}
