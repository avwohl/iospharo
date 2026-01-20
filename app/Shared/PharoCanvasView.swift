import SwiftUI
import AppKit

struct PharoCanvasView: NSViewRepresentable {
    @EnvironmentObject var vmController: VMController

    func makeNSView(context: Context) -> PharoNSView {
        let view = PharoNSView()
        view.vmController = vmController
        // Ensure the view can receive mouse events
        DispatchQueue.main.async {
            view.window?.makeFirstResponder(view)
        }
        return view
    }

    func updateNSView(_ nsView: PharoNSView, context: Context) {
        nsView.vmController = vmController
    }
}

class PharoNSView: NSView {
    var vmController: VMController?
    private var displayLink: CVDisplayLink?
    private var bitmapContext: CGContext?

    override init(frame: NSRect) {
        super.init(frame: frame)
        setupDisplayLink()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setupDisplayLink()
    }

    deinit {
        if let displayLink = displayLink {
            CVDisplayLinkStop(displayLink)
        }
    }

    private func setupDisplayLink() {
        CVDisplayLinkCreateWithActiveCGDisplays(&displayLink)
        guard let displayLink = displayLink else { return }

        let callback: CVDisplayLinkOutputCallback = { _, _, _, _, _, userInfo in
            let view = Unmanaged<PharoNSView>.fromOpaque(userInfo!).takeUnretainedValue()
            DispatchQueue.main.async {
                view.setNeedsDisplay(view.bounds)
            }
            return kCVReturnSuccess
        }

        CVDisplayLinkSetOutputCallback(displayLink, callback,
            Unmanaged.passUnretained(self).toOpaque())
        CVDisplayLinkStart(displayLink)
    }

    override var acceptsFirstResponder: Bool { true }

    // Prevent system context menu from intercepting right-clicks
    override func menu(for event: NSEvent) -> NSMenu? {
        return nil  // Let rightMouseDown handle right-clicks
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let context = NSGraphicsContext.current?.cgContext,
              let vmController = vmController,
              let pixels = vmController.getDisplayPixels() else {
            // Draw gray background if no pixels
            NSColor.darkGray.setFill()
            dirtyRect.fill()
            return
        }

        let (width, height) = vmController.getDisplaySize()
        guard width > 0 && height > 0 else { return }

        // Create bitmap image from pixel data
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)

        guard let dataProvider = CGDataProvider(dataInfo: nil, data: pixels, size: width * height * 4, releaseData: { _, _, _ in }) else { return }

        guard let image = CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: colorSpace,
            bitmapInfo: bitmapInfo,
            provider: dataProvider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else { return }

        // Draw scaled to fit
        context.draw(image, in: bounds)
    }

    // Mouse handling
    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        let x = Int(point.x)
        let y = Int(bounds.height - point.y)
        print("[SWIFT] mouseDown at (\(x), \(y)) buttons=4")
        vmController?.postMouseEvent(type: 1, x: x, y: y, buttons: 4, modifiers: modifiers(from: event))
    }

    override func mouseUp(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        vmController?.postMouseEvent(type: 2, x: Int(point.x), y: Int(bounds.height - point.y), buttons: 0, modifiers: modifiers(from: event))
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        vmController?.postMouseEvent(type: 0, x: Int(point.x), y: Int(bounds.height - point.y), buttons: 4, modifiers: modifiers(from: event))
    }

    override func mouseMoved(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        vmController?.postMouseEvent(type: 0, x: Int(point.x), y: Int(bounds.height - point.y), buttons: 0, modifiers: modifiers(from: event))
    }

    override func rightMouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        vmController?.postMouseEvent(type: 1, x: Int(point.x), y: Int(bounds.height - point.y), buttons: 2, modifiers: modifiers(from: event))
    }

    override func rightMouseUp(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        vmController?.postMouseEvent(type: 2, x: Int(point.x), y: Int(bounds.height - point.y), buttons: 0, modifiers: modifiers(from: event))
    }

    // Keyboard handling
    override func keyDown(with event: NSEvent) {
        let charCode = Int(event.characters?.unicodeScalars.first?.value ?? 0)
        vmController?.postKeyEvent(type: 0, charCode: charCode, keyCode: Int(event.keyCode), modifiers: modifiers(from: event))
    }

    override func keyUp(with event: NSEvent) {
        let charCode = Int(event.characters?.unicodeScalars.first?.value ?? 0)
        vmController?.postKeyEvent(type: 1, charCode: charCode, keyCode: Int(event.keyCode), modifiers: modifiers(from: event))
    }

    private func modifiers(from event: NSEvent) -> Int {
        var mods = 0
        if event.modifierFlags.contains(.shift) { mods |= 1 }
        if event.modifierFlags.contains(.control) { mods |= 2 }
        if event.modifierFlags.contains(.option) { mods |= 4 }
        if event.modifierFlags.contains(.command) { mods |= 8 }
        return mods
    }
}
