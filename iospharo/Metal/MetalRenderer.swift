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

        // Log Metal layer properties
        if let metalLayer = metalView.layer as? CAMetalLayer {
            NSLog("[METAL-INIT] layer.drawableSize=\(metalLayer.drawableSize) framebufferOnly=\(metalLayer.framebufferOnly) contentsScale=\(metalLayer.contentsScale) pixelFormat=\(metalLayer.pixelFormat.rawValue)")
        }
    }

    // MARK: - Texture Management

    func updateDisplayTexture() {
        guard let bridge = bridge else { return }

        // Get display info atomically
        let (pixels, width, height, _) = bridge.getDisplayBufferInfo()

        guard let bits = pixels, width > 0, height > 0 else {
            return
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

        // Always update the texture from the VM's front buffer
        updateDisplayTexture()

        guard let texture = displayTexture else { return }
        guard let drawable = view.currentDrawable else { return }
        guard let rpd = view.currentRenderPassDescriptor else { return }
        guard let cmdBuf = commandQueue.makeCommandBuffer() else { return }

        // First frame: dump full diagnostics
        if drawCount == 1 {
            dumpViewHierarchy(view)
            if let metalLayer = view.layer as? CAMetalLayer {
                NSLog("[METAL-DIAG] drawableSize=\(metalLayer.drawableSize) framebufferOnly=\(metalLayer.framebufferOnly) presentsWithTransaction=\(metalLayer.presentsWithTransaction)")
            }
            let att = rpd.colorAttachments[0]!
            NSLog("[METAL-DIAG] rpd.colorAttachment[0]: loadAction=\(att.loadAction.rawValue) storeAction=\(att.storeAction.rawValue) texture=\(att.texture?.width ?? 0)x\(att.texture?.height ?? 0)")
        }

        guard let enc = cmdBuf.makeRenderCommandEncoder(descriptor: rpd) else { return }

        // Draw textured full-screen quad
        enc.setRenderPipelineState(pipelineState)
        enc.setFragmentTexture(texture, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()

        cmdBuf.present(drawable)
        cmdBuf.commit()

        // First 3 frames + every 300th: wait for completion and verify
        if drawCount <= 3 || drawCount % 300 == 0 {
            cmdBuf.waitUntilCompleted()
            let statusNames = ["notEnqueued", "enqueued", "committed", "scheduled", "completed", "error"]
            let statusName = cmdBuf.status.rawValue < statusNames.count ? statusNames[Int(cmdBuf.status.rawValue)] : "unknown"
            NSLog("[METAL-DRAW] #\(drawCount) status=\(statusName) error=\(cmdBuf.error?.localizedDescription ?? "none") tex=\(texture.width)x\(texture.height) drawable=\(drawable.texture.width)x\(drawable.texture.height)")

            // Readback from source texture (shared storage, always readable)
            var srcPixel: UInt32 = 0
            let mid = MTLRegion(origin: MTLOrigin(x: texture.width/2, y: texture.height/2, z: 0),
                                size: MTLSize(width: 1, height: 1, depth: 1))
            texture.getBytes(&srcPixel, bytesPerRow: 4, from: mid, mipmapLevel: 0)
            NSLog("[METAL-DRAW] #\(drawCount) srcTexture center pixel=\(String(format: "%08X", srcPixel))")
        }

        // Save texture as PNG periodically for visual verification
        if drawCount == 1800 || drawCount == 3600 || drawCount == 6000 {
            saveTextureToPNG(texture, path: "/tmp/iospharo-texture-\(drawCount).png")
        }
    }

    private func saveTextureToPNG(_ texture: MTLTexture, path: String) {
        let w = texture.width
        let h = texture.height
        let bytesPerRow = w * 4
        var pixels = [UInt8](repeating: 0, count: h * bytesPerRow)
        texture.getBytes(&pixels, bytesPerRow: bytesPerRow,
                         from: MTLRegion(origin: MTLOrigin(x: 0, y: 0, z: 0),
                                         size: MTLSize(width: w, height: h, depth: 1)),
                         mipmapLevel: 0)

        // BGRA → RGBA swap for CGImage
        for i in stride(from: 0, to: pixels.count, by: 4) {
            let b = pixels[i]
            pixels[i] = pixels[i+2]     // R
            pixels[i+2] = b             // B
        }

        // Sample pixels for logging
        let p1 = 50 * bytesPerRow + 50 * 4
        let p2 = (h/2) * bytesPerRow + (w/2) * 4
        NSLog("[METAL-SAVE] Pixel(50,50)=R\(pixels[p1])G\(pixels[p1+1])B\(pixels[p1+2])A\(pixels[p1+3])")
        NSLog("[METAL-SAVE] Pixel(center)=R\(pixels[p2])G\(pixels[p2+1])B\(pixels[p2+2])A\(pixels[p2+3])")

        // Count unique colors and non-black pixels
        var nonBlack = 0
        var uniqueColors = Set<UInt32>()
        for y in stride(from: 0, to: h, by: 10) {
            for x in stride(from: 0, to: w, by: 10) {
                let off = y * bytesPerRow + x * 4
                let r = UInt32(pixels[off])
                let g = UInt32(pixels[off+1])
                let b = UInt32(pixels[off+2])
                let color = (r << 16) | (g << 8) | b
                uniqueColors.insert(color)
                if color != 0 { nonBlack += 1 }
            }
        }
        NSLog("[METAL-SAVE] \(w)x\(h) uniqueColors=\(uniqueColors.count) nonBlackSamples=\(nonBlack)")

        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: &pixels, width: w, height: h,
                                 bitsPerComponent: 8, bytesPerRow: bytesPerRow,
                                 space: colorSpace,
                                 bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
            NSLog("[METAL-SAVE] Failed to create CGContext")
            return
        }

        guard let image = ctx.makeImage() else {
            NSLog("[METAL-SAVE] Failed to make CGImage")
            return
        }

        let url = URL(fileURLWithPath: path)
        guard let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else {
            NSLog("[METAL-SAVE] Failed to create image destination")
            return
        }
        CGImageDestinationAddImage(dest, image, nil)
        CGImageDestinationFinalize(dest)
        NSLog("[METAL-SAVE] Saved texture to \(path)")
    }

    private func dumpViewHierarchy(_ view: UIView) {
        var v: UIView? = view
        var depth = 0
        while let current = v {
            let layerType = type(of: current.layer)
            NSLog("[METAL-HIERARCHY] depth=\(depth) \(type(of: current)) frame=\(current.frame) hidden=\(current.isHidden) alpha=\(current.alpha) opaque=\(current.isOpaque) layer=\(layerType) clips=\(current.clipsToBounds)")
            v = current.superview
            depth += 1
        }
        // Also dump the window
        if let window = view.window {
            NSLog("[METAL-HIERARCHY] window=\(type(of: window)) frame=\(window.frame) isKeyWindow=\(window.isKeyWindow)")
            NSLog("[METAL-HIERARCHY] window.rootViewController=\(type(of: window.rootViewController ?? (NSObject() as AnyObject)))")
        } else {
            NSLog("[METAL-HIERARCHY] WARNING: view.window is nil!")
        }
    }
}
