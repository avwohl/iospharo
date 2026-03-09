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
    /// Stable canvas size captured before keyboard appeared.
    /// Prevents keyboard-related SwiftUI relayouts from resizing the canvas.
    @State private var stableCanvasSize: CGSize = .zero

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

            // Priority 0: PHARO_AUTO_LAUNCH env var (simulator testing)
            // Set SIMCTL_CHILD_PHARO_AUTO_LAUNCH=1 before simctl launch
            if ProcessInfo.processInfo.environment["PHARO_AUTO_LAUNCH"] != nil,
               let image = imageManager.images.first {
                launchImage(image)
                return
            }

            // Priority 1: CLI --image flag (immediate launch, no splash)
            if let cliPath = Self.parseCommandLineImagePath() {
                #if DEBUG
                fputs("[CLI] --image resolved to: \(cliPath)\n", stderr)
                #endif
                // Copy image + companions into sandbox so C++ fopen() can access them
                let sandboxPath = Self.copyImageIntoSandbox(cliPath)
                #if DEBUG
                fputs("[CLI] sandbox path: \(sandboxPath)\n", stderr)
                #endif
                if bridge.loadImage(at: sandboxPath) {
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

    /// Copy image + companion files (.changes, .sources, startup.st) into
    /// the app's sandbox temp directory so C++ fopen() can access them.
    /// Mac Catalyst sandbox blocks POSIX file access to arbitrary paths,
    /// so we use NSData to read through the sandboxed API, then write
    /// to the app's temp directory which C++ can access.
    private static func copyImageIntoSandbox(_ sourcePath: String) -> String {
        let fm = FileManager.default
        let sourceDir = (sourcePath as NSString).deletingLastPathComponent
        let baseName = ((sourcePath as NSString).lastPathComponent as NSString).deletingPathExtension
        let tempDir = NSTemporaryDirectory() + "cli_image"

        // Clean and create temp directory
        try? fm.removeItem(atPath: tempDir)
        try? fm.createDirectory(atPath: tempDir, withIntermediateDirectories: true)

        // Helper: read via NSData (sandboxed) and write to temp dir
        func sandboxCopy(_ src: String, _ dst: String) {
            if let data = NSData(contentsOfFile: src) {
                data.write(toFile: dst, atomically: true)
                #if DEBUG
                fputs("[CLI] copied \((src as NSString).lastPathComponent) (\(data.length) bytes)\n", stderr)
                #endif
            }
        }

        // Copy image and companion files
        for ext in ["image", "changes"] {
            let src = (sourceDir as NSString).appendingPathComponent("\(baseName).\(ext)")
            let dst = (tempDir as NSString).appendingPathComponent("\(baseName).\(ext)")
            sandboxCopy(src, dst)
        }

        // Copy .sources file (may have different name)
        if let contents = try? fm.contentsOfDirectory(atPath: sourceDir) {
            for file in contents where file.hasSuffix(".sources") {
                sandboxCopy(
                    (sourceDir as NSString).appendingPathComponent(file),
                    (tempDir as NSString).appendingPathComponent(file))
            }
        }

        // Copy startup.st if present (user-provided test runner)
        let startupSrc = (sourceDir as NSString).appendingPathComponent("startup.st")
        if fm.fileExists(atPath: startupSrc) {
            sandboxCopy(startupSrc, (tempDir as NSString).appendingPathComponent("startup.st"))
        }

        return (tempDir as NSString).appendingPathComponent("\(baseName).image")
    }

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
            // GeometryReader captures the available size and locks the HStack to it
            // while a keyboard is visible. Without this, iPad floating keyboards cause
            // SwiftUI to re-evaluate safe areas during @Published-triggered relayouts,
            // expanding the view past the status bar and creating a gap at the bottom.
            GeometryReader { geo in
                let size = stableCanvasSize.height > 0 ? stableCanvasSize : geo.size
                HStack(spacing: 0) {
                    ModifierStrip(
                        bridge: bridge,
                        keyboardVisible: $bridge.keyboardVisible,
                        showHelp: $showingHelp
                    )
                    PharoCanvasView(bridge: bridge)
                }
                .frame(width: size.width, height: size.height)
                .onAppear { stableCanvasSize = geo.size }
                .onChange(of: geo.size) { newSize in
                    // Update stable size only when keyboard is NOT visible,
                    // OR when width changed (orientation / Stage Manager resize).
                    if !bridge.keyboardVisible || newSize.width != stableCanvasSize.width {
                        stableCanvasSize = newSize
                    }
                }
            }
            .ignoresSafeArea(edges: [.bottom, .horizontal])

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

    /// True when a docked keyboard is confirmed showing (from notification).
    /// Floating/undocked keyboards don't set this, so the full button set
    /// stays visible since they don't cover the bottom of the screen.
    private var keyboardDockedVisible: Bool {
        bridge.keyboardDocked
    }

    private var buttonSpacing: CGFloat { isIPad ? 3 : 4 }
    private var stripWidth: CGFloat { isIPad ? 38 : 40 }

    /// Full-size buttons (modifiers at top, iPad buttons)
    private var buttonSize: CGFloat { isIPad ? 28 : 32 }

    /// Action buttons: smaller on DI iPhones so the bottom group fits
    /// below the Dynamic Island without overlap.
    private var actionButtonSize: CGFloat { hasDynamicIsland ? 20 : 26 }

    // MARK: - Squircle Corner Math
    //
    // Apple's continuous corners are a superellipse with n=5.
    // See .claude/skills/device-geometry.md for the full algorithm.

    /// Squircle corner intrusion: minimum y to clear the corner at distance x.
    private func squircleIntrusion(R: CGFloat, x: CGFloat) -> CGFloat {
        guard x > 0, x < R else { return x <= 0 ? R : 0 }
        let r5 = pow(R, 5)
        let d5 = pow(R - x, 5)
        return R - pow(r5 - d5, 0.2)
    }

    /// Estimate the display corner radius from the safe area leading inset.
    /// Maps: SA 59→R55, SA 62→R62, SA 47-50→R47, SA 44→R39.
    private var estimatedCornerRadius: CGFloat {
        guard let window = mainWindow else { return 0 }
        let leading = max(window.safeAreaInsets.left, window.safeAreaInsets.right)
        if leading >= 60 { return 62 }    // iPhone 16 Pro / Pro Max
        if leading >= 55 { return 55 }    // iPhone 14 Pro, 15 series, 16
        if leading >= 48 { return 47.33 } // iPhone XR, 11
        if leading >= 47 { return 47.33 } // iPhone 12-14, 16e
        if leading >= 44 { return 39 }    // iPhone X, XS, 11 Pro
        return 0                          // iPhone SE
    }

    /// True if the device has a Dynamic Island (vs notch or home button).
    private var hasDynamicIsland: Bool {
        guard let window = mainWindow else { return false }
        return max(window.safeAreaInsets.left, window.safeAreaInsets.right) > 55
    }

    private var mainWindow: UIWindow? {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }.first?
            .windows.first
    }

    // MARK: - DI Zone Centering
    //
    // On Dynamic Island iPhones in landscape, the strip is divided into zones:
    //   Top zone:    squircle corner  →  DI top edge
    //   Bottom zone: DI bottom edge   →  bottom squircle corner − home indicator
    // Each button group is centered in its zone for balanced spacing.
    // The squircle is evaluated at x=2 (strip padding edge, not button edge)
    // so the clipped background doesn't crowd the buttons visually.

    /// Landscape height = portrait screen width (the shorter native dimension).
    private var landscapeHeight: CGFloat {
        UIScreen.main.nativeBounds.width / UIScreen.main.nativeScale
    }

    /// All DI iPhones have a 126.9pt-wide Dynamic Island (= vertical extent in landscape).
    private static let diLandscapeExtent: CGFloat = 126.9

    /// Top edge of the DI in landscape coordinates (DI is vertically centered).
    private var diTopY: CGFloat { landscapeHeight / 2 - Self.diLandscapeExtent / 2 }

    /// Bottom edge of the DI in landscape coordinates.
    private var diBottomY: CGFloat { landscapeHeight / 2 + Self.diLandscapeExtent / 2 }

    /// Height of the top button group (keyboard + ctrl + cmd).
    private var topGroupHeight: CGFloat { 3 * buttonSize + 2 * buttonSpacing }

    /// Height of the bottom action button group (4 small buttons).
    private var bottomGroupHeight: CGFloat { 4 * actionButtonSize + 3 * buttonSpacing }

    /// Top padding: center top group in the zone above the DI.
    private var iPhoneTopPadding: CGFloat {
        let R = estimatedCornerRadius
        guard R > 0 else { return 6 }
        let intrusion = squircleIntrusion(R: R, x: 2)
        guard hasDynamicIsland else {
            // Notch/older phones: intrusion + proportional margin
            return ceil(intrusion + R * 0.15)
        }
        // Center top group in [intrusion, diTopY]
        let zone = diTopY - intrusion
        let extra = max(0, zone - topGroupHeight)
        return ceil(intrusion + extra / 2)
    }

    /// Bottom padding: center bottom group in the zone below the DI.
    private var iPhoneBottomPadding: CGFloat {
        guard let window = mainWindow else { return 6 }
        let R = estimatedCornerRadius
        let home = window.safeAreaInsets.bottom
        guard R > 0 else { return max(home, 6) }
        let intrusion = squircleIntrusion(R: R, x: 2)
        guard !keyboardDockedVisible, hasDynamicIsland else {
            // No action buttons or not a DI phone: just clear corner + home
            return ceil(intrusion) + home
        }
        // Center bottom group in [diBottomY, landscapeHeight − intrusion − home]
        let zoneEnd = landscapeHeight - intrusion - home
        let zone = zoneEnd - diBottomY
        let extra = max(0, zone - bottomGroupHeight)
        return ceil(intrusion + home + extra / 2)
    }

    var body: some View {
        if isIPad {
            // iPad: 28pt gap clears the Pharo menu bar, then buttons.
            // Keyboard toggle at top so it's always accessible.
            // When keyboard is showing, keep essential coding buttons visible
            // but hide less-used ones (Tab, Esc, clipboard, etc.).
            VStack(spacing: 0) {
                Color.clear.frame(height: 28)
                VStack(spacing: buttonSpacing) {
                    keyboardButton
                    ctrlButton
                    cmdButton

                    if keyboardDockedVisible {
                        // Essential buttons for typing: backspace, actions, search
                        stripDivider
                        backspaceButton
                        doItButton
                        printButton
                        inspectButton
                        stripDivider
                        spotterButton
                        refactorButton
                    } else {
                        iPadExtraButtons
                    }

                    Spacer()

                    if !keyboardDockedVisible {
                        StripButton(icon: "questionmark", size: buttonSize,
                                    tooltip: "Help") { showHelp = true }
                    }
                }
                .padding(.vertical, 4)
                .padding(.horizontal, 2)
                .background(Color(.systemGray6).opacity(0.95))
            }
            .frame(width: stripWidth)
        } else {
            // iPhone: top buttons pushed below Dynamic Island,
            // action buttons use smaller size, hidden when keyboard is showing
            VStack(spacing: buttonSpacing) {
                keyboardButton
                ctrlButton
                cmdButton
                Spacer()
                if !keyboardDockedVisible {
                    iPhoneActionButton(icon: "delete.left", tooltip: "Backspace") {
                        bridge.sendRawKey(8, keyCode: 8)
                    }
                    iPhoneActionButton(icon: "play.fill", tooltip: "DoIt (Cmd+D)") {
                        bridge.sendKeyShortcut("d", modifiers: IOS_CMD_KEY)
                    }
                    iPhoneActionButton(icon: "text.append", tooltip: "PrintIt (Cmd+P)") {
                        bridge.sendKeyShortcut("p", modifiers: IOS_CMD_KEY)
                    }
                    iPhoneActionButton(icon: "eyeglasses", tooltip: "InspectIt (Cmd+I)") {
                        bridge.sendKeyShortcut("i", modifiers: IOS_CMD_KEY)
                    }
                }
            }
            .padding(.top, iPhoneTopPadding)
            .padding(.bottom, iPhoneBottomPadding)
            .padding(.horizontal, 2)
            .frame(width: stripWidth)
            .background(Color(.systemGray6).opacity(0.95))
        }
    }

    // MARK: - iPad Extra Buttons (shown when keyboard is hidden)

    private var iPadExtraButtons: some View {
        Group {
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

            // --- Search & refactoring ---
            spotterButton
            refactorButton

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

    private var spotterButton: some View {
        StripButton(icon: "magnifyingglass", size: buttonSize, tooltip: "Spotter (Shift+Enter)") {
            bridge.sendRawKey(13, keyCode: 13, modifiers: Int32(IOS_SHIFT_KEY))
        }
    }

    private var refactorButton: some View {
        StripButton(icon: "wrench", size: buttonSize, tooltip: "Refactor (Cmd+T)") {
            bridge.sendKeyShortcut("t", modifiers: IOS_CMD_KEY)
        }
    }

    private var keyboardButton: some View {
        StripButton(icon: "keyboard", isActive: keyboardVisible, size: buttonSize,
                    tooltip: "Show/hide keyboard") {
            if let view = gPharoMTKView {
                if view.isFirstResponder {
                    view.resignFirstResponder()
                    // Set immediately for responsive button state;
                    // notification will confirm.
                    bridge.keyboardVisible = false
                    bridge.keyboardDocked = false
                } else {
                    view.becomeFirstResponder()
                    // Set visible immediately for button highlight.
                    // Do NOT set keyboardDocked — let the notification
                    // confirm whether the keyboard is docked or floating.
                    // This prevents incorrectly hiding sidebar buttons
                    // when a floating keyboard appears.
                    bridge.keyboardVisible = true
                }
            }
        }
    }

    /// Smaller action button for the iPhone strip bottom section
    private func iPhoneActionButton(icon: String, tooltip: String,
                                     action: @escaping () -> Void) -> some View {
        StripButton(icon: icon, size: actionButtonSize, tooltip: tooltip, action: action)
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
