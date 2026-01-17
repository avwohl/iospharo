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
        // Log EVERY frame to catch menu changes
        if updateTextureCount <= 5 || updateTextureCount % 10 == 0 {
            // Sample where dropdown menu would appear (below menu bar)
            // Menu bar ends around y=75, dropdown would be y=75-300

            // Pixel at (50, 50) - on menu bar
            let p1 = 50 * width + 50
            let pixel1 = p1 < size ? bits[p1] : 0

            // Pixel at (50, 100) - where dropdown should appear
            let p2 = 100 * width + 50
            let pixel2 = p2 < size ? bits[p2] : 0

            // Pixel at (50, 150) - inside dropdown area
            let p3 = 150 * width + 50
            let pixel3 = p3 < size ? bits[p3] : 0

            // Pixel at (50, 200) - inside dropdown area
            let p4 = 200 * width + 50
            let pixel4 = p4 < size ? bits[p4] : 0

            // Log pixel values
            NSLog("[METAL] #\(updateTextureCount): " +
                  "bar(50,50)=\(String(format:"%08X", pixel1)) " +
                  "drop(50,100)=\(String(format:"%08X", pixel2)) " +
                  "drop(50,150)=\(String(format:"%08X", pixel3)) " +
                  "drop(50,200)=\(String(format:"%08X", pixel4))")
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
        // IMPORTANT: Use view bounds (in points) not drawable size (in pixels)
        // Touch events come in point coordinates, so Pharo must also use point coordinates
        // Otherwise menus appear at wrong positions (e.g., world menu at half-coordinates)
        let width = Int(view.bounds.size.width)
        let height = Int(view.bounds.size.height)
        NSLog("[METAL] drawableSizeWillChange: drawable=\(Int(size.width))x\(Int(size.height)), using bounds=\(width)x\(height)")
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
