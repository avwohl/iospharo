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
    @AppStorage("autoLaunchImageID") private var autoLaunchImageID: String?
    @State private var showingHelp = false
    @State private var showingSplash = false
    @State private var splashImage: PharoImage?

    var body: some View {
        ZStack {
            if bridge.isRunning {
                // Pharo is running — full-screen canvas
                pharoCanvas
            } else if showingSplash, let image = splashImage {
                // Auto-launch countdown splash
                AutoLaunchSplashView(
                    imageName: image.name,
                    onLaunch: { launchImage(image) },
                    onCancel: {
                        showingSplash = false
                        splashImage = nil
                    }
                )
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
            guard !bridge.isRunning else { return }

            // Priority 1: CLI --image flag (immediate launch, no splash)
            if let cliPath = Self.parseCommandLineImagePath() {
                if bridge.loadImage(at: cliPath) {
                    bridge.start()
                }
                return
            }

            // Priority 2: User-selected auto-launch image (show splash)
            if let idString = autoLaunchImageID,
               let uuid = UUID(uuidString: idString),
               let image = imageManager.images.first(where: { $0.id == uuid }) {
                splashImage = image
                showingSplash = true
            }
        }
    }

    // MARK: - Launch Helper

    private func launchImage(_ image: PharoImage) {
        showingSplash = false
        splashImage = nil
        imageManager.markLaunched(image)
        imageManager.selectedImageID = image.id
        if bridge.loadImage(at: image.imagePath) {
            bridge.start()
        }
    }

    // MARK: - CLI Argument Parsing

    /// Check for `--image /path/to/Pharo.image` in process arguments.
    /// Usage: `open /path/to/iospharo.app --args --image /tmp/Pharo.image`
    private static func parseCommandLineImagePath() -> String? {
        let args = ProcessInfo.processInfo.arguments
        guard let idx = args.firstIndex(of: "--image"),
              idx + 1 < args.count else { return nil }
        let path = args[idx + 1]
        return FileManager.default.fileExists(atPath: path) ? path : nil
    }

    // MARK: - Views

    private var pharoCanvas: some View {
        ZStack {
            #if targetEnvironment(macCatalyst)
            PharoCanvasView(bridge: bridge)
                .ignoresSafeArea()
            #else
            if bridge.isIPad {
                // iPad: HStack respects top safe area (status bar), strip
                // naturally positioned below it. Canvas extends to full screen.
                HStack(spacing: 0) {
                    ModifierStrip(
                        bridge: bridge,
                        keyboardVisible: $bridge.keyboardVisible,
                        showHelp: $showingHelp
                    )
                    PharoCanvasView(bridge: bridge)
                        .ignoresSafeArea()
                }
                .ignoresSafeArea(.container, edges: [.bottom, .leading, .trailing])
            } else {
                // iPhone: full-screen layout, strip handles camera positioning
                HStack(spacing: 0) {
                    ModifierStrip(
                        bridge: bridge,
                        keyboardVisible: $bridge.keyboardVisible,
                        showHelp: $showingHelp
                    )
                    PharoCanvasView(bridge: bridge)
                }
                .ignoresSafeArea()
            }

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

// MARK: - Left-Side Modifier Strip (iOS only)

#if !targetEnvironment(macCatalyst)
struct ModifierStrip: View {
    @ObservedObject var bridge: PharoBridge
    @Binding var keyboardVisible: Bool
    @Binding var showHelp: Bool

    private var isIPad: Bool { bridge.isIPad }

    private var buttonSize: CGFloat { isIPad ? 34 : 32 }
    private var buttonSpacing: CGFloat { isIPad ? 5 : 4 }
    private var stripWidth: CGFloat { isIPad ? 44 : 40 }

    var body: some View {
        if isIPad {
            // iPad: dark gap above (matches canvas behind Pharo menu bar),
            // gray background only on the button area below.
            VStack(spacing: 0) {
                Color.black
                    .frame(width: stripWidth, height: 28)
                VStack(spacing: buttonSpacing) {
                    iPadStrip
                }
                .padding(.vertical, 6)
                .padding(.horizontal, 2)
                .frame(width: stripWidth)
                .background(Color(.systemGray6).opacity(0.95))
            }
        } else {
            // iPhone: full-height gray strip, buttons split around camera
            VStack(spacing: buttonSpacing) {
                keyboardButton
                ctrlButton
                cmdButton
                Spacer()
                backspaceButton
                doItButton
                printButton
                inspectButton
            }
            .padding(.vertical, 6)
            .padding(.horizontal, 2)
            .frame(width: stripWidth)
            .background(Color(.systemGray6).opacity(0.95))
        }
    }

    // MARK: - iPad Layout (16 buttons)

    private var iPadStrip: some View {
        Group {
            ctrlButton
            cmdButton

            // --- Direct keys ---
            StripButton(label: "Tab", size: buttonSize, tooltip: "Tab") {
                bridge.sendKeyShortcut("\t", modifiers: 0)
            }
            StripButton(label: "Esc", size: buttonSize, tooltip: "Escape") {
                bridge.sendRawKey(27)
            }
            backspaceButton

            stripDivider

            // --- Pharo action shortcuts ---
            doItButton
            printButton
            inspectButton
            StripButton(icon: "ant.fill", size: buttonSize, tooltip: "Debug (Cmd+Shift+D)") {
                bridge.sendKeyShortcut("d", modifiers: IOS_CMD_KEY | IOS_SHIFT_KEY)
            }

            stripDivider

            // --- Clipboard & editing ---
            StripButton(icon: "scissors", size: buttonSize, tooltip: "Cut") {
                bridge.sendKeyShortcut("x", modifiers: IOS_CMD_KEY)
            }
            StripButton(icon: "doc.on.doc", size: buttonSize, tooltip: "Copy") {
                bridge.sendKeyShortcut("c", modifiers: IOS_CMD_KEY)
            }
            StripButton(icon: "doc.on.clipboard", size: buttonSize, tooltip: "Paste") {
                bridge.sendKeyShortcut("v", modifiers: IOS_CMD_KEY)
            }
            StripButton(icon: "arrow.up.left.and.arrow.down.right", size: buttonSize, tooltip: "Expand (Cmd+2)") {
                bridge.sendKeyShortcut("2", modifiers: IOS_CMD_KEY)
            }
            StripButton(icon: "checkmark.circle", size: buttonSize, tooltip: "Accept (Cmd+S)") {
                bridge.sendKeyShortcut("s", modifiers: IOS_CMD_KEY)
            }
            StripButton(icon: "xmark.circle", size: buttonSize, tooltip: "Cancel (Cmd+L)") {
                bridge.sendKeyShortcut("l", modifiers: IOS_CMD_KEY)
            }

            Spacer()

            // --- Utility ---
            keyboardButton
            StripButton(icon: "questionmark", size: buttonSize, tooltip: "Help") {
                showHelp = true
            }
        }
    }

    // MARK: - Shared Buttons

    private var ctrlButton: some View {
        StripButton(icon: "control",
                    isActive: bridge.ctrlModifierActive, size: buttonSize,
                    tooltip: "Toggle Ctrl modifier") {
            bridge.ctrlModifierActive.toggle()
        }
    }

    private var cmdButton: some View {
        StripButton(icon: "command",
                    isActive: bridge.cmdModifierActive, size: buttonSize,
                    tooltip: "Toggle Cmd modifier") {
            bridge.cmdModifierActive.toggle()
        }
    }

    private var backspaceButton: some View {
        StripButton(icon: "delete.left", size: buttonSize, tooltip: "Backspace") {
            bridge.sendRawKey(8, keyCode: 8)
        }
    }

    private var doItButton: some View {
        StripButton(icon: "play.fill", size: buttonSize, tooltip: "DoIt (Cmd+D)") {
            bridge.sendKeyShortcut("d", modifiers: IOS_CMD_KEY)
        }
    }

    private var printButton: some View {
        StripButton(icon: "text.append", size: buttonSize, tooltip: "PrintIt (Cmd+P)") {
            bridge.sendKeyShortcut("p", modifiers: IOS_CMD_KEY)
        }
    }

    private var inspectButton: some View {
        StripButton(icon: "eyeglasses", size: buttonSize, tooltip: "InspectIt (Cmd+I)") {
            bridge.sendKeyShortcut("i", modifiers: IOS_CMD_KEY)
        }
    }

    private var keyboardButton: some View {
        StripButton(icon: "keyboard", isActive: keyboardVisible, size: buttonSize,
                    tooltip: "Show/hide keyboard") {
            keyboardVisible.toggle()
            if let view = gPharoMTKView {
                if keyboardVisible {
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
            }
        }
    }

    private var stripDivider: some View {
        Divider()
            .frame(width: buttonSize - 6)
            .background(Color.gray.opacity(0.5))
    }
}

struct StripButton: View {
    let label: String?
    let icon: String?
    let isActive: Bool
    let size: CGFloat
    let tooltip: String?
    let action: () -> Void

    init(label: String? = nil, icon: String? = nil, isActive: Bool = false,
         size: CGFloat = 34, tooltip: String? = nil, action: @escaping () -> Void) {
        self.label = label
        self.icon = icon
        self.isActive = isActive
        self.size = size
        self.tooltip = tooltip
        self.action = action
    }

    var body: some View {
        Button(action: action) {
            Group {
                if let icon = icon {
                    Image(systemName: icon)
                        .font(.system(size: size * 0.4))
                } else if let label = label {
                    Text(label)
                        .font(.system(size: size * 0.29, weight: .semibold, design: .rounded))
                }
            }
            .foregroundColor(isActive ? .white : .primary)
            .frame(width: size, height: size)
            .background(isActive ? Color.blue : Color.gray.opacity(0.2))
            .cornerRadius(size * 0.22)
        }
        .help(tooltip ?? "")
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

                    helpRow("sidebar.left", "Left strip", "Modifier keys, actions, clipboard")
                    helpRow("keyboard", "Keyboard button", "Show/hide the soft keyboard")

                    Divider().background(Color.white.opacity(0.3))

                    VStack(alignment: .leading, spacing: 6) {
                        Text("Strip actions:")
                            .font(.caption)
                            .foregroundColor(.white.opacity(0.7))
                        HStack(spacing: 12) {
                            shortcutLabel("DoIt", "Cmd+D")
                            shortcutLabel("PrintIt", "Cmd+P")
                            shortcutLabel("InspectIt", "Cmd+I")
                        }
                        HStack(spacing: 12) {
                            shortcutLabel("Debug", "Cmd+Shift+D")
                            shortcutLabel("Accept", "Cmd+S")
                            shortcutLabel("Cancel", "Cmd+L")
                        }
                        Text("Also: Cut, Copy, Paste, Expand selection")
                            .font(.system(size: 10))
                            .foregroundColor(.white.opacity(0.6))
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
