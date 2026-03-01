/*
 * PharoBridge.swift
 *
 * Swift wrapper for the Pharo VM C API.
 * Manages VM lifecycle and bridges events between SwiftUI and the VM.
 */

import Foundation
import Combine
import UIKit

/// Buffer for clipboard text returned to C (freed on next call)
private var gClipboardBuffer: UnsafeMutablePointer<CChar>?

/// Main bridge between Swift and the Pharo VM
@MainActor
class PharoBridge: ObservableObject {

    /// Singleton instance
    static let shared = PharoBridge()

    /// Published state
    @Published var isRunning = false
    @Published var isInitialized = false
    @Published var errorMessage: String?

    /// Virtual Ctrl key toggle — when active, touches and keyboard events include Ctrl modifier
    @Published var ctrlModifierActive = false

    /// Virtual Cmd key toggle — when active, keyboard events include Cmd modifier (one-shot)
    @Published var cmdModifierActive = false

    /// Soft keyboard visibility toggle (iOS only)
    @Published var keyboardVisible = false

    private var imagePath: String?

    private var displayCallback: IOSDisplayUpdateCallback?

    private init() {
        setupDisplayCallback()
        setupClipboardCallbacks()
        setupTextInputCallback()
    }

    // MARK: - Display Callback

    private func setupDisplayCallback() {
        // Register a no-op callback. The Metal renderer polls the front buffer
        // directly on every draw(in:) call, so no notification is needed.
        let callback: IOSDisplayUpdateCallback = { x, y, width, height in
            // No-op: MetalRenderer copies the front buffer every frame.
        }

        self.displayCallback = callback
        ios_registerDisplayUpdateCallback(callback)
    }

    // MARK: - Clipboard Callbacks

    private func setupClipboardCallbacks() {
        vm_setClipboardCallbacks(
            // ClipboardGetFunc: return pasteboard text as C string
            {
                free(gClipboardBuffer)
                gClipboardBuffer = nil

                // Read clipboard — always on main thread since UIPasteboard requires it.
                // Use DispatchQueue.main.async + semaphore instead of .sync to avoid
                // deadlocking if the main thread is waiting on the VM.
                var text: String?
                if Thread.isMainThread {
                    text = UIPasteboard.general.string
                } else {
                    let sema = DispatchSemaphore(value: 0)
                    DispatchQueue.main.async {
                        text = UIPasteboard.general.string
                        sema.signal()
                    }
                    sema.wait()
                }

                guard let str = text, !str.isEmpty else { return nil }
                gClipboardBuffer = strdup(str)
                return UnsafePointer(gClipboardBuffer!)
            },
            // ClipboardSetFunc: set pasteboard text from C string
            { cString in
                guard let cString = cString else { return }
                let string = String(cString: cString)
                if Thread.isMainThread {
                    UIPasteboard.general.string = string
                } else {
                    DispatchQueue.main.async {
                        UIPasteboard.general.string = string
                    }
                }
            }
        )
    }

    // MARK: - Text Input Callbacks

    private func setupTextInputCallback() {
        vm_setTextInputCallback { active in
            DispatchQueue.main.async {
                #if targetEnvironment(macCatalyst)
                // Mac Catalyst: becomeFirstResponder captures hardware keyboard
                // without showing an on-screen keyboard, so always honor Pharo's request.
                guard let view = gPharoMTKView else { return }
                if active {
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
                #else
                // iOS: Don't let Pharo's SDL_StartTextInput show the soft keyboard.
                // The user controls the keyboard via the floating toolbar button.
                // Pharo calls SDL_StartTextInput aggressively (e.g. when refocusing
                // any morph), which would pop the keyboard at unwanted times.
                #endif
            }
        }
    }

    // MARK: - VM Lifecycle

    /// Load a Pharo image file
    func loadImage(at path: String) -> Bool {
        guard FileManager.default.fileExists(atPath: path) else {
            errorMessage = "Image file not found: \(path)"
            return false
        }

        self.imagePath = path
        return true
    }

    /// Start the VM with the loaded image
    func start() {
        guard let imagePath = imagePath else {
            errorMessage = "No image loaded"
            return
        }

        guard !isRunning else {
            errorMessage = "VM is already running"
            return
        }

        // Don't set isRunning yet - wait until VM is initialized
        // This prevents SwiftUI from showing canvas before VM is ready
        errorMessage = nil

        // Initialize VM synchronously on main thread
        var parameters = VMParameters()
        vm_parameters_init(&parameters)

        parameters.imageFileName = strdup(imagePath)
        parameters.isInteractiveSession = true
        parameters.isWorker = false

        // Memory: 2GB virtual (mmap lazy commit, no physical RAM until touched)
        parameters.maxOldSpaceSize = 2 * 1024 * 1024 * 1024
        parameters.edenSize = 10 * 1024 * 1024
        parameters.maxCodeSize = 0

        // Don't pre-set display size from UIScreen.main.bounds — on Mac Catalyst
        // it returns the full screen size, not the window size. The default 1024x768
        // from vm_init() is used initially. drawableSizeWillChange fires once the
        // Metal view is laid out and provides the actual view dimensions, triggering
        // a single clean resize + redraw.

        // Change working directory to image's directory so Pharo's
        // StartupPreferencesLoader finds startup.st alongside the image
        let imageDir = (imagePath as NSString).deletingLastPathComponent
        #if DEBUG
        fputs("[BRIDGE] imagePath=\(imagePath) imageDir=\(imageDir)\n", stderr)
        #endif
        FileManager.default.changeCurrentDirectoryPath(imageDir)

        // Write startup.st with image patches (loaded by StartupPreferencesLoader)
        Self.writeStartupScript(to: imageDir)
        #if DEBUG
        fputs("[BRIDGE] after writeStartupScript, startup.st exists=\(FileManager.default.fileExists(atPath: imageDir + "/startup.st"))\n", stderr)
        #endif

        let initResult = vm_init(&parameters)

        if initResult != 0 {
            isInitialized = true
            isRunning = true
            #if DEBUG
            NSLog("[BRIDGE] VM initialized, starting interpreter on background thread")
            #endif

            // Start the interpreter on a background thread.
            // This returns immediately, leaving the main thread free for
            // SwiftUI/UIKit event processing and Metal rendering.
            vm_run()

            // Monitor for VM exit (primitiveQuit sets running_ = false)
            DispatchQueue.global(qos: .utility).async { [weak self] in
                while vm_isRunning() {
                    Thread.sleep(forTimeInterval: 0.1)
                }
                // VM has exited — clean up on main thread
                DispatchQueue.main.async {
                    self?.handleVMExit()
                }
            }
        } else {
            errorMessage = "Failed to initialize VM"
        }

        vm_parameters_destroy(&parameters)
    }

    // MARK: - Image Patches

    /// Write startup.st with Pharo image patches.
    /// Pharo's StartupPreferencesLoader auto-loads startup.st from the
    /// working directory on every image startup.
    private static func writeStartupScript(to directory: String) {
        let script = """
        "Pharo 13 image patches - auto-applied by iospharo VM"
        "Fix: doc browser uses anonymous GitHub API to avoid IceTokenCredentials crash"
        (Smalltalk hasClassNamed: #MicGitHubRessourceReference) ifTrue: [
          MicGitHubRessourceReference compile: 'githubApi
            ^ MicGitHubAPI new beAnonymous'].
        "Fix: doc browser error handler uses messageText instead of message"
        (Smalltalk hasClassNamed: #MicDocumentBrowserModel) ifTrue: [
          MicDocumentBrowserModel compile: 'document
            resourceReference ifNil: [ ^ nil ].
            document ifNotNil: [ ^ document ].
            [ document := resourceReference loadMicrodown ]
              on: Error
              do: [ :error |
                document := Microdown parse: ''# Error
        '', error messageText ].
            ^ document'].
        "Robust childrenOf: that handles API errors"
        (Smalltalk hasClassNamed: #MicDocumentBrowserPresenter) ifTrue: [
          MicDocumentBrowserPresenter compile: 'childrenOf: aNode
            [ (aNode isKindOf: MicElement) ifTrue: [ ^ aNode subsections children ].
              aNode loadChildren
                ifNotEmpty: [ :children | ^ children sort: [:a :b |
                    (self displayStringOf: a) < (self displayStringOf: b)] ]
                ifEmpty: [
                  [ ^ self childrenOf: (MicSectionBlock fromRoot: aNode loadMicrodown) ]
                    on: Error do: [ ^ #() ]]
            ] on: Error do: [ ^ #() ]'].
        "Fix: menu shortcut symbols (U+2318 etc.) missing from embedded Source Sans Pro"
        "The embedded font is from 2012; Adobe added these glyphs in v2.040 (2018)."
        "Replace Unicode symbols with readable ASCII abbreviations."
        (Smalltalk hasClassNamed: #KMShortcutPrinter) ifTrue: [
          KMShortcutPrinter symbolTable
            at: #Cmd put: 'Cmd+';
            at: #Meta put: 'Cmd+';
            at: #Alt put: 'Opt+';
            at: #Ctrl put: 'Ctrl+';
            at: #Shift put: 'Shift+';
            at: #Enter put: 'Enter'].
        "Refresh stale doc browser windows from previous saved sessions"
        "The tree may be empty if the image was saved when SSL was broken."
        (Smalltalk hasClassNamed: #MicDocumentBrowserPresenter) ifTrue: [
          [
            (Delay forMilliseconds: 3000) wait.
            MicDocumentBrowserPresenter allInstances do: [:each |
              [ each updateTree ] on: Error do: [ "ignore" ] ].
          ] fork ].
        """
        let path = (directory as NSString).appendingPathComponent("startup.st")
        #if DEBUG
        NSLog("[BRIDGE] writeStartupScript: directory=%@ path=%@", directory, path)
        #endif
        do {
            try script.write(toFile: path, atomically: true, encoding: .utf8)
            #if DEBUG
            NSLog("[BRIDGE] startup.st written successfully (%d bytes)", script.count)
            #endif
        } catch {
            NSLog("[BRIDGE] ERROR writing startup.st: %@", error.localizedDescription)
        }
    }

    // MARK: - Display Access

    /// Get all display info atomically (prevents tearing during resize)
    func getDisplayBufferInfo() -> (pixels: UnsafeMutablePointer<UInt32>?, width: Int, height: Int, size: Int) {
        var info = IOSDisplayBufferInfo()
        ios_getDisplayBufferInfo(&info)
        return (info.pixels, Int(info.width), Int(info.height), Int(info.size))
    }

    /// Notify VM of view size change
    func setDisplaySize(width: Int, height: Int) {
        ios_setDisplaySize(Int32(width), Int32(height))
    }

    // MARK: - Touch Events
    // Mouse event types for vm_postMouseEvent: 0=move, 1=down, 2=up

    func sendTouchDown(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        vm_postMouseEvent(1, Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    func sendTouchMoved(to point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        vm_postMouseEvent(0, Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    func sendTouchUp(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        vm_postMouseEvent(2, Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    func sendMouseMoved(to point: CGPoint, modifiers: Int = 0) {
        vm_postMouseEvent(0, Int32(point.x), Int32(point.y),
                          0, Int32(modifiers))
    }

    // MARK: - Keyboard Events
    // Key event types for vm_postKeyEvent: 0=down, 1=up, 2=stroke

    /// Send key down event
    func sendKeyDown(_ character: Character, modifiers: Int = 0) {
        guard let scalar = character.unicodeScalars.first else { return }
        vm_postKeyEvent(0, // type: down
                        Int32(scalar.value), 0, Int32(modifiers))
    }

    /// Send key up event
    func sendKeyUp(_ character: Character, modifiers: Int = 0) {
        guard let scalar = character.unicodeScalars.first else { return }
        vm_postKeyEvent(1, // type: up
                        Int32(scalar.value), 0, Int32(modifiers))
    }

    /// Send key typed (down + up)
    func sendKeyTyped(_ character: Character, modifiers: Int = 0) {
        sendKeyDown(character, modifiers: modifiers)
        sendKeyUp(character, modifiers: modifiers)
    }

    /// Send a full key shortcut sequence (down + stroke + up) for toolbar action buttons.
    /// Unlike sendKeyTyped which only sends down + up, this includes the stroke event
    /// that Pharo needs to process the character as text input with modifiers.
    func sendKeyShortcut(_ character: Character, modifiers: Int) {
        guard let scalar = character.unicodeScalars.first else { return }
        let code = Int32(scalar.value)
        let mods = Int32(modifiers)
        vm_postKeyEvent(0, code, 0, mods)  // down
        vm_postKeyEvent(2, code, 0, mods)  // stroke
        vm_postKeyEvent(1, code, 0, mods)  // up
    }

    /// Send a raw key down + up by integer key code, incorporating active modifier toggles.
    /// Clears modifier toggles after use (same behavior as PharoCanvasView keyboard input).
    func sendRawKey(_ charCode: Int32, keyCode: Int32 = 0, modifiers: Int32 = 0) {
        var mods = modifiers
        if ctrlModifierActive { mods |= Int32(IOS_CTRL_KEY) }
        if cmdModifierActive { mods |= Int32(IOS_CMD_KEY) }
        if mods != modifiers {
            DispatchQueue.main.async { [weak self] in
                self?.ctrlModifierActive = false
                self?.cmdModifierActive = false
            }
        }
        vm_postKeyEvent(0, charCode, keyCode, mods)  // down
        vm_postKeyEvent(1, charCode, keyCode, mods)  // up
    }

    /// Send string as key events
    func sendString(_ string: String, modifiers: Int = 0) {
        for char in string {
            sendKeyTyped(char, modifiers: modifiers)
        }
    }

    // MARK: - Scroll Events

    /// Send scroll wheel event (for pinch zoom, two-finger scroll)
    func sendScrollEvent(at point: CGPoint, deltaX: Int, deltaY: Int, modifiers: Int = 0) {
        vm_postScrollEvent(Int32(point.x), Int32(point.y),
                           Int32(deltaX), Int32(deltaY),
                           Int32(modifiers))
    }

    // MARK: - Utilities

    /// Check if running on iPad (or Mac Catalyst, which uses iPad idiom)
    var isIPad: Bool {
        return UIDevice.current.userInterfaceIdiom == .pad
    }

    /// Get device model string
    var deviceModel: String {
        return String(cString: iosGetDeviceModel())
    }

    /// Get iOS version string
    var systemVersion: String {
        return String(cString: iosGetSystemVersion())
    }

    /// Open URL in Safari
    func openURL(_ urlString: String) -> Bool {
        return iosOpenURL(urlString) != 0
    }

    /// Show native alert
    func showAlert(title: String, message: String) {
        iosShowAlert(title, message)
    }

    /// Trigger haptic feedback
    func hapticFeedback(style: HapticStyle = .medium) {
        iosHapticFeedback(Int32(style.rawValue))
    }

    enum HapticStyle: Int {
        case light = 0
        case medium = 1
        case heavy = 2
    }

    // MARK: - Shutdown

    /// Called when the interpreter exits naturally (e.g. primitiveQuit)
    private func handleVMExit() {
        guard isRunning else { return }
        #if DEBUG
        NSLog("[BRIDGE] VM exited, cleaning up")
        #endif
        vm_stop()  // Join threads, stop heartbeat (idempotent)
        isRunning = false
        isInitialized = false

        // Exit the app after a brief delay to let SwiftUI settle.
        // The VM cannot be re-launched without restarting the process
        // (global memory/thread state is not fully resettable yet).
        // This matches the Cmd+Q behavior on Mac Catalyst.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            exit(0)
        }
    }

    /// Stop the VM - MUST be called before app exit to prevent crash
    func stop() {
        guard isRunning else { return }
        #if DEBUG
        NSLog("[BRIDGE] Stopping VM...")
        #endif
        vm_stop()
        #if DEBUG
        NSLog("[BRIDGE] VM stopped")
        #endif
        isRunning = false
        isInitialized = false
    }
}

// MARK: - Button/Modifier Constants

let IOS_RED_BUTTON = 4
let IOS_YELLOW_BUTTON = 2
let IOS_BLUE_BUTTON = 1

let IOS_SHIFT_KEY = 1
let IOS_CTRL_KEY = 2
let IOS_ALT_KEY = 4
let IOS_CMD_KEY = 8

