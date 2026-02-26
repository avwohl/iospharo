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

    /// Display dimensions
    @Published var displayWidth: Int = 1024
    @Published var displayHeight: Int = 768

    /// Middle-click mode toggle (iOS floating button)
    @Published var middleClickActive = false

    private var imagePath: String?

    private var displayCallback: IOSDisplayUpdateCallback?

    private init() {
        setupDisplayCallback()
        setupClipboardCallbacks()
        setupTextInputCallback()
    }

    // MARK: - Display Callback

    private func setupDisplayCallback() {
        // Register a lightweight callback that just sets a flag.
        // The Metal renderer polls the front buffer directly on every
        // draw(in:) call, so the callback is mainly for future use.
        let callback: IOSDisplayUpdateCallback = { x, y, width, height in
            // No-op: MetalRenderer always copies the front buffer
            // on every draw(in:) call, bypassing the callback chain.
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
                guard let view = gPharoMTKView else { return }
                if active {
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
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
        // Defer property updates to avoid "Publishing changes from within view updates" warning
        Task { @MainActor in
            self.displayWidth = width
            self.displayHeight = height
        }
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

    func sendTouchCancelled(at point: CGPoint, buttons: Int = IOS_RED_BUTTON) {
        vm_postMouseEvent(2, Int32(point.x), Int32(point.y),
                          Int32(buttons), 0)
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

    /// Check if running on iPad
    var isIPad: Bool {
        return iosIsIPad() != 0
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

