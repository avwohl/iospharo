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

    private var texUpdateLogCount = 0

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

        // Pixel value diagnostics — log periodically
        texUpdateLogCount += 1
        if texUpdateLogCount <= 5 || texUpdateLogCount % 300 == 0 {
            let center = height > 384 && width > 512 ? bits[384 * width + 512] : bits[0]
            let corner = bits[0]
            var nonZero = 0
            let step = max(1, width * height / 1000)
            var idx = 0
            while idx < width * height {
                if bits[idx] != 0 { nonZero += 1 }
                idx += step
            }
            NSLog("[TEX-UPDATE] #%d %dx%d center=0x%08x corner=0x%08x nonZero=%d/1000",
                  texUpdateLogCount, width, height,
                  center, corner, nonZero)
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

    private var lastDrawTime = CFAbsoluteTimeGetCurrent()
    private var drawGapCount = 0

    func draw(in view: MTKView) {
        drawCount += 1
        let now = CFAbsoluteTimeGetCurrent()
        let gap = now - lastDrawTime
        lastDrawTime = now

        // Log if there's a gap > 1s (display link stopped and restarted?)
        if gap > 1.0 && drawCount > 1 {
            drawGapCount += 1
            NSLog("[METAL-GAP] #%d gap=%.1fs (gap#%d) view.window=%@",
                  drawCount, gap, drawGapCount,
                  view.window != nil ? "yes" : "nil")
        }

        updateDisplayTexture()

        guard let texture = displayTexture else {
            if drawCount <= 10 || drawCount % 300 == 0 {
                NSLog("[METAL-DRAW] #%d NO TEXTURE", drawCount)
            }
            return
        }
        guard let drawable = view.currentDrawable else {
            if drawCount <= 10 || drawCount % 300 == 0 {
                NSLog("[METAL-DRAW] #%d NO DRAWABLE", drawCount)
            }
            return
        }
        guard let rpd = view.currentRenderPassDescriptor else {
            if drawCount <= 10 || drawCount % 300 == 0 {
                NSLog("[METAL-DRAW] #%d NO RPD", drawCount)
            }
            return
        }
        guard let cmdBuf = commandQueue.makeCommandBuffer() else { return }

        guard let enc = cmdBuf.makeRenderCommandEncoder(descriptor: rpd) else { return }

        enc.setRenderPipelineState(pipelineState)
        enc.setFragmentTexture(texture, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()

        cmdBuf.present(drawable)
        cmdBuf.commit()

        // Save display buffer directly as PNG (UIKit drawHierarchy doesn't capture Metal on Mac Catalyst)
        if drawCount == 900 || drawCount == 1500 || drawCount == 2100 {
            saveDisplayBufferAsPNG(frame: drawCount)
        }

        if drawCount <= 5 || drawCount % 600 == 0 {
            NSLog("[METAL-DRAW] #%d tex=%dx%d view=%@ isPaused=%d",
                  drawCount, texture.width, texture.height,
                  view.window != nil ? "inWindow" : "noWindow",
                  view.isPaused ? 1 : 0)
        }
    }

    // MARK: - Direct Buffer Screenshot

    private func saveDisplayBufferAsPNG(frame: Int) {
        guard let bridge = bridge else { return }
        let (pixels, width, height, _) = bridge.getDisplayBufferInfo()
        guard let bits = pixels, width > 0, height > 0 else {
            NSLog("[SCREENSHOT] No display buffer at frame %d", frame)
            return
        }

        // Create CGImage from raw BGRA pixel data
        let bytesPerRow = width * 4
        let totalBytes = bytesPerRow * height
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
            NSLog("[SCREENSHOT] Failed to create CGContext at frame %d", frame)
            return
        }

        guard let cgImage = context.makeImage() else {
            NSLog("[SCREENSHOT] Failed to create CGImage at frame %d", frame)
            return
        }

        let image = UIImage(cgImage: cgImage)
        if let data = image.pngData() {
            let path = "/tmp/iospharo-buf-\(frame).png"
            try? data.write(to: URL(fileURLWithPath: path))
            NSLog("[SCREENSHOT] Saved buffer %dx%d to %@", width, height, path as NSString)
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
