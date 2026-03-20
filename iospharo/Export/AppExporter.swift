/*
 * AppExporter.swift
 *
 * Generates a standalone Xcode project from a Pharo image.
 * The exported project embeds the image, links PharoVMCore.xcframework,
 * and boots directly into the Pharo canvas (no image library UI).
 *
 * Pipeline:
 *   Phase 1: Copy and prepare the image
 *   Phase 2: Generate Xcode project files
 *   Phase 3: Open in Xcode (user builds manually)
 */

import Foundation

struct ExportConfig {
    let appName: String
    let bundleID: String
    let teamID: String?
    let exportMacOS: Bool
    let exportIOS: Bool
    let exportWatchOS: Bool
    let kioskMode: Bool
    let stripImage: Bool
    let appIconURL: URL?
    let outputDirectory: URL
    let sourceImage: PharoImage

    /// Bundle ID for the watch companion app
    var watchBundleID: String { bundleID + ".watchapp" }
}

enum ExportError: LocalizedError {
    case imageNotFound
    case outputDirectoryExists(String)
    case copyFailed(String)
    case frameworkNotFound(String)
    case headerNotFound(String)
    case writeFailed(String)

    var errorDescription: String? {
        switch self {
        case .imageNotFound: return "Source image file not found"
        case .outputDirectoryExists(let path): return "Output directory already exists: \(path)"
        case .copyFailed(let detail): return "Failed to copy files: \(detail)"
        case .frameworkNotFound(let name): return "Required framework not found: \(name)"
        case .headerNotFound(let name): return "Required header not found: \(name)"
        case .writeFailed(let detail): return "Failed to write file: \(detail)"
        }
    }
}

class AppExporter {

    /// Sanitize a name to be a valid Swift identifier and filesystem name
    static func sanitizeName(_ name: String) -> String {
        var result = name
            .replacingOccurrences(of: " ", with: "")
            .replacingOccurrences(of: "-", with: "")
            .replacingOccurrences(of: "_", with: "")
        result = result.filter { $0.isLetter || $0.isNumber }
        if result.isEmpty { result = "PharoApp" }
        if let first = result.first, first.isNumber {
            result = "App" + result
        }
        return result
    }

    /// Export a Pharo image as a standalone Xcode project.
    /// Returns the URL of the generated project directory.
    func export(config: ExportConfig,
                progress: @escaping (String) -> Void) async throws -> URL {

        let fm = FileManager.default
        let projectDir = config.outputDirectory.appendingPathComponent(config.appName)

        // Refuse to overwrite existing directory
        if fm.fileExists(atPath: projectDir.path) {
            throw ExportError.outputDirectoryExists(projectDir.path)
        }

        // Phase 1: Copy and prepare image
        progress("Copying image files...")
        let resourcesDir = projectDir.appendingPathComponent("Resources")
        try fm.createDirectory(at: resourcesDir, withIntermediateDirectories: true)

        let sourceImageURL = config.sourceImage.imageURL
        guard fm.fileExists(atPath: sourceImageURL.path) else {
            throw ExportError.imageNotFound
        }

        // Copy .image file
        let destImageURL = resourcesDir.appendingPathComponent(config.sourceImage.imageFileName)
        try fm.copyItem(at: sourceImageURL, to: destImageURL)

        // Copy .changes file if present
        let changesName = (config.sourceImage.imageFileName as NSString)
            .deletingPathExtension + ".changes"
        let sourceChanges = config.sourceImage.directoryURL.appendingPathComponent(changesName)
        if fm.fileExists(atPath: sourceChanges.path) {
            try fm.copyItem(at: sourceChanges, to: resourcesDir.appendingPathComponent(changesName))
        }

        // Copy .sources file if present
        let sourceDir = config.sourceImage.directoryURL
        if let sources = try? fm.contentsOfDirectory(at: sourceDir, includingPropertiesForKeys: nil)
            .first(where: { $0.pathExtension == "sources" }) {
            try fm.copyItem(at: sources, to: resourcesDir.appendingPathComponent(sources.lastPathComponent))
        }

        // Write startup.st (kiosk mode stripping or plain patches)
        progress("Writing startup script...")
        let startupScript = AppTemplates.startupScript(kioskMode: config.kioskMode, stripImage: config.stripImage)
        try startupScript.write(
            to: resourcesDir.appendingPathComponent("startup.st"),
            atomically: true, encoding: .utf8)

        // Phase 2: Generate project structure
        progress("Generating project files...")

        // Sources directory
        let sourcesDir = projectDir.appendingPathComponent("Sources")
        try fm.createDirectory(at: sourcesDir, withIntermediateDirectories: true)

        // Write generated Swift source files
        let swiftFiles: [(String, String)] = [
            ("App.swift", AppTemplates.appSwift(appName: config.appName)),
            ("ContentView.swift", AppTemplates.contentViewSwift(appName: config.appName,
                                                                  imageFileName: config.sourceImage.imageFileName)),
            ("PharoBridge.swift", AppTemplates.pharoBridgeSwift()),
            ("MetalRenderer.swift", AppTemplates.metalRendererSwift()),
            ("Shaders.metal", AppTemplates.shadersMetal()),
            ("PharoCanvasView.swift", AppTemplates.pharoCanvasViewSwift()),
        ]

        for (filename, content) in swiftFiles {
            try content.write(to: sourcesDir.appendingPathComponent(filename),
                            atomically: true, encoding: .utf8)
        }

        // Write bridging header
        let bridgingHeader = AppTemplates.bridgingHeader()
        try bridgingHeader.write(
            to: sourcesDir.appendingPathComponent("BridgingHeader.h"),
            atomically: true, encoding: .utf8)

        // Copy platform headers (VMParameters.h, MotionData.h)
        progress("Copying platform headers...")
        let headersDir = projectDir.appendingPathComponent("Headers")
        try fm.createDirectory(at: headersDir, withIntermediateDirectories: true)

        // Headers are at known paths relative to the iospharo source tree.
        // In the app bundle, they're included via the xcframework headers.
        // For export, we generate minimal stubs that match the real structs.
        let vmParamsHeader = AppTemplates.vmParametersHeader()
        try vmParamsHeader.write(to: headersDir.appendingPathComponent("VMParameters.h"),
                                 atomically: true, encoding: .utf8)
        let motionHeader = AppTemplates.motionDataHeader()
        try motionHeader.write(to: headersDir.appendingPathComponent("MotionData.h"),
                               atomically: true, encoding: .utf8)

        // Copy frameworks
        progress("Copying frameworks...")
        let fwDstDir = projectDir.appendingPathComponent("Frameworks")
        try fm.createDirectory(at: fwDstDir, withIntermediateDirectories: true)

        // PharoVMCore.a already has cairo, freetype, harfbuzz, pixman, libpng16,
        // libssl, libcrypto, libssh2, libgit2 merged in via build-xcframework.sh.
        // Only PharoVMCore, SDL2, and libffi need separate xcframeworks.
        let requiredFrameworks = [
            "PharoVMCore",
            "SDL2",
            "libffi",
        ]

        // Frameworks are in the iospharo source tree under Frameworks/
        // Locate them relative to the main bundle or from a known path
        let fwSourceDir = Self.frameworksSourceDirectory()

        for fwName in requiredFrameworks {
            let xcfwName = "\(fwName).xcframework"
            let src = fwSourceDir.appendingPathComponent(xcfwName)
            guard fm.fileExists(atPath: src.path) else {
                throw ExportError.frameworkNotFound(xcfwName)
            }
            try fm.copyItem(at: src, to: fwDstDir.appendingPathComponent(xcfwName))
        }

        // Assets catalog
        progress("Generating asset catalog...")
        let assetsDir = projectDir.appendingPathComponent("Assets.xcassets")
        try fm.createDirectory(at: assetsDir, withIntermediateDirectories: true)
        try AppTemplates.assetsContentsJSON().write(
            to: assetsDir.appendingPathComponent("Contents.json"),
            atomically: true, encoding: .utf8)

        let accentDir = assetsDir.appendingPathComponent("AccentColor.colorset")
        try fm.createDirectory(at: accentDir, withIntermediateDirectories: true)
        try AppTemplates.colorSetJSON().write(
            to: accentDir.appendingPathComponent("Contents.json"),
            atomically: true, encoding: .utf8)

        let iconDir = assetsDir.appendingPathComponent("AppIcon.appiconset")
        try fm.createDirectory(at: iconDir, withIntermediateDirectories: true)
        if let iconURL = config.appIconURL, fm.fileExists(atPath: iconURL.path) {
            let iconFilename = "AppIcon.png"
            try fm.copyItem(at: iconURL, to: iconDir.appendingPathComponent(iconFilename))
            try AppTemplates.appIconJSONWithImage(filename: iconFilename).write(
                to: iconDir.appendingPathComponent("Contents.json"),
                atomically: true, encoding: .utf8)
        } else {
            try AppTemplates.appIconJSON().write(
                to: iconDir.appendingPathComponent("Contents.json"),
                atomically: true, encoding: .utf8)
        }

        // Watch companion app (if enabled)
        if config.exportWatchOS {
            progress("Generating Watch companion app...")
            let watchDir = projectDir.appendingPathComponent("WatchApp")
            try fm.createDirectory(at: watchDir, withIntermediateDirectories: true)

            let watchFiles: [(String, String)] = [
                ("WatchApp.swift", AppTemplates.watchAppSwift(appName: config.appName)),
                ("ContentView.swift", AppTemplates.watchContentViewSwift(appName: config.appName)),
            ]
            for (filename, content) in watchFiles {
                try content.write(to: watchDir.appendingPathComponent(filename),
                                atomically: true, encoding: .utf8)
            }

            try AppTemplates.watchInfoPlist(appName: config.appName).write(
                to: watchDir.appendingPathComponent("Info.plist"),
                atomically: true, encoding: .utf8)

            // Watch asset catalog
            let watchAssetsDir = watchDir.appendingPathComponent("Assets.xcassets")
            try fm.createDirectory(at: watchAssetsDir, withIntermediateDirectories: true)
            try AppTemplates.assetsContentsJSON().write(
                to: watchAssetsDir.appendingPathComponent("Contents.json"),
                atomically: true, encoding: .utf8)

            let watchAccentDir = watchAssetsDir.appendingPathComponent("AccentColor.colorset")
            try fm.createDirectory(at: watchAccentDir, withIntermediateDirectories: true)
            try AppTemplates.colorSetJSON().write(
                to: watchAccentDir.appendingPathComponent("Contents.json"),
                atomically: true, encoding: .utf8)

            let watchIconDir = watchAssetsDir.appendingPathComponent("AppIcon.appiconset")
            try fm.createDirectory(at: watchIconDir, withIntermediateDirectories: true)
            if let iconURL = config.appIconURL, fm.fileExists(atPath: iconURL.path) {
                let iconFilename = "AppIcon.png"
                try fm.copyItem(at: iconURL, to: watchIconDir.appendingPathComponent(iconFilename))
                try AppTemplates.watchAppIconJSONWithImage(filename: iconFilename).write(
                    to: watchIconDir.appendingPathComponent("Contents.json"),
                    atomically: true, encoding: .utf8)
            } else {
                try AppTemplates.watchAppIconJSON().write(
                    to: watchIconDir.appendingPathComponent("Contents.json"),
                    atomically: true, encoding: .utf8)
            }
        }

        // Write project config files
        progress("Generating Xcode project configuration...")
        let infoPlist = AppTemplates.infoPlist(
            appName: config.appName,
            imageFileName: config.sourceImage.imageFileName)
        try infoPlist.write(to: projectDir.appendingPathComponent("Info.plist"),
                           atomically: true, encoding: .utf8)

        let entitlements = AppTemplates.entitlements()
        try entitlements.write(
            to: projectDir.appendingPathComponent("\(config.appName).entitlements"),
            atomically: true, encoding: .utf8)

        // Generate the Xcode project
        progress("Generating Xcode project...")
        let generator = XcodeProjGenerator(config: config, projectDir: projectDir)
        try generator.generate()

        // Write build.sh convenience script
        let buildScript = AppTemplates.buildScript(
            appName: config.appName,
            macOS: config.exportMacOS,
            iOS: config.exportIOS)
        let scriptURL = projectDir.appendingPathComponent("build.sh")
        try buildScript.write(to: scriptURL, atomically: true, encoding: .utf8)
        // Make executable
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: scriptURL.path)

        progress("Done")
        return projectDir
    }

    // MARK: - Framework Location

    /// Find the Frameworks/ directory containing xcframeworks.
    /// In development: relative to the source tree.
    /// In the built app: inside the app bundle.
    static func frameworksSourceDirectory() -> URL {
        // Try the iospharo source tree location first
        // The app's main bundle path tells us where we are
        let bundle = Bundle.main

        // In Mac Catalyst, the bundle is at:
        //   .../DerivedData/.../Build/Products/Debug-maccatalyst/iospharo.app
        // The source tree Frameworks/ is at the project root
        if let resourcePath = bundle.resourcePath {
            // Check if xcframeworks are embedded in the app bundle's Frameworks/
            let bundleFW = URL(fileURLWithPath: resourcePath)
                .deletingLastPathComponent()
                .appendingPathComponent("Frameworks")
            if FileManager.default.fileExists(
                atPath: bundleFW.appendingPathComponent("PharoVMCore.xcframework").path) {
                return bundleFW
            }
        }

        // Fall back to source tree location
        // Navigate from executable to project root
        let execURL = URL(fileURLWithPath: ProcessInfo.processInfo.arguments[0])
        var candidate = execURL
        for _ in 0..<10 {
            candidate = candidate.deletingLastPathComponent()
            let fwDir = candidate.appendingPathComponent("Frameworks")
            if FileManager.default.fileExists(
                atPath: fwDir.appendingPathComponent("PharoVMCore.xcframework").path) {
                return fwDir
            }
        }

        // Last resort: hardcoded development path
        return URL(fileURLWithPath: "/Users/wohl/src/iospharo/Frameworks")
    }
}
