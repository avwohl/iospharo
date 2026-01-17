/*
 * MetalRenderer.swift
 *
 * Metal rendering backend for displaying the Pharo VM framebuffer.
 * Updates a texture from VM display bits and renders it to screen.
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
        // Get the default Metal device
        guard let device = MTLCreateSystemDefaultDevice() else {
            print("MetalRenderer: Failed to create Metal device")
            return nil
        }

        // Create command queue
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
        metalView.clearColor = MTLClearColor(red: 0.2, green: 0.2, blue: 0.2, alpha: 1.0)

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
    }

    // MARK: - Texture Management

    /// Update the display texture from VM framebuffer
    /// WIP: Resize tearing not fully fixed yet
    private var updateTextureCount = 0

    func updateDisplayTexture() {
        guard let bridge = bridge else { return }

        // Get display info atomically
        let (pixels, width, height, size) = bridge.getDisplayBufferInfo()

        guard let bits = pixels, width > 0, height > 0 else {
            return
        }

        // Log texture updates and sample pixel data
        updateTextureCount += 1
        // Log more frequently to see changes after user interaction
        if updateTextureCount <= 5 || updateTextureCount % 100 == 0 {
            // Sample menu bar area to understand the layout
            // Menu bar in Pharo is typically 28 points tall
            // In 2048x1536 (2x Retina), that would be y=0-56 pixels
            // OR if coordinates aren't scaled, y=0-28

            // Pixel at (50, 25) - should be on menu bar if not scaled
            let p1 = 25 * width + 50
            let pixel1 = p1 < size ? bits[p1] : 0

            // Pixel at (50, 50) - below menu bar if not scaled, on menu bar if scaled
            let p2 = 50 * width + 50
            let pixel2 = p2 < size ? bits[p2] : 0

            // Pixel at (50, 75) - definitely below menu bar
            let p3 = 75 * width + 50
            let pixel3 = p3 < size ? bits[p3] : 0

            // Log pixel values
            NSLog("[METAL] updateTexture #\(updateTextureCount): \(width)x\(height) " +
                  "(50,25)=\(String(format:"0x%08X", pixel1)) " +
                  "(50,50)=\(String(format:"0x%08X", pixel2)) " +
                  "(50,75)=\(String(format:"0x%08X", pixel3))")
        }

        // Recreate texture if size changed
        if displayTexture == nil ||
           textureWidth != width ||
           textureHeight != height {
            createTexture(width: width, height: height)
        }

        guard let texture = displayTexture else {
            return
        }

        // Copy framebuffer to texture
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

        print("MetalRenderer: Created texture \(width)x\(height)")
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // Notify bridge of size change
        let width = Int(size.width)
        let height = Int(size.height)
        bridge?.setDisplaySize(width: width, height: height)
    }

    func draw(in view: MTKView) {
        // Update texture if display changed OR if we don't have a texture yet
        if bridge?.displayNeedsUpdate == true || displayTexture == nil {
            updateDisplayTexture()
            bridge?.displayDidUpdate()
        }

        // Ensure we have a texture and drawable
        guard let texture = displayTexture,
              let drawable = view.currentDrawable,
              let renderPassDescriptor = view.currentRenderPassDescriptor else {
            return
        }

        // Create command buffer
        guard let commandBuffer = commandQueue.makeCommandBuffer() else {
            return
        }

        // Create render encoder
        guard let renderEncoder = commandBuffer.makeRenderCommandEncoder(
            descriptor: renderPassDescriptor
        ) else {
            return
        }

        // Set pipeline and texture
        renderEncoder.setRenderPipelineState(pipelineState)
        renderEncoder.setFragmentTexture(texture, index: 0)

        // Draw full-screen quad (triangle strip with 4 vertices)
        renderEncoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)

        renderEncoder.endEncoding()

        // Present and commit
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}
