/*
 * MetalRenderer.swift
 *
 * Metal rendering backend for displaying the Pharo VM framebuffer.
 * Updates a texture from VM display bits and renders it to screen.
 *
 * Note: screencapture cannot capture Metal layer content from Mac Catalyst apps.
 * Use saveDisplayBufferAsPNG() for visual verification — it reads the VM's
 * display buffer directly via CGContext.
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

        if drawCount <= 3 || drawCount % 1800 == 0 {
            NSLog("[METAL-DRAW] #%d tex=%dx%d", drawCount, texture.width, texture.height)
        }
    }

    // MARK: - Direct Buffer Screenshot

    /// Save the VM's display buffer as a PNG file.
    /// This is the only reliable screenshot method — UIKit drawHierarchy doesn't
    /// capture Metal content on Mac Catalyst, and screencapture can't capture
    /// Metal layers from headless machines.
    func saveDisplayBufferAsPNG(tag: String) {
        guard let bridge = bridge else { return }
        let (pixels, width, height, _) = bridge.getDisplayBufferInfo()
        guard let bits = pixels, width > 0, height > 0 else {
            NSLog("[SCREENSHOT] No display buffer for tag=%@", tag as NSString)
            return
        }

        let bytesPerRow = width * 4
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        // Pharo stores ARGB big-endian = BGRA in memory (little-endian)
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)

        guard let context = CGContext(
            data: UnsafeMutableRawPointer(bits),
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: bytesPerRow,
            space: colorSpace,
            bitmapInfo: bitmapInfo.rawValue
        ) else {
            NSLog("[SCREENSHOT] Failed to create CGContext for tag=%@", tag as NSString)
            return
        }

        guard let cgImage = context.makeImage() else {
            NSLog("[SCREENSHOT] Failed to create CGImage for tag=%@", tag as NSString)
            return
        }

        let image = UIImage(cgImage: cgImage)
        if let data = image.pngData() {
            let path = "/tmp/iospharo-\(tag).png"
            try? data.write(to: URL(fileURLWithPath: path))
            NSLog("[SCREENSHOT] Saved %dx%d to %@", width, height, path as NSString)
        }
    }
}
