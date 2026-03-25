/*
 * AppTemplates.swift
 *
 * Template strings for all generated files in the exported Xcode project.
 * Each static method returns the content of one generated file.
 */

import Foundation

enum AppTemplates {

    // MARK: - Swift Sources

    static func appSwift(appName: String) -> String {
        """
        import SwiftUI

        @main
        struct \(appName)App: App {
            @UIApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
            @StateObject private var bridge = PharoBridge.shared

            var body: some Scene {
                WindowGroup {
                    ContentView()
                        .environmentObject(bridge)
                }
                .commands {
                    CommandGroup(replacing: .appTermination) {
                        Button("Quit \(appName)") {
                            PharoBridge.shared.stop()
                            exit(0)
                        }
                        .keyboardShortcut("q")
                    }
                }
            }
        }

        class AppDelegate: NSObject, UIApplicationDelegate {
            func application(_ application: UIApplication,
                             didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
                return true
            }

            func applicationWillTerminate(_ application: UIApplication) {
                MainActor.assumeIsolated {
                    PharoBridge.shared.stop()
                }
            }

            #if targetEnvironment(macCatalyst)
            func application(_ application: UIApplication,
                             configurationForConnecting connectingSceneSession: UISceneSession,
                             options: UIScene.ConnectionOptions) -> UISceneConfiguration {
                let config = UISceneConfiguration(name: nil, sessionRole: connectingSceneSession.role)
                config.delegateClass = SceneDelegate.self
                return config
            }
            #endif
        }

        #if targetEnvironment(macCatalyst)
        class SceneDelegate: NSObject, UIWindowSceneDelegate {
            func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
                       options connectionOptions: UIScene.ConnectionOptions) {
                guard let windowScene = scene as? UIWindowScene else { return }
                if let sizeRestrictions = windowScene.sizeRestrictions {
                    sizeRestrictions.minimumSize = CGSize(width: 800, height: 600)
                    sizeRestrictions.maximumSize = CGSize(width: 2560, height: 1600)
                }
                if #available(macCatalyst 16.0, *) {
                    let prefs = UIWindowScene.GeometryPreferences.Mac(
                        systemFrame: CGRect(x: 100, y: 100, width: 1024, height: 768))
                    windowScene.requestGeometryUpdate(prefs) { _ in }
                }
                if let titlebar = windowScene.titlebar {
                    titlebar.titleVisibility = .hidden
                    titlebar.toolbarStyle = .automatic
                }
            }

            func sceneDidDisconnect(_ scene: UIScene) {
                let remaining = UIApplication.shared.connectedScenes.filter { $0 != scene }
                if remaining.isEmpty {
                    PharoBridge.shared.stop()
                    exit(0)
                }
            }
        }
        #endif
        """
    }

    static func contentViewSwift(appName: String, imageFileName: String) -> String {
        """
        import SwiftUI

        struct ContentView: View {
            @EnvironmentObject var bridge: PharoBridge

            var body: some View {
                ZStack {
                    if bridge.isRunning {
                        PharoCanvasView(bridge: bridge)
                            .ignoresSafeArea()
                    } else {
                        ProgressView("Starting \\(appName)...")
                    }

                    if let error = bridge.errorMessage {
                        VStack {
                            Spacer()
                            HStack {
                                Image(systemName: "exclamationmark.triangle.fill")
                                    .foregroundColor(.yellow)
                                Text(error)
                                    .foregroundColor(.white)
                            }
                            .padding()
                            .background(Color.red.opacity(0.8))
                            .cornerRadius(10)
                            .padding()
                        }
                    }
                }
                .onAppear {
                    guard !bridge.isRunning else { return }
                    // Find the embedded image in the app bundle's Resources/
                    if let imageURL = Bundle.main.url(forResource: "\((imageFileName as NSString).deletingPathExtension)",
                                                       withExtension: "image",
                                                       subdirectory: "Resources") {
                        if bridge.loadImage(at: imageURL.path) {
                            bridge.start()
                        }
                    } else {
                        bridge.errorMessage = "Embedded image not found: \(imageFileName)"
                    }
                }
            }
        }
        """
    }

    static func pharoBridgeSwift() -> String {
        """
        import Foundation
        import Combine
        import UIKit

        private var gClipboardBuffer: UnsafeMutablePointer<CChar>?

        @MainActor
        class PharoBridge: ObservableObject {
            static let shared = PharoBridge()

            @Published var isRunning = false
            @Published var isInitialized = false
            @Published var errorMessage: String?

            private var imagePath: String?
            private var displayCallback: IOSDisplayUpdateCallback?

            private init() {
                setupDisplayCallback()
                setupClipboardCallbacks()
                setupTextInputCallback()
            }

            private func setupDisplayCallback() {
                let callback: IOSDisplayUpdateCallback = { _, _, _, _ in }
                self.displayCallback = callback
                ios_registerDisplayUpdateCallback(callback)
            }

            private func setupClipboardCallbacks() {
                vm_setClipboardCallbacks(
                    {
                        free(gClipboardBuffer)
                        gClipboardBuffer = nil
                        var text: String?
                        if Thread.isMainThread {
                            text = UIPasteboard.general.string
                        } else {
                            let sema = DispatchSemaphore(value: 0)
                            DispatchQueue.main.async {
                                text = UIPasteboard.general.string
                                sema.signal()
                            }
                            sema.wait()
                        }
                        guard let str = text, !str.isEmpty else { return nil }
                        gClipboardBuffer = strdup(str)
                        return UnsafePointer(gClipboardBuffer!)
                    },
                    { cString in
                        guard let cString = cString else { return }
                        let string = String(cString: cString)
                        if Thread.isMainThread {
                            UIPasteboard.general.string = string
                        } else {
                            DispatchQueue.main.async {
                                UIPasteboard.general.string = string
                            }
                        }
                    }
                )
            }

            private func setupTextInputCallback() {
                vm_setTextInputCallback { active in
                    DispatchQueue.main.async {
                        #if targetEnvironment(macCatalyst)
                        guard let view = gPharoMTKView else { return }
                        if active { view.becomeFirstResponder() }
                        else { view.resignFirstResponder() }
                        #endif
                    }
                }
            }

            func loadImage(at path: String) -> Bool {
                guard FileManager.default.fileExists(atPath: path) else {
                    errorMessage = "Image file not found: \\(path)"
                    return false
                }
                self.imagePath = path
                return true
            }

            func start() {
                guard let imagePath = imagePath else {
                    errorMessage = "No image loaded"
                    return
                }
                guard !isRunning else { return }
                errorMessage = nil

                var parameters = VMParameters()
                vm_parameters_init(&parameters)
                parameters.imageFileName = strdup(imagePath)
                parameters.isInteractiveSession = true
                parameters.isWorker = false
                let totalRAM = ProcessInfo.processInfo.physicalMemory
                let maxHeap: UInt64 = min(2 * 1024 * 1024 * 1024, totalRAM / 2)
                parameters.maxOldSpaceSize = Int64(maxHeap)
                parameters.edenSize = 10 * 1024 * 1024
                parameters.maxCodeSize = 0

                let imageDir = (imagePath as NSString).deletingLastPathComponent
                FileManager.default.changeCurrentDirectoryPath(imageDir)

                let initResult = vm_init(&parameters)
                if initResult != 0 {
                    isInitialized = true
                    isRunning = true
                    vm_run()

                    DispatchQueue.global(qos: .utility).async { [weak self] in
                        while vm_isRunning() { Thread.sleep(forTimeInterval: 0.1) }
                        DispatchQueue.main.async {
                            self?.handleVMExit()
                        }
                    }
                } else {
                    errorMessage = "Failed to initialize VM"
                }
                vm_parameters_destroy(&parameters)
            }

            func getDisplayBufferInfo() -> (pixels: UnsafeMutablePointer<UInt32>?, width: Int, height: Int, size: Int) {
                var info = IOSDisplayBufferInfo()
                ios_getDisplayBufferInfo(&info)
                return (info.pixels, Int(info.width), Int(info.height), Int(info.size))
            }

            func setDisplaySize(width: Int, height: Int) {
                ios_setDisplaySize(Int32(width), Int32(height))
            }

            func sendMouseMoved(to point: CGPoint, modifiers: Int = 0) {
                vm_postMouseEvent(0, Int32(point.x), Int32(point.y), 0, Int32(modifiers))
            }

            func sendTouchDown(at point: CGPoint, buttons: Int = 4, modifiers: Int = 0) {
                vm_postMouseEvent(1, Int32(point.x), Int32(point.y), Int32(buttons), Int32(modifiers))
            }

            func sendTouchMoved(to point: CGPoint, buttons: Int = 4, modifiers: Int = 0) {
                vm_postMouseEvent(0, Int32(point.x), Int32(point.y), Int32(buttons), Int32(modifiers))
            }

            func sendTouchUp(at point: CGPoint, buttons: Int = 4, modifiers: Int = 0) {
                vm_postMouseEvent(2, Int32(point.x), Int32(point.y), Int32(buttons), Int32(modifiers))
            }

            func sendScrollEvent(at point: CGPoint, deltaX: Int, deltaY: Int, modifiers: Int = 0) {
                vm_postScrollEvent(Int32(point.x), Int32(point.y),
                                   Int32(deltaX), Int32(deltaY), Int32(modifiers))
            }

            private func handleVMExit() {
                guard isRunning else { return }
                vm_stop()
                isRunning = false
                isInitialized = false
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { exit(0) }
            }

            func stop() {
                guard isRunning else { return }
                vm_stop()
                isRunning = false
                isInitialized = false
            }
        }
        """
    }

    static func pharoCanvasViewSwift() -> String {
        """
        import SwiftUI
        import MetalKit

        weak var gPharoMTKView: PharoMTKView?

        class PharoMTKView: MTKView {
            weak var bridge: PharoBridge?

            override init(frame frameRect: CGRect, device: MTLDevice?) {
                super.init(frame: frameRect, device: device)
                isUserInteractionEnabled = true
                isMultipleTouchEnabled = true
            }
            required init(coder: NSCoder) { super.init(coder: coder) }

            override var canBecomeFirstResponder: Bool { true }

            override func didMoveToWindow() {
                super.didMoveToWindow()
                #if targetEnvironment(macCatalyst)
                if window != nil {
                    DispatchQueue.main.async { self.becomeFirstResponder() }
                }
                #endif
            }

            private var currentButton: Int = 4

            override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
                guard let touch = touches.first, let bridge = bridge else { return }
                let point = touch.location(in: self)
                var buttons = buttonMaskToPharo(event)
                #if targetEnvironment(macCatalyst)
                // Ctrl+click = right click
                if event?.modifierFlags.contains(.control) == true { buttons = 2 }
                #endif
                currentButton = buttons
                bridge.sendMouseMoved(to: point)
                bridge.sendTouchDown(at: point, buttons: buttons)
            }

            override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
                guard let touch = touches.first, let bridge = bridge else { return }
                bridge.sendTouchMoved(to: touch.location(in: self), buttons: currentButton)
            }

            override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
                guard let touch = touches.first, let bridge = bridge else { return }
                bridge.sendTouchUp(at: touch.location(in: self), buttons: currentButton)
            }

            override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
                guard let touch = touches.first, let bridge = bridge else { return }
                bridge.sendTouchUp(at: touch.location(in: self), buttons: currentButton)
            }

            override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
                for press in presses {
                    guard let key = press.key else { continue }
                    postKeyDown(key)
                }
            }

            override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
                for press in presses {
                    guard let key = press.key else { continue }
                    postKeyUp(key)
                }
            }

            func postKeyDown(_ key: UIKey) {
                let mods = Int32(modifierFlagsToPharo(key.modifierFlags))
                if let char = key.characters.first, let scalar = char.unicodeScalars.first, scalar.value > 0 {
                    let code = Int32(scalar.value)
                    vm_postKeyEvent(0, code, 0, mods)
                    vm_postKeyEvent(2, code, 0, mods)
                } else {
                    let code = specialKeyCharCode(key.keyCode)
                    if code > 0 {
                        vm_postKeyEvent(0, code, 0, mods)
                        vm_postKeyEvent(2, code, 0, mods)
                    }
                }
            }

            func postKeyUp(_ key: UIKey) {
                let mods = Int32(modifierFlagsToPharo(key.modifierFlags))
                if let char = key.characters.first, let scalar = char.unicodeScalars.first, scalar.value > 0 {
                    vm_postKeyEvent(1, Int32(scalar.value), 0, mods)
                } else {
                    let code = specialKeyCharCode(key.keyCode)
                    if code > 0 { vm_postKeyEvent(1, code, 0, mods) }
                }
            }

            func modifierFlagsToPharo(_ flags: UIKeyModifierFlags) -> Int {
                var mods = 0
                if flags.contains(.shift) { mods |= 1 }
                if flags.contains(.control) { mods |= 2 }
                if flags.contains(.alternate) { mods |= 4 }
                if flags.contains(.command) { mods |= 8 }
                return mods
            }

            func specialKeyCharCode(_ keyCode: UIKeyboardHIDUsage) -> Int32 {
                switch keyCode {
                case .keyboardReturnOrEnter: return 13
                case .keyboardEscape: return 27
                case .keyboardDeleteOrBackspace: return 8
                case .keyboardTab: return 9
                case .keyboardDeleteForward: return 127
                case .keyboardUpArrow: return 30
                case .keyboardDownArrow: return 31
                case .keyboardLeftArrow: return 28
                case .keyboardRightArrow: return 29
                case .keyboardHome: return 1
                case .keyboardEnd: return 4
                case .keyboardPageUp: return 11
                case .keyboardPageDown: return 12
                default: return 0
                }
            }

            func buttonMaskToPharo(_ event: UIEvent?) -> Int {
                guard let event = event else { return 4 }
                #if targetEnvironment(macCatalyst)
                if #available(macCatalyst 13.4, *) {
                    let mask = event.buttonMask
                    if mask.contains(.secondary) { return 2 }
                    if mask.rawValue & 0x4 != 0 { return 1 }
                }
                #endif
                if event.modifierFlags.contains(.control) { return 2 }
                return 4
            }
        }

        class PharoCanvasViewController: UIViewController {
            var mtkView: PharoMTKView!
            var renderer: MetalRenderer?
            weak var bridge: PharoBridge?

            override func loadView() {
                view = UIView()
                #if targetEnvironment(macCatalyst)
                view.backgroundColor = .white
                #else
                view.backgroundColor = .black
                #endif
            }

            override func viewDidLoad() {
                super.viewDidLoad()
                mtkView = PharoMTKView()
                mtkView.bridge = bridge
                gPharoMTKView = mtkView
                mtkView.translatesAutoresizingMaskIntoConstraints = false
                mtkView.isPaused = false
                mtkView.enableSetNeedsDisplay = false
                mtkView.preferredFramesPerSecond = 30
                mtkView.backgroundColor = UIColor(white: 0.92, alpha: 1.0)
                if let bridge = bridge {
                    renderer = MetalRenderer(metalView: mtkView, bridge: bridge)
                }
                view.addSubview(mtkView)
                #if targetEnvironment(macCatalyst)
                NSLayoutConstraint.activate([
                    mtkView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
                    mtkView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
                    mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                    mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
                ])
                #else
                NSLayoutConstraint.activate([
                    mtkView.topAnchor.constraint(equalTo: view.topAnchor),
                    mtkView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
                    mtkView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                    mtkView.trailingAnchor.constraint(equalTo: view.trailingAnchor)
                ])
                #endif
                setupGestureRecognizers()
                #if targetEnvironment(macCatalyst)
                let quitCommand = UIKeyCommand(input: "q", modifierFlags: .command,
                                               action: #selector(handleQuit(_:)))
                quitCommand.title = "Quit"
                addKeyCommand(quitCommand)
                #endif
            }

            #if targetEnvironment(macCatalyst)
            @objc func handleQuit(_ sender: Any?) {
                PharoBridge.shared.stop()
                exit(0)
            }
            #endif

            override var canBecomeFirstResponder: Bool {
                #if targetEnvironment(macCatalyst)
                return false
                #else
                return true
                #endif
            }

            override func viewDidAppear(_ animated: Bool) {
                super.viewDidAppear(animated)
                #if targetEnvironment(macCatalyst)
                mtkView.becomeFirstResponder()
                #else
                becomeFirstResponder()
                #endif
            }

            #if !targetEnvironment(macCatalyst)
            override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
                for press in presses {
                    guard let key = press.key else { continue }
                    mtkView.postKeyDown(key)
                }
            }

            override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
                for press in presses {
                    guard let key = press.key else { continue }
                    mtkView.postKeyUp(key)
                }
            }
            #endif

            private func setupGestureRecognizers() {
                let targetView = mtkView as UIView

                #if targetEnvironment(macCatalyst)
                let hoverGesture = UIHoverGestureRecognizer(target: self, action: #selector(handleHover(_:)))
                hoverGesture.cancelsTouchesInView = false
                targetView.addGestureRecognizer(hoverGesture)

                let contextMenuInteraction = UIContextMenuInteraction(delegate: self)
                targetView.addInteraction(contextMenuInteraction)

                let scrollGesture = UIPanGestureRecognizer(target: self, action: #selector(handleScroll(_:)))
                scrollGesture.minimumNumberOfTouches = 2
                scrollGesture.maximumNumberOfTouches = 2
                scrollGesture.allowedScrollTypesMask = .continuous
                targetView.addGestureRecognizer(scrollGesture)
                #else
                let longPress = UILongPressGestureRecognizer(target: self, action: #selector(handleLongPress(_:)))
                longPress.minimumPressDuration = 0.5
                longPress.cancelsTouchesInView = true
                targetView.addGestureRecognizer(longPress)

                let twoFingerPan = UIPanGestureRecognizer(target: self, action: #selector(handleTwoFingerPan(_:)))
                twoFingerPan.minimumNumberOfTouches = 2
                twoFingerPan.maximumNumberOfTouches = 2
                twoFingerPan.cancelsTouchesInView = true
                targetView.addGestureRecognizer(twoFingerPan)

                let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(handleTwoFingerTap(_:)))
                twoFingerTap.numberOfTouchesRequired = 2
                twoFingerTap.require(toFail: twoFingerPan)
                targetView.addGestureRecognizer(twoFingerTap)
                #endif
            }

            #if targetEnvironment(macCatalyst)
            @objc func handleHover(_ gesture: UIHoverGestureRecognizer) {
                guard let bridge = bridge else { return }
                let point = gesture.location(in: mtkView)
                if gesture.state == .began || gesture.state == .changed {
                    bridge.sendMouseMoved(to: point)
                }
            }

            @objc func handleScroll(_ gesture: UIPanGestureRecognizer) {
                guard let bridge = bridge else { return }
                let point = gesture.location(in: mtkView)
                let translation = gesture.translation(in: mtkView)
                if gesture.state == .began || gesture.state == .changed {
                    let dx = Int(translation.x)
                    let dy = Int(-translation.y)
                    if dx != 0 || dy != 0 {
                        bridge.sendScrollEvent(at: point, deltaX: dx, deltaY: dy)
                    }
                    gesture.setTranslation(.zero, in: mtkView)
                }
            }
            #endif

            @objc func handleLongPress(_ gesture: UILongPressGestureRecognizer) {
                guard let bridge = bridge else { return }
                let point = gesture.location(in: mtkView)
                switch gesture.state {
                case .began:
                    bridge.sendTouchUp(at: point, buttons: 4)
                    bridge.sendMouseMoved(to: point)
                    bridge.sendTouchDown(at: point, buttons: 2)
                case .changed:
                    bridge.sendTouchMoved(to: point, buttons: 2)
                case .ended, .cancelled:
                    bridge.sendTouchUp(at: point, buttons: 2)
                default: break
                }
            }

            @objc func handleTwoFingerTap(_ gesture: UITapGestureRecognizer) {
                guard let bridge = bridge else { return }
                let point = gesture.location(in: mtkView)
                bridge.sendMouseMoved(to: point)
                bridge.sendTouchDown(at: point, buttons: 2)
                bridge.sendTouchUp(at: point, buttons: 2)
            }

            @objc func handleTwoFingerPan(_ gesture: UIPanGestureRecognizer) {
                guard let bridge = bridge else { return }
                let point = gesture.location(in: mtkView)
                let translation = gesture.translation(in: mtkView)
                if gesture.state == .began || gesture.state == .changed {
                    let dx = Int(translation.x)
                    let dy = Int(-translation.y)
                    if dx != 0 || dy != 0 {
                        bridge.sendScrollEvent(at: point, deltaX: dx, deltaY: dy)
                    }
                    gesture.setTranslation(.zero, in: mtkView)
                }
            }
        }

        #if targetEnvironment(macCatalyst)
        extension PharoCanvasViewController: UIContextMenuInteractionDelegate {
            func contextMenuInteraction(_ interaction: UIContextMenuInteraction,
                                        configurationForMenuAtLocation location: CGPoint) -> UIContextMenuConfiguration? {
                if let bridge = bridge {
                    bridge.sendMouseMoved(to: location)
                    bridge.sendTouchDown(at: location, buttons: 2)
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        bridge.sendTouchUp(at: location, buttons: 2)
                    }
                }
                return nil
            }
        }
        #endif

        struct PharoCanvasView: UIViewControllerRepresentable {
            @ObservedObject var bridge: PharoBridge

            func makeUIViewController(context: Context) -> PharoCanvasViewController {
                let vc = PharoCanvasViewController()
                vc.bridge = bridge
                return vc
            }

            func updateUIViewController(_ vc: PharoCanvasViewController, context: Context) {}
        }
        """
    }

    static func metalRendererSwift() -> String {
        """
        import Metal
        import MetalKit
        import simd

        @MainActor
        class MetalRenderer: NSObject, MTKViewDelegate {
            private let device: MTLDevice
            private let commandQueue: MTLCommandQueue
            private let pipelineState: MTLRenderPipelineState
            private var displayTexture: MTLTexture?
            private var textureWidth: Int = 0
            private var textureHeight: Int = 0
            private weak var bridge: PharoBridge?

            init?(metalView: MTKView, bridge: PharoBridge) {
                guard let device = MTLCreateSystemDefaultDevice(),
                      let commandQueue = device.makeCommandQueue() else { return nil }
                self.device = device
                self.commandQueue = commandQueue
                self.bridge = bridge
                metalView.device = device
                metalView.colorPixelFormat = .bgra8Unorm
                metalView.clearColor = MTLClearColor(red: 0.92, green: 0.92, blue: 0.92, alpha: 1.0)
                guard let library = device.makeDefaultLibrary(),
                      let vertexFn = library.makeFunction(name: "vertexShader"),
                      let fragmentFn = library.makeFunction(name: "fragmentShader") else { return nil }
                let desc = MTLRenderPipelineDescriptor()
                desc.vertexFunction = vertexFn
                desc.fragmentFunction = fragmentFn
                desc.colorAttachments[0].pixelFormat = metalView.colorPixelFormat
                do { self.pipelineState = try device.makeRenderPipelineState(descriptor: desc) }
                catch { return nil }
                super.init()
                metalView.delegate = self
                if let metalLayer = metalView.layer as? CAMetalLayer {
                    metalLayer.framebufferOnly = true
                }
            }

            func updateDisplayTexture() {
                guard let bridge = bridge else { return }
                let (pixels, width, height, _) = bridge.getDisplayBufferInfo()
                guard let bits = pixels, width > 0, height > 0 else { return }
                if displayTexture == nil || textureWidth != width || textureHeight != height {
                    let desc = MTLTextureDescriptor.texture2DDescriptor(
                        pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
                    desc.usage = [.shaderRead]
                    desc.storageMode = .shared
                    displayTexture = device.makeTexture(descriptor: desc)
                    textureWidth = width
                    textureHeight = height
                }
                guard let texture = displayTexture else { return }
                texture.replace(
                    region: MTLRegion(origin: MTLOrigin(x: 0, y: 0, z: 0),
                                     size: MTLSize(width: width, height: height, depth: 1)),
                    mipmapLevel: 0, withBytes: bits, bytesPerRow: width * 4)
            }

            func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
                let w = Int(view.bounds.size.width)
                let h = Int(view.bounds.size.height)
                bridge?.setDisplaySize(width: w, height: h)
            }

            func draw(in view: MTKView) {
                guard ffi_isSDLRenderingActive() else {
                    guard let drawable = view.currentDrawable,
                          let rpd = view.currentRenderPassDescriptor,
                          let cb = commandQueue.makeCommandBuffer(),
                          let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
                    enc.endEncoding()
                    cb.present(drawable)
                    cb.commit()
                    return
                }
                updateDisplayTexture()
                guard let texture = displayTexture,
                      let drawable = view.currentDrawable,
                      let rpd = view.currentRenderPassDescriptor,
                      let cb = commandQueue.makeCommandBuffer(),
                      let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }
                enc.setRenderPipelineState(pipelineState)
                enc.setFragmentTexture(texture, index: 0)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                enc.endEncoding()
                cb.present(drawable)
                cb.commit()
            }
        }
        """
    }

    static func shadersMetal() -> String {
        """
        #include <metal_stdlib>
        using namespace metal;

        struct VertexOut {
            float4 position [[position]];
            float2 texCoord;
        };

        vertex VertexOut vertexShader(uint vertexID [[vertex_id]]) {
            float2 positions[] = {
                float2(-1.0, -1.0), float2( 1.0, -1.0),
                float2(-1.0,  1.0), float2( 1.0,  1.0)
            };
            float2 texCoords[] = {
                float2(0.0, 1.0), float2(1.0, 1.0),
                float2(0.0, 0.0), float2(1.0, 0.0)
            };
            VertexOut out;
            out.position = float4(positions[vertexID], 0.0, 1.0);
            out.texCoord = texCoords[vertexID];
            return out;
        }

        fragment float4 fragmentShader(VertexOut in [[stage_in]],
                                       texture2d<float> displayTexture [[texture(0)]]) {
            constexpr sampler textureSampler(mag_filter::nearest, min_filter::nearest,
                                             address::clamp_to_edge);
            float4 color = displayTexture.sample(textureSampler, in.texCoord);
            color.a = 1.0;
            return color;
        }
        """
    }

    // MARK: - Headers

    static func bridgingHeader() -> String {
        """
        #ifndef BridgingHeader_h
        #define BridgingHeader_h

        #include <stdint.h>
        #include <stdbool.h>
        #include "../Headers/VMParameters.h"

        int vm_init(VMParameters* parameters);
        void vm_run(void);
        bool vm_isRunning(void);
        void vm_stop(void);
        void vm_parameters_init(VMParameters* parameters);
        void vm_parameters_destroy(VMParameters* parameters);

        typedef void (*IOSDisplayUpdateCallback)(int x, int y, int width, int height);
        void ios_registerDisplayUpdateCallback(IOSDisplayUpdateCallback callback);
        void ios_setDisplaySize(int width, int height);

        typedef struct {
            uint32_t* pixels;
            int width;
            int height;
            size_t size;
        } IOSDisplayBufferInfo;

        void ios_getDisplayBufferInfo(IOSDisplayBufferInfo* info);

        void vm_postMouseEvent(int type, int x, int y, int buttons, int modifiers);
        void vm_postKeyEvent(int type, int charCode, int keyCode, int modifiers);
        void vm_postScrollEvent(int x, int y, int deltaX, int deltaY, int modifiers);

        typedef const char* (*ClipboardGetFunc)(void);
        typedef void (*ClipboardSetFunc)(const char* text);
        void vm_setClipboardCallbacks(ClipboardGetFunc getFunc, ClipboardSetFunc setFunc);

        typedef void (*TextInputFunc)(bool active);
        void vm_setTextInputCallback(TextInputFunc func);

        bool ffi_isSDLRenderingActive(void);

        #include "../Headers/MotionData.h"

        int iosIsIPad(void);
        const char* iosGetDeviceModel(void);
        const char* iosGetSystemVersion(void);
        int iosOpenURL(const char* urlString);
        void iosShowAlert(const char* title, const char* message);
        void iosHapticFeedback(int style);

        #endif
        """
    }

    static func vmParametersHeader() -> String {
        """
        #ifndef VMParameters_h
        #define VMParameters_h

        #include <stdint.h>
        #include <stdbool.h>

        #ifdef __cplusplus
        extern "C" {
        #endif

        typedef struct VMParameterVector_ {
            uint32_t count;
            const char** parameters;
        } VMParameterVector;

        typedef struct VMParameters_ {
            char* imageFileName;
            bool isDefaultImage;
            bool defaultImageFound;
            bool isInteractiveSession;
            bool isWorker;
            int maxStackFramesToPrint;
            long long maxOldSpaceSize;
            long long maxCodeSize;
            long long edenSize;
            long long minPermSpaceSize;
            long long maxSlotsForNewSpaceAlloc;
            int processArgc;
            const char** processArgv;
            const char** environmentVector;
            bool avoidSearchingSegmentsWithPinnedObjects;
            VMParameterVector vmParameters;
            VMParameterVector imageParameters;
        } VMParameters;

        #ifdef __cplusplus
        }
        #endif

        #endif
        """
    }

    static func motionDataHeader() -> String {
        """
        #ifndef MotionData_h
        #define MotionData_h

        #include <stdint.h>

        #ifdef __cplusplus
        extern "C" {
        #endif

        #define MOTION_HAS_ACCELEROMETER  (1 << 0)
        #define MOTION_HAS_GYROSCOPE      (1 << 1)
        #define MOTION_HAS_MAGNETOMETER   (1 << 2)
        #define MOTION_HAS_DEVICE_MOTION  (1 << 3)

        typedef struct {
            double accelerometerX, accelerometerY, accelerometerZ;
            double gyroX, gyroY, gyroZ;
            double magnetometerX, magnetometerY, magnetometerZ;
            double roll, pitch, yaw;
            double timestamp;
            int32_t available;
            int32_t active;
        } MotionData;

        void motion_update(const MotionData* data);
        void motion_getData(MotionData* out);
        int motion_isAvailable(void);
        void motion_requestStart(void);
        void motion_requestStop(void);
        int motion_startRequested(void);
        int motion_stopRequested(void);

        #ifdef __cplusplus
        }
        #endif

        #endif
        """
    }

    // MARK: - Startup Script

    static func startupScript(kioskMode: Bool, stripImage: Bool) -> String {
        var script = """
        "Auto-generated startup script for exported app"
        "Fix: doc browser uses anonymous GitHub API to avoid IceTokenCredentials crash"
        (Smalltalk hasClassNamed: #MicGitHubRessourceReference) ifTrue: [
          MicGitHubRessourceReference compile: 'githubApi
            ^ MicGitHubAPI new beAnonymous'].
        """

        if stripImage {
            script += """

            "Strip development tools to reduce image size"
            | packagesToRemove |
            packagesToRemove := #(
              'Iceberg' 'Iceberg-*' 'LibGit-*'
              'Calypso-*'
              'NewTools-*'
              'Spec2-Tools*' 'Spec2-Backend-Tests*'
              'GT-*' 'Tool-*'
              '*-Tests' '*-Tests-*' '*Test' '*Tests*'
              'MonticelloMocks' 'ReleaseTests'
              'SUnit-*'
              'Reflectivity-Tools*'
              'Debugger-*' 'DebugPoints*'
              'HelpSystem-*'
              'Metacello-*'
              'Ring-Definitions-Tests*'
              'DrTests*'
            ).
            (RPackageOrganizer default packages
              select: [ :pkg |
                packagesToRemove anySatisfy: [ :pattern |
                  pattern includesSubstring: '*'
                    ifTrue: [ pkg name matchPattern: pattern ]
                    ifFalse: [ pkg name = pattern ] ] ])
              do: [ :pkg |
                [ pkg removeFromSystem ]
                  on: Error do: [ :e | "skip packages with removal errors" ] ].
            Smalltalk cleanUp: true except: {} confirming: false.
            3 timesRepeat: [ Smalltalk garbageCollect ].
            """
        }

        if kioskMode {
            script += """

            "Kiosk mode: hide development chrome"
            TaskbarMorph showTaskbar: false.
            MenubarMorph showMenubar: false.
            WorldState desktopMenuPragmaKeyword: ''.
            """
            if !stripImage {
                script += """

                "Clean caches"
                Smalltalk cleanUp: true except: {} confirming: false.
                3 timesRepeat: [ Smalltalk garbageCollect ].
                """
            }
        }

        return script
    }

    // MARK: - Config Files

    static func infoPlist(appName: String, imageFileName: String) -> String {
        """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>CFBundleDevelopmentRegion</key>
            <string>en</string>
            <key>CFBundleExecutable</key>
            <string>$(EXECUTABLE_NAME)</string>
            <key>CFBundleIdentifier</key>
            <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
            <key>CFBundleName</key>
            <string>\(appName)</string>
            <key>CFBundlePackageType</key>
            <string>APPL</string>
            <key>CFBundleShortVersionString</key>
            <string>1.0</string>
            <key>CFBundleVersion</key>
            <string>1</string>
            <key>ITSAppUsesNonExemptEncryption</key>
            <false/>
            <key>NSAppTransportSecurity</key>
            <dict>
                <key>NSAllowsArbitraryLoads</key>
                <true/>
            </dict>
            <key>UIUserInterfaceStyle</key>
            <string>Light</string>
            <key>UILaunchStoryboardName</key>
            <string></string>
            <key>PharoImageFile</key>
            <string>\(imageFileName)</string>
            <key>PharoMaxOldSpaceSize</key>
            <integer>2147483648</integer>
            <key>PharoEdenSize</key>
            <integer>10485760</integer>
        </dict>
        </plist>
        """
    }

    static func entitlements() -> String {
        """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>com.apple.security.app-sandbox</key>
            <true/>
            <key>com.apple.security.network.client</key>
            <true/>
            <key>com.apple.security.network.server</key>
            <true/>
        </dict>
        </plist>
        """
    }

    // MARK: - Watch Companion App

    static func watchAppSwift(appName: String) -> String {
        """
        import SwiftUI

        @main
        struct \(appName)WatchApp: App {
            var body: some Scene {
                WindowGroup {
                    ContentView()
                }
            }
        }
        """
    }

    static func watchContentViewSwift(appName: String) -> String {
        """
        import SwiftUI

        struct ContentView: View {
            var body: some View {
                VStack(spacing: 12) {
                    Image(systemName: "apps.iphone")
                        .font(.system(size: 40))
                        .foregroundColor(.accentColor)
                    Text("\(appName)")
                        .font(.headline)
                    Text("Open on iPhone or iPad")
                        .font(.caption2)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                }
                .padding()
            }
        }
        """
    }

    static func watchInfoPlist(appName: String) -> String {
        """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
            <key>CFBundleDevelopmentRegion</key>
            <string>en</string>
            <key>CFBundleExecutable</key>
            <string>$(EXECUTABLE_NAME)</string>
            <key>CFBundleIdentifier</key>
            <string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
            <key>CFBundleName</key>
            <string>\(appName) Watch</string>
            <key>CFBundlePackageType</key>
            <string>APPL</string>
            <key>CFBundleShortVersionString</key>
            <string>1.0</string>
            <key>CFBundleVersion</key>
            <string>1</string>
            <key>WKApplication</key>
            <true/>
        </dict>
        </plist>
        """
    }

    static func watchAppIconJSON() -> String {
        """
        {
          "images" : [
            {
              "idiom" : "universal",
              "platform" : "watchos",
              "size" : "1024x1024"
            }
          ],
          "info" : {
            "author" : "xcode",
            "version" : 1
          }
        }
        """
    }

    static func watchAppIconJSONWithImage(filename: String) -> String {
        """
        {
          "images" : [
            {
              "filename" : "\(filename)",
              "idiom" : "universal",
              "platform" : "watchos",
              "size" : "1024x1024"
            }
          ],
          "info" : {
            "author" : "xcode",
            "version" : 1
          }
        }
        """
    }

    // MARK: - Assets

    static func assetsContentsJSON() -> String {
        """
        {
          "info" : {
            "author" : "xcode",
            "version" : 1
          }
        }
        """
    }

    static func colorSetJSON() -> String {
        """
        {
          "colors" : [
            {
              "idiom" : "universal"
            }
          ],
          "info" : {
            "author" : "xcode",
            "version" : 1
          }
        }
        """
    }

    static func appIconJSONWithImage(filename: String) -> String {
        """
        {
          "images" : [
            {
              "filename" : "\(filename)",
              "idiom" : "universal",
              "platform" : "ios",
              "size" : "1024x1024"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "16x16"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "16x16"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "32x32"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "32x32"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "128x128"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "128x128"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "256x256"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "256x256"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "512x512"
            },
            {
              "filename" : "\(filename)",
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "512x512"
            }
          ],
          "info" : {
            "author" : "xcode",
            "version" : 1
          }
        }
        """
    }

    static func appIconJSON() -> String {
        """
        {
          "images" : [
            {
              "idiom" : "universal",
              "platform" : "ios",
              "size" : "1024x1024"
            },
            {
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "16x16"
            },
            {
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "16x16"
            },
            {
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "32x32"
            },
            {
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "32x32"
            },
            {
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "128x128"
            },
            {
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "128x128"
            },
            {
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "256x256"
            },
            {
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "256x256"
            },
            {
              "idiom" : "mac",
              "scale" : "1x",
              "size" : "512x512"
            },
            {
              "idiom" : "mac",
              "scale" : "2x",
              "size" : "512x512"
            }
          ],
          "info" : {
            "author" : "xcode",
            "version" : 1
          }
        }
        """
    }

    // MARK: - Build Script

    static func buildScript(appName: String, macOS: Bool, iOS: Bool) -> String {
        var script = """
        #!/bin/bash
        # Build script for \(appName)
        # Generated by Pharo Smalltalk — Export as App
        set -e

        PROJECT="\(appName).xcodeproj"
        SCHEME="\(appName)"

        """

        if macOS {
            script += """

            echo "Building for macOS (Mac Catalyst)..."
            xcodebuild \\
                -project "$PROJECT" \\
                -scheme "$SCHEME" \\
                -destination 'platform=macOS,variant=Mac Catalyst' \\
                -configuration Release \\
                build

            echo "macOS build complete."

            """
        }

        if iOS {
            script += """

            echo "Building for iOS..."
            xcodebuild \\
                -project "$PROJECT" \\
                -scheme "$SCHEME" \\
                -destination 'generic/platform=iOS' \\
                -configuration Release \\
                build

            echo "iOS build complete."

            """
        }

        script += """

        echo "Done. Find the build products in DerivedData."
        """

        return script
    }
}
