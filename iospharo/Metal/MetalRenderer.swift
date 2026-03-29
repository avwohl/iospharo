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

    init?(metalView: MTKView, bridge: PharoBridge) {
        guard let device = MTLCreateSystemDefaultDevice() else {
            NSLog("MetalRenderer: Failed to create Metal device")
            return nil
        }

        guard let commandQueue = device.makeCommandQueue() else {
            NSLog("MetalRenderer: Failed to create command queue")
            return nil
        }

        self.device = device
        self.commandQueue = commandQueue
        self.bridge = bridge

        // Configure the Metal view
        metalView.device = device
        metalView.colorPixelFormat = .bgra8Unorm
        metalView.clearColor = MTLClearColor(red: 0.92, green: 0.92, blue: 0.92, alpha: 1.0)

        // Create the render pipeline
        guard let library = device.makeDefaultLibrary() else {
            NSLog("MetalRenderer: Failed to create shader library")
            return nil
        }

        guard let vertexFunction = library.makeFunction(name: "vertexShader"),
              let fragmentFunction = library.makeFunction(name: "fragmentShader") else {
            NSLog("MetalRenderer: Failed to load shader functions")
            return nil
        }

        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = vertexFunction
        pipelineDescriptor.fragmentFunction = fragmentFunction
        pipelineDescriptor.colorAttachments[0].pixelFormat = metalView.colorPixelFormat

        do {
            self.pipelineState = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        } catch {
            NSLog("MetalRenderer: Failed to create pipeline state: %@", error.localizedDescription)
            return nil
        }

        super.init()

        metalView.delegate = self

        if let metalLayer = metalView.layer as? CAMetalLayer {
            metalLayer.framebufferOnly = true
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

        // Fill with grey matching the view's clear color to prevent garbage flash.
        // GPU texture memory is uninitialized and may contain stale data from
        // previous operations, which appears as random colored pixels.
        if let texture = displayTexture {
            let count = width * height
            let grey = [UInt32](repeating: 0xFFEBEBEB, count: count)
            grey.withUnsafeBufferPointer { buf in
                texture.replace(
                    region: MTLRegion(
                        origin: MTLOrigin(x: 0, y: 0, z: 0),
                        size: MTLSize(width: width, height: height, depth: 1)
                    ),
                    mipmapLevel: 0,
                    withBytes: buf.baseAddress!,
                    bytesPerRow: width * 4
                )
            }
        }

        #if DEBUG
        NSLog("[METAL] Created texture \(width)x\(height)")
        #endif
    }

    // MARK: - MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        // Use logical points (view.bounds) not physical pixels (size parameter).
        // The Metal renderer stretches the texture to fill the drawable, handling
        // retina scaling automatically. Using physical pixels would make Pharo
        // text microscopic on 2x/3x retina displays.
        let width = Int(view.bounds.size.width)
        let height = Int(view.bounds.size.height)

        #if DEBUG
        NSLog("[METAL] drawableSizeWillChange: drawable=\(Int(size.width))x\(Int(size.height)), bounds=\(width)x\(height)")
        #endif
        bridge?.setDisplaySize(width: width, height: height)
    }

    private var drawCount = 0

    func draw(in view: MTKView) {
        drawCount += 1

        // Don't render the Pharo framebuffer until Pharo has completed its first
        // frame. Two paths: SDL (SDL_RenderPresent sets sdlActive) or Display Form
        // (primitiveBeDisplay sets displayFormReady). P13 uses SDL; P14 may use
        // the Display Form path during startup before SDL initializes.
        guard ffi_isSDLRenderingActive() || vm_isDisplayFormReady() else {
            // Just present the clear color (light gray)
            guard let drawable = view.currentDrawable,
                  let rpd = view.currentRenderPassDescriptor,
                  let cmdBuf = commandQueue.makeCommandBuffer(),
                  let enc = cmdBuf.makeRenderCommandEncoder(descriptor: rpd) else { return }
            enc.endEncoding()
            cmdBuf.present(drawable)
            cmdBuf.commit()
            return
        }

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

        #if DEBUG
        if drawCount <= 3 || drawCount % 1800 == 0 {
            NSLog("[METAL-DRAW] #%d tex=%dx%d", drawCount, texture.width, texture.height)
        }
        #endif
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
            #if DEBUG
            NSLog("[SCREENSHOT] No display buffer for tag=%@", tag as NSString)
            #endif
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
            #if DEBUG
            NSLog("[SCREENSHOT] Failed to create CGContext for tag=%@", tag as NSString)
            #endif
            return
        }

        guard let cgImage = context.makeImage() else {
            #if DEBUG
            NSLog("[SCREENSHOT] Failed to create CGImage for tag=%@", tag as NSString)
            #endif
            return
        }

        let image = UIImage(cgImage: cgImage)
        if let data = image.pngData() {
            let path = FileManager.default.temporaryDirectory
                .appendingPathComponent("iospharo-\(tag).png").path
            try? data.write(to: URL(fileURLWithPath: path))
            #if DEBUG
            NSLog("[SCREENSHOT] Saved %dx%d to %@", width, height, path as NSString)
            #endif
        }
    }
}
