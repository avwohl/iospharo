import Foundation
import Combine

class VMController: ObservableObject {
    @Published var isRunning = false
    @Published var isLoaded = false
    @Published var statusMessage = "Ready"

    private var displayPixels: UnsafeMutablePointer<UInt32>?
    private var displayWidth: Int = 1024
    private var displayHeight: Int = 768

    func initialize() {
        statusMessage = "Initializing VM..."

        let heapSize = 256 * 1024 * 1024  // 256 MB
        guard vm_initialize(heapSize) else {
            statusMessage = "Failed to initialize VM"
            return
        }

        // Set up display
        vm_setDisplaySize(Int32(displayWidth), Int32(displayHeight), 32)
        displayPixels = vm_getDisplayPixels()

        statusMessage = "VM initialized"
    }

    func loadImage(path: String) {
        statusMessage = "Loading image..."

        guard vm_loadImage(path) else {
            statusMessage = "Failed to load image"
            return
        }

        isLoaded = true
        statusMessage = "Image loaded"
    }

    func start() {
        guard isLoaded && !isRunning else { return }

        statusMessage = "Running..."
        vm_run()
        isRunning = true
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
