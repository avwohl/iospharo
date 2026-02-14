/*
 * MetalRenderer.swift
 *
 * Metal rendering backend for displaying the Pharo VM framebuffer.
 * Updates a texture from VM display bits and renders it to screen.
 *
 * Note: screencapture cannot capture Metal layer content from Mac Catalyst apps.
 * Use saveUIKitScreenshot() or check /tmp/iospharo-uikit-*.png for visual verification.
 */

import Metal
import MetalKit
import simd

/// Metal renderer for the Pharo display
@MainActor
class MetalRenderer: NSObject, MTKViewDelegate {

    // Metal objects
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let pipelineState: MTLRenderPipelineState

    // Display texture
    private var displayTexture: MTLTexture?
    private var textureWidth: Int = 0
    private var textureHeight: Int = 0

    // Reference to bridge
    private weak var bridge: PharoBridge?

    /// Initialize the renderer
    init?(metalView: MTKView, bridge: PharoBridge) {
        guard let device = MTLCreateSystemDefaultDevice() else {
            print("MetalRenderer: Failed to create Metal device")
            return nil
        }

        guard let commandQueue = device.makeCommandQueue() else {
            print("MetalRenderer: Failed to create command queue")
            return nil
        }

        self.device = device
        self.commandQueue = commandQueue
        self.bridge = bridge

        // Configure the Metal view
        metalView.device = device
        metalView.colorPixelFormat = .bgra8Unorm
        metalView.clearColor = MTLClearColor(red: 0.15, green: 0.20, blue: 0.22, alpha: 1.0)

        // Create the render pipeline
        guard let library = device.makeDefaultLibrary() else {
            print("MetalRenderer: Failed to create shader library")
            return nil
        }

        guard let vertexFunction = library.makeFunction(name: "vertexShader"),
              let fragmentFunction = library.makeFunction(name: "fragmentShader") else {
            print("MetalRenderer: Failed to load shader functions")
            return nil
        }

        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = vertexFunction
        pipelineDescriptor.fragmentFunction = fragmentFunction
        pipelineDescriptor.colorAttachments[0].pixelFormat = metalView.colorPixelFormat

        do {
            self.pipelineState = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        } catch {
            print("MetalRenderer: Failed to create pipeline state: \(error)")
            return nil
        }

        super.init()

        metalView.delegate = self

        if let metalLayer = metalView.layer as? CAMetalLayer {
            metalLayer.framebufferOnly = false
            NSLog("[METAL-INIT] drawableSize=\(metalLayer.drawableSize) contentsScale=\(metalLayer.contentsScale)")
        }
    }

    // MARK: - Texture Management

    func updateDisplayTexture() {
        guard let bridge = bridge else { return }

        let (pixels, width, height, _) = bridge.getDisplayBufferInfo()

        guard let bits = pixels, width > 0, height > 0 else {
            return
        }

        if displayTexture == nil ||
           textureWidth != width ||
           textureHeight != height {
            createTexture(width: width, height: height)
        }

        guard let texture = displayTexture else {
            return
        }

        let region = MTLRegion(
            origin: MTLOrigin(x: 0, y: 0, z: 0),
            size: MTLSize(width: width, height: height, depth: 1)
        )

        texture.replace(
            region: region,
            mipmapLevel: 0,
            withBytes: bits,
            bytesPerRow: width * 4
        )
    }

    private func createTexture(width: Int, height: Int) {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm,
            width: width,
            height: height,
            mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared

        displayTexture = device.makeTexture(descriptor: descriptor)
        textureWidth = width
        textureHeight = height

        NSLog("[METAL] Created texture \(width)x\(height)")
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        let width = Int(view.bounds.size.width)
        let height = Int(view.bounds.size.height)
        NSLog("[METAL] drawableSizeWillChange: drawable=\(Int(size.width))x\(Int(size.height)), bounds=\(width)x\(height)")
        bridge?.setDisplaySize(width: width, height: height)
    }

    private var drawCount = 0

    func draw(in view: MTKView) {
        drawCount += 1

        updateDisplayTexture()

        guard let texture = displayTexture else { return }
        guard let drawable = view.currentDrawable else { return }
        guard let rpd = view.currentRenderPassDescriptor else { return }
        guard let cmdBuf = commandQueue.makeCommandBuffer() else { return }

        guard let enc = cmdBuf.makeRenderCommandEncoder(descriptor: rpd) else { return }

        enc.setRenderPipelineState(pipelineState)
        enc.setFragmentTexture(texture, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()

        cmdBuf.present(drawable)
        cmdBuf.commit()

        // UIKit screenshot captures the composited view hierarchy (Metal content included).
        // screencapture cannot see Metal layers on Mac Catalyst.
        if drawCount == 900 {
            saveUIKitScreenshot(view, frame: drawCount)
        }

        if drawCount <= 3 || drawCount % 600 == 0 {
            NSLog("[METAL-DRAW] #\(drawCount) tex=\(texture.width)x\(texture.height)")
        }
    }

    // MARK: - Screenshot (UIKit-based, works on Mac Catalyst)

    private func saveUIKitScreenshot(_ view: MTKView, frame: Int) {
        guard let window = view.window else {
            NSLog("[SCREENSHOT] No window at frame %d", frame)
            return
        }
        let renderer = UIGraphicsImageRenderer(bounds: window.bounds)
        let image = renderer.image { ctx in
            window.drawHierarchy(in: window.bounds, afterScreenUpdates: false)
        }
        if let data = image.pngData() {
            let path = "/tmp/iospharo-uikit-\(frame).png"
            try? data.write(to: URL(fileURLWithPath: path))
            NSLog("[SCREENSHOT] Saved %dx%d to %@", Int(image.size.width), Int(image.size.height), path as NSString)
        }
    }

    // MARK: - View Hierarchy Dump

    private func dumpViewHierarchy(_ view: UIView) {
        var v: UIView? = view
        var depth = 0
        while let current = v {
            let layerType = type(of: current.layer)
            NSLog("[METAL-HIERARCHY] depth=\(depth) \(type(of: current)) frame=\(current.frame) hidden=\(current.isHidden) layer=\(layerType)")
            v = current.superview
            depth += 1
        }
    }
}
