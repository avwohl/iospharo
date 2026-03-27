/*
 * PharoBridge.swift
 *
 * Swift wrapper for the Pharo VM C API.
 * Manages VM lifecycle and bridges events between SwiftUI and the VM.
 */

import Foundation
import Combine
import UIKit

/// Buffer for clipboard text returned to C (freed on next call)
private var gClipboardBuffer: UnsafeMutablePointer<CChar>?

/// Main bridge between Swift and the Pharo VM
@MainActor
class PharoBridge: ObservableObject {

    /// Singleton instance
    static let shared = PharoBridge()

    /// Published state
    @Published var isRunning = false
    @Published var isInitialized = false
    @Published var errorMessage: String?

    /// Virtual Ctrl key toggle — when active, touches and keyboard events include Ctrl modifier
    @Published var ctrlModifierActive = false

    /// Virtual Cmd key toggle — when active, keyboard events include Cmd modifier (one-shot)
    @Published var cmdModifierActive = false

    /// Soft keyboard visibility toggle (iOS only)
    @Published var keyboardVisible = false

    /// True only when a docked (full-width, bottom-anchored) keyboard is confirmed
    /// showing via keyboardWillChangeFrame notification. The sidebar hides non-essential
    /// buttons only when this is true. Floating/undocked keyboards don't set this.
    @Published var keyboardDocked = false

    private var imagePath: String?

    private var displayCallback: IOSDisplayUpdateCallback?

    /// Core Motion manager — polls CMMotionManager and writes to shared C struct
    private let motionManager = CoreMotionManager()
    private var motionTimer: Timer?

    private init() {
        setupDisplayCallback()
        setupClipboardCallbacks()
        setupTextInputCallback()
    }

    // MARK: - Display Callback

    private func setupDisplayCallback() {
        // Register a no-op callback. The Metal renderer polls the front buffer
        // directly on every draw(in:) call, so no notification is needed.
        let callback: IOSDisplayUpdateCallback = { x, y, width, height in
            // No-op: MetalRenderer copies the front buffer every frame.
        }

        self.displayCallback = callback
        ios_registerDisplayUpdateCallback(callback)
    }

    // MARK: - Clipboard Callbacks

    private func setupClipboardCallbacks() {
        vm_setClipboardCallbacks(
            // ClipboardGetFunc: return pasteboard text as C string
            {
                free(gClipboardBuffer)
                gClipboardBuffer = nil

                // Read clipboard — always on main thread since UIPasteboard requires it.
                // Use DispatchQueue.main.async + semaphore instead of .sync to avoid
                // deadlocking if the main thread is waiting on the VM.
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
            // ClipboardSetFunc: set pasteboard text from C string
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

    // MARK: - Text Input Callbacks

    private func setupTextInputCallback() {
        vm_setTextInputCallback { active in
            DispatchQueue.main.async {
                #if targetEnvironment(macCatalyst)
                // Mac Catalyst: becomeFirstResponder captures hardware keyboard
                // without showing an on-screen keyboard, so always honor Pharo's request.
                guard let view = gPharoMTKView else { return }
                if active {
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
                #else
                // iOS: Don't let Pharo's SDL_StartTextInput show the soft keyboard.
                // The user controls the keyboard via the floating toolbar button.
                // Pharo calls SDL_StartTextInput aggressively (e.g. when refocusing
                // any morph), which would pop the keyboard at unwanted times.
                #endif
            }
        }
    }

    // MARK: - VM Lifecycle

    /// Load a Pharo image file
    func loadImage(at path: String) -> Bool {
        guard FileManager.default.fileExists(atPath: path) else {
            errorMessage = "Image file not found: \(path)"
            return false
        }

        self.imagePath = path
        return true
    }

    /// Start the VM with the loaded image
    func start() {
        guard let imagePath = imagePath else {
            errorMessage = "No image loaded"
            return
        }

        guard !isRunning else {
            errorMessage = "VM is already running"
            return
        }

        // Don't set isRunning yet - wait until VM is initialized
        // This prevents SwiftUI from showing canvas before VM is ready
        errorMessage = nil

        // Initialize VM synchronously on main thread
        var parameters = VMParameters()
        vm_parameters_init(&parameters)

        parameters.imageFileName = strdup(imagePath)
        parameters.isInteractiveSession = true
        parameters.isWorker = false

        // Memory: cap heap at half of physical RAM (iPhone 8 has only 2 GB
        // and iOS rejects a 2 GB mmap reservation on low-memory devices)
        let totalRAM = ProcessInfo.processInfo.physicalMemory
        let maxHeap: UInt64 = min(2 * 1024 * 1024 * 1024, totalRAM / 2)
        parameters.maxOldSpaceSize = Int64(maxHeap)
        parameters.edenSize = 10 * 1024 * 1024
        parameters.maxCodeSize = 0

        // Pre-set display size so Pharo creates the Welcome window at the
        // correct dimensions from the start. Without this, the default 1024x768
        // makes Pharo lay out for a large screen, then the resize to the actual
        // Metal view size cuts off content that doesn't auto-shrink.
        if let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
           let window = scene.windows.first {
            let bounds = window.bounds
            #if targetEnvironment(macCatalyst)
            // Mac Catalyst: no modifier strip, use window bounds directly
            let w = Int(bounds.width)
            let h = Int(bounds.height)
            #else
            // iOS: subtract the ModifierStrip width from the canvas dimension
            let stripWidth = UIDevice.current.userInterfaceIdiom == .pad ? 38 : 40
            let w = Int(bounds.width) - stripWidth
            let h = Int(bounds.height)
            #endif
            if w > 0 && h > 0 {
                ios_setDisplaySize(Int32(w), Int32(h))
                #if DEBUG
                fputs("[BRIDGE] pre-set display size: \(w)x\(h) (window=\(Int(bounds.width))x\(Int(bounds.height)))\n", stderr)
                #endif
            }
        }

        // Change working directory to image's directory so Pharo's
        // StartupPreferencesLoader finds startup.st alongside the image
        let imageDir = (imagePath as NSString).deletingLastPathComponent
        #if DEBUG
        fputs("[BRIDGE] imagePath=\(imagePath) imageDir=\(imageDir)\n", stderr)
        #endif
        FileManager.default.changeCurrentDirectoryPath(imageDir)

        // Write startup.st with image patches (loaded by StartupPreferencesLoader)
        // Skip if --image flag was used and a startup.st already exists (user-provided)
        let existingStartup = FileManager.default.fileExists(atPath: imageDir + "/startup.st")
        let isCLILaunch = ProcessInfo.processInfo.arguments.contains("--image")
        if isCLILaunch && existingStartup {
            #if DEBUG
            fputs("[BRIDGE] skipping writeStartupScript — --image flag with existing startup.st\n", stderr)
            #endif
        } else {
            Self.writeStartupScript(to: imageDir)
        }
        #if DEBUG
        fputs("[BRIDGE] after writeStartupScript, startup.st exists=\(FileManager.default.fileExists(atPath: imageDir + "/startup.st"))\n", stderr)
        #endif

        let initResult = vm_init(&parameters)

        if initResult != 0 {
            isInitialized = true
            isRunning = true
            #if DEBUG
            NSLog("[BRIDGE] VM initialized, starting interpreter on background thread")
            #endif

            // Poll Core Motion start/stop requests from the VM thread.
            // The timer runs on the main run loop at ~10 Hz.
            let mgr = motionManager
            motionTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { _ in
                mgr.pollRequests()
            }

            // Start the interpreter on a background thread.
            // This returns immediately, leaving the main thread free for
            // SwiftUI/UIKit event processing and Metal rendering.
            vm_run()

            // Monitor for VM exit (primitiveQuit sets running_ = false)
            DispatchQueue.global(qos: .utility).async { [weak self] in
                while vm_isRunning() {
                    Thread.sleep(forTimeInterval: 0.1)
                }
                // VM has exited — clean up on main thread
                DispatchQueue.main.async {
                    self?.handleVMExit()
                }
            }
        } else {
            errorMessage = "Failed to initialize VM"
        }

        vm_parameters_destroy(&parameters)
    }

    // MARK: - Image Patches

    /// Write startup.st with Pharo image patches.
    /// Pharo's StartupPreferencesLoader auto-loads startup.st from the
    /// working directory on every image startup.
    private static func writeStartupScript(to directory: String) {
        let script = """
        "Pharo image patches - auto-applied by Pharo Smalltalk VM"
        "Supports both Pharo 13 and Pharo 14 images"
        | isP14 |
        isP14 := SystemVersion current major >= 14.
        "Disable sub-pixel text rendering — our VM does not implement BitBlt rule 41"
        "Without this, FreeTypeSubPixelAntiAliasedGlyphRenderer crashes with nil data"
        (Smalltalk hasClassNamed: #FreeTypeSettings) ifTrue: [
          FreeTypeSettings current instVarNamed: 'bitBltSubPixelAvailable' put: false].
        "Fix: doc browser uses anonymous GitHub API to avoid IceTokenCredentials crash"
        (Smalltalk hasClassNamed: #MicGitHubRessourceReference) ifTrue: [
          MicGitHubRessourceReference compile: 'githubApi
            ^ MicGitHubAPI new beAnonymous'].
        "Fix: doc browser error handler uses messageText instead of message"
        (Smalltalk hasClassNamed: #MicDocumentBrowserModel) ifTrue: [
          MicDocumentBrowserModel compile: 'document
            resourceReference ifNil: [ ^ nil ].
            document ifNotNil: [ ^ document ].
            [ document := resourceReference loadMicrodown ]
              on: Error
              do: [ :error |
                document := Microdown parse: ''# Error
        '', error messageText ].
            ^ document'].
        "Robust childrenOf: that handles API errors"
        (Smalltalk hasClassNamed: #MicDocumentBrowserPresenter) ifTrue: [
          MicDocumentBrowserPresenter compile: 'childrenOf: aNode
            [ (aNode isKindOf: MicElement) ifTrue: [ ^ aNode subsections children ].
              aNode loadChildren
                ifNotEmpty: [ :children | ^ children sort: [:a :b |
                    (self displayStringOf: a) < (self displayStringOf: b)] ]
                ifEmpty: [
                  [ ^ self childrenOf: (MicSectionBlock fromRoot: aNode loadMicrodown) ]
                    on: Error do: [ ^ #() ]]
            ] on: Error do: [ ^ #() ]'].
        "Fix: menu shortcut symbols (U+2318 etc.) missing from embedded Source Sans Pro"
        "The embedded font is from 2012; Adobe added these glyphs in v2.040 (2018)."
        "Replace Unicode symbols with readable ASCII abbreviations."
        (Smalltalk hasClassNamed: #KMShortcutPrinter) ifTrue: [
          KMShortcutPrinter symbolTable
            at: #Cmd put: 'Cmd+';
            at: #Meta put: 'Cmd+';
            at: #Alt put: 'Opt+';
            at: #Ctrl put: 'Ctrl+';
            at: #Shift put: 'Shift+';
            at: #Enter put: 'Enter'].
        "Fix: bullet chars (U+2022) in learning docs show as ? — not in embedded Source Sans Pro"
        "Patch the bulletForLevel: renderer to use ASCII * and - instead of Unicode bullet."
        (Smalltalk hasClassNamed: #MicRichTextComposer) ifTrue: [
          MicRichTextComposer compile: 'bulletForLevel: level
            ^ (''*-'' at: (level - 1 \\\\ 2) + 1) asText'].
        "Fix: WarpBlt Smalltalk fallback drops alpha channel in mixPix:"
        "mixPix:sourceMap:destMap: averages R,G,B but ignores alpha (bits 24-31)."
        "This causes Color inspector swatch to be transparent (gray) instead of colored."
        (Smalltalk hasClassNamed: #WarpBlt) ifTrue: [
          WarpBlt compile: 'mixPix: pix sourceMap: sourceMap destMap: destMap
            | r g b a rgb nPix bitsPerColor d |
            nPix := pix size.
            r := 0. g := 0. b := 0. a := 0.
            1 to: nPix do: [ :i |
              rgb := sourceForm depth <= 8
                ifTrue: [ sourceMap at: (pix at: i) + 1 ]
                ifFalse: [ sourceForm depth = 32
                    ifTrue: [ pix at: i ]
                    ifFalse: [ self rgbMap: (pix at: i) from: 5 to: 8 ] ].
              a := a + ((rgb bitShift: -24) bitAnd: 255).
              r := r + ((rgb bitShift: -16) bitAnd: 255).
              g := g + ((rgb bitShift: -8) bitAnd: 255).
              b := b + ((rgb bitShift: 0) bitAnd: 255) ].
            destMap
              ifNil: [ bitsPerColor := 3.
                destForm depth = 16 ifTrue: [ bitsPerColor := 5 ].
                destForm depth = 32 ifTrue: [ bitsPerColor := 8 ] ]
              ifNotNil: [ destMap size = 512 ifTrue: [ bitsPerColor := 3 ].
                destMap size = 4096 ifTrue: [ bitsPerColor := 4 ].
                destMap size = 32768 ifTrue: [ bitsPerColor := 5 ] ].
            d := bitsPerColor - 8.
            rgb := ((a // nPix bitShift: d) bitShift: bitsPerColor * 3)
              + ((r // nPix bitShift: d) bitShift: bitsPerColor * 2)
              + ((g // nPix bitShift: d) bitShift: bitsPerColor)
              + ((b // nPix bitShift: d) bitShift: 0).
            ^ destMap ifNil: [ rgb ] ifNotNil: [ destMap at: rgb + 1 ]'].
        "Debug: patch fullDrawOn: to log errors to stderr AND to a file"
        Morph compile: 'fullDrawOn: aCanvas
            self visible ifFalse: [ ^ self ].
            (aCanvas isVisible: self fullBounds) ifFalse: [ ^ self ].
            (self hasProperty: #errorOnDraw) ifTrue: [ ^ self drawErrorOn: aCanvas ].
            [
                self hasDropShadow ifTrue: [ self drawDropShadowOn: aCanvas ].
                aCanvas roundCornersOf: self during: [
                    (aCanvas isVisible: self bounds) ifTrue: [ aCanvas drawMorph: self ].
                    self drawSubmorphsOn: aCanvas.
                    self drawDropHighlightOn: aCanvas.
                    self drawMouseDownHighlightOn: aCanvas ]
            ] on: Error do: [ :err |
                | errText stackText |
                errText := self class name , '': '' , err messageText.
                stackText := err signalerContext
                    ifNotNil: [ :ctx | String streamContents: [:s | ctx shortDebugStackOn: s] ]
                    ifNil: [ '''' ].
                Stdio stderr
                    nextPutAll: ''[DRAW-ERROR] '';
                    nextPutAll: errText; lf;
                    nextPutAll: stackText; lf;
                    flush.
                [ (FileSystem workingDirectory / ''draw_errors.txt'')
                    writeStreamDo: [ :f |
                        f setToEnd.
                        f nextPutAll: DateAndTime now printString; nextPutAll: '' '';
                          nextPutAll: errText; cr;
                          nextPutAll: stackText; cr; cr ] ]
                    on: Error do: [ :e | "ignore file errors" ].
                self setProperty: #errorOnDraw toValue: true.
                self setProperty: #drawError toValue: err freeze.
                self drawErrorOn: aCanvas ]'.
        "Debug: override drawErrorOn: to show error text visibly in the red box"
        Morph compile: 'drawErrorOn: aCanvas
            | stringBounds lineH |
            aCanvas
                frameAndFillRectangle: bounds
                fillColor: (Color red alpha: 0.3)
                borderWidth: 2
                borderColor: Color yellow.
            lineH := TextStyle defaultFont pixelSize * 1.2.
            stringBounds := bounds insetBy: 4.
            "Show class name"
            aCanvas drawString: self class name in: stringBounds.
            stringBounds := stringBounds top: stringBounds top + lineH.
            "Show error message and stack trace"
            self valueOfProperty: #drawError ifPresentDo: [ :error |
                | trace |
                aCanvas drawString: error messageText in: stringBounds.
                stringBounds := stringBounds top: stringBounds top + lineH.
                trace := String streamContents: [ :s |
                    error signalerContext shortDebugStackOn: s ].
                trace linesDo: [ :aString |
                    stringBounds top < (bounds bottom - 4) ifTrue: [
                        aCanvas drawString: aString in: stringBounds.
                        stringBounds := stringBounds top: stringBounds top + lineH ] ] ]'.
        "About window: add Pharo Smalltalk VM disclaimer and source link"
        "P13 uses SmalltalkImage>>systemInformationString; P14 inlines in StPharoSettings"
        isP14 ifFalse: [
          SmalltalkImage compile: 'systemInformationString
            | s |
            s := String streamContents: [ :stream |
                stream
                    nextPutAll: ''Pharo '';
                    nextPutAll: SystemVersion current dottedMajorMinorPatch; cr; cr;
                    nextPutAll: ''Running on Pharo Smalltalk — a community VM for iOS and macOS.''; cr;
                    nextPutAll: ''This is NOT the official Pharo VM.''; cr; cr;
                    nextPutAll: ''Source code: https://github.com/avwohl/iospharo''; cr; cr;
                    nextPutAll: ''Built from: '';
                    nextPutAll: SystemVersion current commitHash; cr;
                    nextPutAll: ''Last update: '';
                    nextPutAll: SystemVersion current date printString; cr; cr;
                    nextPutAll: Smalltalk license ].
            ^ s'].
        isP14 ifTrue: [
          (Smalltalk hasClassNamed: #StPharoSettings) ifTrue: [
            StPharoSettings class compile: 'openPharoAbout
              | about |
              about := String cr , ''Pharo '' , SystemVersion current dottedMajorMinorPatch , String cr,
                String cr,
                ''Running on Pharo Smalltalk — a community VM for iOS and macOS.'' , String cr,
                ''This is NOT the official Pharo VM.'' , String cr , String cr,
                ''Source code: https://github.com/avwohl/iospharo'' , String cr , String cr,
                ''Build information: '', SystemVersion current asString, String cr,
                SystemVersion current date asString , String cr, String cr,
                Smalltalk licenseString.
              SpInformDialog new
                title: ''About Pharo'';
                icon: (self iconNamed: #pharo);
                label: about;
                acceptLabel: ''Close'';
                openDialog']].
        "P14-only: fix nil renderer during early startup"
        "P14 starts MorphicRenderLoop before the SDL renderer is created."
        "outputExtent must call validate lazily to create the renderer on demand."
        isP14 ifTrue: [
          (Smalltalk hasClassNamed: #OSSDL2FormRenderer) ifTrue: [
            OSSDL2FormRenderer compile: 'outputExtent
              renderer ifNil: [ self validate ].
              renderer ifNil: [ ^ 0@0 ].
              ^ renderer outputExtent']].
        "Fix: prevent windows from opening under the Pharo menu bar"
        "Override openInWorld: to bump windows below the MenubarMorph"
        SystemWindow compile: 'openInWorld: aWorld
            super openInWorld: aWorld.
            aWorld submorphsDo: [ :m |
                (m class name = ''MenubarMorph'' and: [ self top < m bottom ])
                    ifTrue: [ self position: self position x @ m bottom ] ]'.
        "Fix: reposition any existing windows that overlap the menu bar"
        [
          | menuBarBottom |
          (Delay forMilliseconds: 500) wait.
          menuBarBottom := 0.
          World submorphsDo: [ :m |
              (m class name = 'MenubarMorph') ifTrue: [ menuBarBottom := m bottom ] ].
          menuBarBottom > 0 ifTrue: [
              World submorphsDo: [ :m |
                  (m isKindOf: SystemWindow) ifTrue: [
                      m top < menuBarBottom ifTrue: [
                          m position: m position x @ menuBarBottom ] ] ] ].
        ] fork.
        "Refresh stale doc browser windows from previous saved sessions"
        "The tree may be empty if the image was saved when SSL was broken."
        "P13 uses updateTree; P14 removed it — use updateSourcePresenter instead."
        (Smalltalk hasClassNamed: #MicDocumentBrowserPresenter) ifTrue: [
          [
            (Delay forMilliseconds: 3000) wait.
            MicDocumentBrowserPresenter allInstances do: [:each |
              isP14
                ifTrue:  [ [ each updateSourcePresenter ] on: Error do: [ "ignore" ] ]
                ifFalse: [ [ each updateTree ] on: Error do: [ "ignore" ] ] ].
          ] fork ].
        "P14: Start MorphicRenderLoop after patches are applied."
        "We pass -interactive (single dash) to avoid P14's early render loop start."
        "P14's isInteractive scans for --interactive (double dash), so single-dash"
        "does not trigger it. After all patches are applied, we start the loop here."
        isP14 ifTrue: [
          MorphicUIManager new activate.
          MorphicUIManager new spawnNewProcess].
        """
        let path = (directory as NSString).appendingPathComponent("startup.st")
        #if DEBUG
        NSLog("[BRIDGE] writeStartupScript: directory=%@ path=%@", directory, path)
        #endif
        do {
            try script.write(toFile: path, atomically: true, encoding: .utf8)
            #if DEBUG
            NSLog("[BRIDGE] startup.st written successfully (%d bytes)", script.count)
            #endif
        } catch {
            NSLog("[BRIDGE] ERROR writing startup.st: %@", error.localizedDescription)
        }
    }

    // MARK: - Display Access

    /// Get all display info atomically (prevents tearing during resize)
    func getDisplayBufferInfo() -> (pixels: UnsafeMutablePointer<UInt32>?, width: Int, height: Int, size: Int) {
        var info = IOSDisplayBufferInfo()
        ios_getDisplayBufferInfo(&info)
        return (info.pixels, Int(info.width), Int(info.height), Int(info.size))
    }

    /// Notify VM of view size change
    func setDisplaySize(width: Int, height: Int) {
        ios_setDisplaySize(Int32(width), Int32(height))
    }

    // MARK: - Touch Events
    // Mouse event types for vm_postMouseEvent: 0=move, 1=down, 2=up

    func sendTouchDown(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        vm_postMouseEvent(1, Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    func sendTouchMoved(to point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        vm_postMouseEvent(0, Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    func sendTouchUp(at point: CGPoint, buttons: Int = IOS_RED_BUTTON, modifiers: Int = 0) {
        vm_postMouseEvent(2, Int32(point.x), Int32(point.y),
                          Int32(buttons), Int32(modifiers))
    }

    func sendMouseMoved(to point: CGPoint, modifiers: Int = 0) {
        vm_postMouseEvent(0, Int32(point.x), Int32(point.y),
                          0, Int32(modifiers))
    }

    // MARK: - Keyboard Events
    // Key event types for vm_postKeyEvent: 0=down, 1=up, 2=stroke

    /// Send key down event
    func sendKeyDown(_ character: Character, modifiers: Int = 0) {
        guard let scalar = character.unicodeScalars.first else { return }
        vm_postKeyEvent(0, // type: down
                        Int32(scalar.value), 0, Int32(modifiers))
    }

    /// Send key up event
    func sendKeyUp(_ character: Character, modifiers: Int = 0) {
        guard let scalar = character.unicodeScalars.first else { return }
        vm_postKeyEvent(1, // type: up
                        Int32(scalar.value), 0, Int32(modifiers))
    }

    /// Send key typed (down + up)
    func sendKeyTyped(_ character: Character, modifiers: Int = 0) {
        sendKeyDown(character, modifiers: modifiers)
        sendKeyUp(character, modifiers: modifiers)
    }

    /// Send a full key shortcut sequence (down + stroke + up) for toolbar action buttons.
    /// Unlike sendKeyTyped which only sends down + up, this includes the stroke event
    /// that Pharo needs to process the character as text input with modifiers.
    func sendKeyShortcut(_ character: Character, modifiers: Int) {
        guard let scalar = character.unicodeScalars.first else { return }
        let code = Int32(scalar.value)
        let mods = Int32(modifiers)
        vm_postKeyEvent(0, code, 0, mods)  // down
        vm_postKeyEvent(2, code, 0, mods)  // stroke
        vm_postKeyEvent(1, code, 0, mods)  // up
    }

    /// Send a raw key down + up by integer key code, incorporating active modifier toggles.
    /// Clears modifier toggles after use (same behavior as PharoCanvasView keyboard input).
    func sendRawKey(_ charCode: Int32, keyCode: Int32 = 0, modifiers: Int32 = 0) {
        var mods = modifiers
        if ctrlModifierActive { mods |= Int32(IOS_CTRL_KEY) }
        if cmdModifierActive { mods |= Int32(IOS_CMD_KEY) }
        if mods != modifiers {
            DispatchQueue.main.async { [weak self] in
                self?.ctrlModifierActive = false
                self?.cmdModifierActive = false
            }
        }
        vm_postKeyEvent(0, charCode, keyCode, mods)  // down
        vm_postKeyEvent(1, charCode, keyCode, mods)  // up
    }

    /// Send string as key events
    func sendString(_ string: String, modifiers: Int = 0) {
        for char in string {
            sendKeyTyped(char, modifiers: modifiers)
        }
    }

    // MARK: - Scroll Events

    /// Send scroll wheel event (for pinch zoom, two-finger scroll)
    func sendScrollEvent(at point: CGPoint, deltaX: Int, deltaY: Int, modifiers: Int = 0) {
        vm_postScrollEvent(Int32(point.x), Int32(point.y),
                           Int32(deltaX), Int32(deltaY),
                           Int32(modifiers))
    }

    // MARK: - Utilities

    /// Check if running on iPad (or Mac Catalyst, which uses iPad idiom)
    var isIPad: Bool {
        return UIDevice.current.userInterfaceIdiom == .pad
    }

    /// Get device model string
    var deviceModel: String {
        return String(cString: iosGetDeviceModel())
    }

    /// Get iOS version string
    var systemVersion: String {
        return String(cString: iosGetSystemVersion())
    }

    /// Open URL in Safari
    func openURL(_ urlString: String) -> Bool {
        return iosOpenURL(urlString) != 0
    }

    /// Show native alert
    func showAlert(title: String, message: String) {
        iosShowAlert(title, message)
    }

    /// Trigger haptic feedback
    func hapticFeedback(style: HapticStyle = .medium) {
        iosHapticFeedback(Int32(style.rawValue))
    }

    enum HapticStyle: Int {
        case light = 0
        case medium = 1
        case heavy = 2
    }

    // MARK: - Shutdown

    /// Called when the interpreter exits naturally (e.g. primitiveQuit)
    private func handleVMExit() {
        guard isRunning else { return }
        #if DEBUG
        NSLog("[BRIDGE] VM exited, cleaning up")
        #endif
        motionTimer?.invalidate()
        motionTimer = nil
        motionManager.stop()
        vm_stop()     // Wait for thread, detach it, stop heartbeat
        vm_destroy()  // Delete all C++ objects, reset state for relaunch
        isRunning = false
        isInitialized = false
        imagePath = nil
        // SwiftUI will transition back to the image library since isRunning = false
    }

    /// Stop the VM - MUST be called before app exit to prevent crash
    func stop() {
        guard isRunning else { return }
        #if DEBUG
        NSLog("[BRIDGE] Stopping VM...")
        #endif
        motionTimer?.invalidate()
        motionTimer = nil
        motionManager.stop()
        vm_stop()
        #if DEBUG
        NSLog("[BRIDGE] VM stopped")
        #endif
        isRunning = false
        isInitialized = false
    }
}

// MARK: - Button/Modifier Constants

let IOS_RED_BUTTON = 4
let IOS_YELLOW_BUTTON = 2
let IOS_BLUE_BUTTON = 1

let IOS_SHIFT_KEY = 1
let IOS_CTRL_KEY = 2
let IOS_ALT_KEY = 4
let IOS_CMD_KEY = 8

