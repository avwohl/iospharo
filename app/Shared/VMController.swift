import Foundation
import Combine
import os.log

private let logger = Logger(subsystem: "com.awohl.pharo", category: "VMController")

class VMController: ObservableObject {
    @Published var isRunning = false
    @Published var isLoaded = false
    @Published var statusMessage = "Ready"

    private var displayPixels: UnsafeMutablePointer<UInt32>?
    private var displayWidth: Int = 1024
    private var displayHeight: Int = 768

    func initialize() {
        statusMessage = "Initializing VM..."
        logger.info("[VMController] initialize() called")

        let heapSize = 2 * 1024 * 1024 * 1024  // 2 GB - needed until scavenge/GC is fixed
        logger.info("[VMController] Calling vm_initialize with heapSize=\(heapSize)")
        guard vm_initialize(heapSize) else {
            statusMessage = "Failed to initialize VM"
            logger.info("[VMController] vm_initialize FAILED")
            return
        }
        logger.info("[VMController] vm_initialize SUCCESS")

        // Set up display
        vm_setDisplaySize(Int32(displayWidth), Int32(displayHeight), 32)
        displayPixels = vm_getDisplayPixels()
        logger.info("[VMController] Display set up")

        statusMessage = "VM initialized"
    }

    func loadImage(path: String) {
        statusMessage = "Loading image..."
        logger.info("[VMController] loadImage() called with path=\(path)")

        guard vm_loadImage(path) else {
            statusMessage = "Failed to load image"
            logger.info("[VMController] vm_loadImage FAILED")
            return
        }

        isLoaded = true
        statusMessage = "Image loaded"
        logger.info("[VMController] vm_loadImage SUCCESS, isLoaded=true")
    }

    func start() {
        logger.info("[VMController] start() called")
        guard isLoaded && !isRunning else { return }

        statusMessage = "Running..."
        logger.info("[VMController] Calling vm_run()")
        vm_run()
        isRunning = true
        logger.info("[VMController] vm_run() returned, isRunning=true")
    }

    func stop() {
        vm_stop()
        isRunning = false
        statusMessage = "Stopped"
    }

    func getDisplayPixels() -> UnsafeMutablePointer<UInt32>? {
        return vm_getDisplayPixels()
    }

    func getDisplaySize() -> (width: Int, height: Int) {
        return (Int(vm_getDisplayWidth()), Int(vm_getDisplayHeight()))
    }

    func postMouseEvent(type: Int, x: Int, y: Int, buttons: Int, modifiers: Int) {
        vm_postMouseEvent(Int32(type), Int32(x), Int32(y), Int32(buttons), Int32(modifiers))
    }

    func postKeyEvent(type: Int, charCode: Int, keyCode: Int, modifiers: Int) {
        vm_postKeyEvent(Int32(type), Int32(charCode), Int32(keyCode), Int32(modifiers))
    }
}
