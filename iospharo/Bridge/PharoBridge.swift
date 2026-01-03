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

    /// VM thread
    private var vmThread: Thread?
    private var imagePath: String?

    /// Display update callback (stored to prevent deallocation)
    private var displayCallback: IOSDisplayUpdateCallback?

    private init() {
        setupDisplayCallback()
    }

    // MARK: - Display Callback

    private func setupDisplayCallback() {
        // Create a C callback that posts to main thread
        let callback: IOSDisplayUpdateCallback = { x, y, width, height in
            DispatchQueue.main.async {
                PharoBridge.shared.handleDisplayUpdate(x: Int(x), y: Int(y),
                                                        width: Int(width), height: Int(height))
            }
        }

        self.displayCallback = callback
        ios_registerDisplayUpdateCallback(callback)
    }

    private func handleDisplayUpdate(x: Int, y: Int, width: Int, height: Int) {
        // Update dimensions if changed
        let newWidth = Int(ios_getDisplayWidth())
        let newHeight = Int(ios_getDisplayHeight())

        if newWidth != displayWidth || newHeight != displayHeight {
            displayWidth = newWidth
            displayHeight = newHeight
        }

        // Signal that display needs refresh
        displayNeedsUpdate = true
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

        isRunning = true
        errorMessage = nil

        // Run VM on a dedicated thread
        vmThread = Thread { [weak self] in
            self?.runVM(imagePath: imagePath)
        }
        vmThread?.name = "PharoVM"
        vmThread?.qualityOfService = .userInteractive
        vmThread?.start()
    }

    private nonisolated func runVM(imagePath: String) {
        var parameters = VMParameters()
        vm_parameters_init(&parameters)

        // Set image path
        parameters.imageFileName = strdup(imagePath)
        parameters.isInteractiveSession = true
        parameters.isWorker = false

        // Memory configuration (512MB default, can be adjusted)
        parameters.maxOldSpaceSize = 512 * 1024 * 1024
        parameters.edenSize = 10 * 1024 * 1024
        // No code zone for iOS - interpreter only (no JIT)
        parameters.maxCodeSize = 0

        // Initialize VM
        let initResult = vm_init(&parameters)

        if initResult != 0 {
            DispatchQueue.main.async {
                self.isInitialized = true
            }

            // Run interpreter (blocking)
            vm_run_interpreter()
        } else {
            DispatchQueue.main.async {
                self.errorMessage = "Failed to initialize VM"
            }
        }

        // Cleanup
        vm_parameters_destroy(&parameters)

        DispatchQueue.main.async {
            self.isRunning = false
            self.isInitialized = false
        }
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

    /// Get display depth (bits per pixel)
    func getDisplayDepth() -> Int {
        return Int(ios_getDisplayDepth())
    }

    /// Notify VM of view size change
    func setDisplaySize(width: Int, height: Int) {
        ios_setDisplaySize(Int32(width), Int32(height))
        displayWidth = width
        displayHeight = height
    }

    /// Mark display as refreshed
    func displayDidUpdate() {
        displayNeedsUpdate = false
    }

    // MARK: - Touch Events

    /// Send touch down event
    func sendTouchDown(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        ios_queueTouchEvent(Int32(IOS_TOUCH_DOWN),
                           Int32(point.x), Int32(point.y),
                           Int32(buttons), Int32(modifiers))
    }

    /// Send touch moved event
    func sendTouchMoved(to point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        ios_queueTouchEvent(Int32(IOS_TOUCH_MOVED),
                           Int32(point.x), Int32(point.y),
                           Int32(buttons), Int32(modifiers))
    }

    /// Send touch up event
    func sendTouchUp(at point: CGPoint, modifiers: Int = 0) {
        ios_queueTouchEvent(Int32(IOS_TOUCH_UP),
                           Int32(point.x), Int32(point.y),
                           0, Int32(modifiers))
    }

    /// Send touch cancelled event
    func sendTouchCancelled(at point: CGPoint) {
        ios_queueTouchEvent(Int32(IOS_TOUCH_CANCELLED),
                           Int32(point.x), Int32(point.y),
                           0, 0)
    }

    // MARK: - Keyboard Events

    /// Send key down event
    func sendKeyDown(_ character: Character, modifiers: Int = 0) {
        guard let scalar = character.unicodeScalars.first else { return }
        ios_queueKeyEvent(Int32(scalar.value), Int32(IOS_KEY_DOWN), Int32(modifiers))
    }

    /// Send key up event
    func sendKeyUp(_ character: Character, modifiers: Int = 0) {
        guard let scalar = character.unicodeScalars.first else { return }
        ios_queueKeyEvent(Int32(scalar.value), Int32(IOS_KEY_UP), Int32(modifiers))
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
