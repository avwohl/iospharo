/*
 * PharoBridge.swift
 *
 * Swift wrapper for the Pharo VM C API.
 * Manages VM lifecycle and bridges events between SwiftUI and the VM.
 */

import Foundation
import Combine

/// Main bridge between Swift and the Pharo VM
@MainActor
class PharoBridge: ObservableObject {

    /// Singleton instance
    static let shared = PharoBridge()

    /// Published state
    @Published var isRunning = false
    @Published var isInitialized = false
    @Published var displayNeedsUpdate = false
    @Published var errorMessage: String?

    /// Display dimensions
    @Published var displayWidth: Int = 1024
    @Published var displayHeight: Int = 768

    private var imagePath: String?

    /// Display update callback (stored to prevent deallocation)
    private var displayCallback: IOSDisplayUpdateCallback?

    private init() {
        setupDisplayCallback()
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

        // Memory configuration (512MB default, can be adjusted)
        parameters.maxOldSpaceSize = 512 * 1024 * 1024
        parameters.edenSize = 10 * 1024 * 1024
        parameters.maxCodeSize = 0

        let initResult = vm_init(&parameters)

        if initResult != 0 {
            isInitialized = true
            isRunning = true
            NSLog("[BRIDGE] VM initialized, scheduling interpreter on main thread")

            // Schedule the interpreter to run on the NEXT run loop iteration.
            // The interpreter runs on the main thread and periodically pumps
            // CFRunLoopRunInMode to let AppKit/UIKit process events.
            CFRunLoopPerformBlock(CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue) {
                NSLog("[BRIDGE] Starting interpreter on main thread")
                vm_runOnMainThread()

                // VM exited
                DispatchQueue.main.async {
                    self.isRunning = false
                    self.isInitialized = false
                    NSLog("[BRIDGE] VM stopped, exiting app")
                    exit(0)
                }
            }
            // Wake up the run loop to process the block
            CFRunLoopWakeUp(CFRunLoopGetMain())
        } else {
            errorMessage = "Failed to initialize VM"
        }

        vm_parameters_destroy(&parameters)
    }

    // MARK: - Display Access

    /// Get pointer to display bits for Metal rendering
    func getDisplayBits() -> UnsafeMutableRawPointer? {
        return ios_getDisplayBits()
    }

    /// Get current display dimensions
    func getDisplaySize() -> (width: Int, height: Int) {
        return (Int(ios_getDisplayWidth()), Int(ios_getDisplayHeight()))
    }

    /// Get all display info atomically (prevents tearing during resize)
    func getDisplayBufferInfo() -> (pixels: UnsafeMutablePointer<UInt32>?, width: Int, height: Int, size: Int) {
        var info = IOSDisplayBufferInfo()
        ios_getDisplayBufferInfo(&info)
        return (info.pixels, Int(info.width), Int(info.height), Int(info.size))
    }

    /// Get display depth (bits per pixel)
    func getDisplayDepth() -> Int {
        return Int(ios_getDisplayDepth())
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

    /// Mark display as refreshed
    func displayDidUpdate() {
        displayNeedsUpdate = false
    }

    // MARK: - Touch Events
    // Mouse event types for vm_postMouseEvent: 0=move, 1=down, 2=up

    private static var bridgeLogFile: UnsafeMutablePointer<FILE>? = {
        fopen("/tmp/bridge_events.log", "w")
    }()

    private func logBridgeEvent(_ msg: String) {
        if let file = PharoBridge.bridgeLogFile {
            fputs("[BRIDGE] \(msg)\n", file)
            fflush(file)
        }
    }

    /// Send touch down event
    func sendTouchDown(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        logBridgeEvent("sendTouchDown BEFORE vm_postMouseEvent at (\(Int(point.x)),\(Int(point.y))) buttons=\(buttons)")
        vm_postMouseEvent(1, // type: down
                          Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
        logBridgeEvent("sendTouchDown AFTER vm_postMouseEvent")
    }

    /// Send touch moved event
    func sendTouchMoved(to point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        logBridgeEvent("sendTouchMoved at (\(Int(point.x)),\(Int(point.y))) buttons=\(buttons)")
        vm_postMouseEvent(0, // type: move
                          Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    /// Send touch up event
    func sendTouchUp(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        logBridgeEvent("sendTouchUp at (\(Int(point.x)),\(Int(point.y))) buttons=\(buttons)")
        vm_postMouseEvent(2, // type: up
                          Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    /// Send touch cancelled event
    func sendTouchCancelled(at point: CGPoint, buttons: Int = IOS_RED_BUTTON) {
        vm_postMouseEvent(2, // type: up (treat cancelled as up)
                          Int32(point.x), Int32(point.y),
                          Int32(buttons), 0)
    }

    /// Send mouse moved event (no buttons pressed) - Mac Catalyst hover
    func sendMouseMoved(to point: CGPoint, modifiers: Int = 0) {
        logBridgeEvent("sendMouseMoved at (\(Int(point.x)),\(Int(point.y)))")
        vm_postMouseEvent(0, // type: move
                          Int32(point.x), Int32(point.y),
                          0, Int32(modifiers))  // buttons = 0 for hover
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

    /// Stop the VM - MUST be called before app exit to prevent crash
    func stop() {
        guard isRunning else { return }
        NSLog("[BRIDGE] Stopping VM...")
        vm_stop()
        NSLog("[BRIDGE] VM stopped")
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

let IOS_TOUCH_DOWN = 0
let IOS_TOUCH_MOVED = 1
let IOS_TOUCH_UP = 2
let IOS_TOUCH_CANCELLED = 3

let IOS_KEY_DOWN = 0
let IOS_KEY_UP = 1
let IOS_KEY_CHAR = 2
