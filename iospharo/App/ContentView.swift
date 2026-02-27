/*
 * ContentView.swift
 *
 * Main content view — routes between the image library and the Pharo canvas.
 */

import SwiftUI

// MARK: - Content View

struct ContentView: View {

    @EnvironmentObject var bridge: PharoBridge
    @EnvironmentObject var imageManager: ImageManager

    @AppStorage("hasSeenGestureHelp") private var hasSeenGestureHelp = false
    @State private var showingHelp = false

    var body: some View {
        ZStack {
            if bridge.isRunning {
                // Pharo is running — full-screen canvas
                pharoCanvas
            } else {
                // Image library (handles empty state, downloads, list)
                ImageLibraryView()
            }

            // Error overlay for VM errors
            if let error = bridge.errorMessage {
                errorOverlay(message: error)
            }
        }
        .onAppear {
            imageManager.load()
        }
    }

    // MARK: - Views

    private var pharoCanvas: some View {
        ZStack {
            PharoCanvasView(bridge: bridge)
                #if targetEnvironment(macCatalyst)
                .ignoresSafeArea()
                #endif

            #if !targetEnvironment(macCatalyst)
            FloatingToolbar(
                ctrlActive: $bridge.ctrlModifierActive,
                keyboardVisible: $bridge.keyboardVisible,
                showHelp: $showingHelp
            )

            // Gesture help overlay — shown on first launch or when help tapped
            if showingHelp || !hasSeenGestureHelp {
                GestureHelpOverlay {
                    hasSeenGestureHelp = true
                    showingHelp = false
                }
            }
            #endif
        }
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
        let encoded = addr | (1 << 1)
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

// MARK: - Floating Toolbar (iOS only)

#if !targetEnvironment(macCatalyst)
struct FloatingToolbar: View {
    @Binding var ctrlActive: Bool
    @Binding var keyboardVisible: Bool
    @Binding var showHelp: Bool
    @State private var offset: CGSize = .zero
    @State private var dragOffset: CGSize = .zero

    var body: some View {
        VStack {
            Spacer()
            HStack {
                Spacer()
                HStack(spacing: 8) {
                    // Help button
                    FloatingButton(
                        icon: "questionmark",
                        label: nil,
                        isActive: false,
                        action: { showHelp = true }
                    )

                    // Keyboard toggle
                    FloatingButton(
                        icon: "keyboard",
                        label: nil,
                        isActive: keyboardVisible,
                        action: {
                            keyboardVisible.toggle()
                            if let view = gPharoMTKView {
                                if keyboardVisible {
                                    view.becomeFirstResponder()
                                } else {
                                    view.resignFirstResponder()
                                }
                            }
                        }
                    )

                    // Virtual Ctrl key — stays active until tapped again
                    FloatingButton(
                        icon: "control",
                        label: "Ctrl",
                        isActive: ctrlActive,
                        action: {
                            ctrlActive.toggle()
                        }
                    )
                }
                .offset(x: offset.width + dragOffset.width,
                        y: offset.height + dragOffset.height)
                .gesture(
                    DragGesture()
                        .onChanged { value in
                            dragOffset = value.translation
                        }
                        .onEnded { value in
                            offset.width += value.translation.width
                            offset.height += value.translation.height
                            dragOffset = .zero
                        }
                )
                .padding(.trailing, 16)
                .padding(.bottom, 80)
            }
        }
        .allowsHitTesting(true)
    }
}

struct FloatingButton: View {
    let icon: String
    let label: String?
    let isActive: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Group {
                if let label = label {
                    Text(label)
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                } else {
                    Image(systemName: icon)
                        .font(.system(size: 16))
                }
            }
            .foregroundColor(isActive ? .white : .gray)
            .frame(width: 40, height: 40)
            .background(isActive ? Color.blue : Color.gray.opacity(0.3))
            .clipShape(Circle())
        }
    }
}

// MARK: - Gesture Help Overlay

struct GestureHelpOverlay: View {
    let onDismiss: () -> Void

    var body: some View {
        ZStack {
            // Dim background
            Color.black.opacity(0.7)
                .ignoresSafeArea()
                .onTapGesture { onDismiss() }

            VStack(spacing: 20) {
                Text("Quick Start")
                    .font(.title2)
                    .fontWeight(.bold)
                    .foregroundColor(.white)

                VStack(alignment: .leading, spacing: 14) {
                    helpRow("hand.tap", "Tap", "Left-click (select, activate)")
                    helpRow("hand.tap", "Long press", "Right-click (context menu)")
                    helpRow("hand.draw", "Two-finger scroll", "Scroll lists and text")
                    helpRow("hand.tap", "Two-finger tap", "Right-click (alternative)")

                    Divider().background(Color.white.opacity(0.3))

                    helpRow("keyboard", "Keyboard button", "Show/hide the soft keyboard")
                    helpRow("control", "Ctrl button", "Hold Ctrl for shortcuts")

                    Divider().background(Color.white.opacity(0.3))

                    VStack(alignment: .leading, spacing: 4) {
                        Text("With Ctrl active or hardware keyboard:")
                            .font(.caption)
                            .foregroundColor(.white.opacity(0.7))
                        HStack(spacing: 16) {
                            shortcutLabel("Ctrl+D", "Do It")
                            shortcutLabel("Ctrl+P", "Print It")
                            shortcutLabel("Ctrl+E", "Inspect It")
                        }
                    }
                }

                Button {
                    onDismiss()
                } label: {
                    Text("Got it")
                        .font(.headline)
                        .foregroundColor(.white)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                        .background(Color.blue)
                        .cornerRadius(10)
                }
                .padding(.top, 8)

                Text("Tap ? to see this again")
                    .font(.caption)
                    .foregroundColor(.white.opacity(0.5))
            }
            .padding(24)
            .frame(maxWidth: 360)
            .background(Color(.systemGray6).opacity(0.95))
            .cornerRadius(16)
            .environment(\.colorScheme, .dark)
        }
        .transition(.opacity)
    }

    private func helpRow(_ icon: String, _ gesture: String, _ description: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.system(size: 18))
                .foregroundColor(.blue)
                .frame(width: 28, alignment: .center)
            VStack(alignment: .leading, spacing: 1) {
                Text(gesture)
                    .font(.subheadline)
                    .fontWeight(.medium)
                    .foregroundColor(.white)
                Text(description)
                    .font(.caption)
                    .foregroundColor(.white.opacity(0.7))
            }
        }
    }

    private func shortcutLabel(_ key: String, _ action: String) -> some View {
        VStack(spacing: 2) {
            Text(key)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundColor(.white)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(Color.gray.opacity(0.4))
                .cornerRadius(4)
            Text(action)
                .font(.system(size: 10))
                .foregroundColor(.white.opacity(0.7))
        }
    }
}
#endif

// MARK: - Preview

#Preview {
    ContentView()
        .environmentObject(PharoBridge.shared)
        .environmentObject(ImageManager())
}
