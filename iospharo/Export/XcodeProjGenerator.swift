/*
 * XcodeProjGenerator.swift
 *
 * Generates a minimal but valid Xcode project (.xcodeproj/project.pbxproj)
 * for the exported Pharo app. No external dependencies (no xcodegen).
 *
 * The generated project:
 *   - Links PharoVMCore.xcframework and other xcframeworks
 *   - Compiles Swift sources with a bridging header
 *   - Copies Resources/ (Pharo image, startup.st) into the bundle
 *   - Supports both iOS and Mac Catalyst targets
 *   - Optionally includes a watchOS companion app target
 */

import Foundation

class XcodeProjGenerator {

    let config: ExportConfig
    let projectDir: URL
    private let watchOS: Bool

    init(config: ExportConfig, projectDir: URL) {
        self.config = config
        self.projectDir = projectDir
        self.watchOS = config.exportWatchOS
    }

    func generate() throws {
        let xcodeprojDir = projectDir.appendingPathComponent("\(config.appName).xcodeproj")
        try FileManager.default.createDirectory(at: xcodeprojDir, withIntermediateDirectories: true)

        let pbxproj = generatePbxproj()
        try pbxproj.write(to: xcodeprojDir.appendingPathComponent("project.pbxproj"),
                         atomically: true, encoding: .utf8)
    }

    // MARK: - pbxproj Generation

    /// Generates a deterministic UUID-like string from a name
    private func id(_ name: String) -> String {
        // Use a simple hash to generate 24-char hex IDs
        var hash: UInt64 = 0xcbf29ce484222325  // FNV offset basis
        for byte in name.utf8 {
            hash ^= UInt64(byte)
            hash &*= 0x100000001b3  // FNV prime
        }
        let hi = String(hash, radix: 16, uppercase: true)
        // Second hash for more bits
        var hash2: UInt64 = 0x84222325cbf29ce4
        for byte in name.utf8.reversed() {
            hash2 ^= UInt64(byte)
            hash2 &*= 0x100000001b3
        }
        let lo = String(hash2, radix: 16, uppercase: true)
        let combined = (hi + lo).padding(toLength: 24, withPad: "0", startingAt: 0)
        return String(combined.prefix(24))
    }

    private func generatePbxproj() -> String {
        let appName = config.appName
        let bundleID = config.bundleID
        let teamID = config.teamID ?? ""

        // File references
        let sourceFiles = [
            "App.swift",
            "ContentView.swift",
            "PharoBridge.swift",
            "MetalRenderer.swift",
            "PharoCanvasView.swift",
        ]

        let metalFiles = ["Shaders.metal"]

        let watchSourceFiles = ["WatchApp.swift", "ContentView.swift"]

        // PharoVMCore.a has all third-party libs merged in except SDL2 and libffi
        let frameworks = [
            "PharoVMCore", "SDL2", "libffi",
        ]

        var lines = [String]()

        func ln(_ s: String) { lines.append(s) }

        ln("// !$*UTF8*$!")
        ln("{")
        ln("\tarchiveVersion = 1;")
        ln("\tclasses = {")
        ln("\t};")
        ln("\tobjectVersion = 56;")
        ln("\tobjects = {")

        // === PBXBuildFile ===
        ln("")
        ln("/* Begin PBXBuildFile section */")
        // Main target sources
        for f in sourceFiles {
            ln("\t\t\(id("build_\(f)")) /* \(f) in Sources */ = {isa = PBXBuildFile; fileRef = \(id("file_\(f)")); };")
        }
        for f in metalFiles {
            ln("\t\t\(id("build_\(f)")) /* \(f) in Sources */ = {isa = PBXBuildFile; fileRef = \(id("file_\(f)")); };")
        }
        ln("\t\t\(id("build_assets")) /* Assets.xcassets in Resources */ = {isa = PBXBuildFile; fileRef = \(id("file_assets")); };")
        ln("\t\t\(id("build_resources")) /* Resources in Resources */ = {isa = PBXBuildFile; fileRef = \(id("file_resources")); };")
        for fw in frameworks {
            ln("\t\t\(id("build_fw_\(fw)")) /* \(fw).xcframework in Frameworks */ = {isa = PBXBuildFile; fileRef = \(id("file_fw_\(fw)")); };")
            ln("\t\t\(id("embed_fw_\(fw)")) /* \(fw).xcframework in Embed Frameworks */ = {isa = PBXBuildFile; fileRef = \(id("file_fw_\(fw)")); settings = {ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }; };")
        }
        // Watch target sources
        if watchOS {
            for f in watchSourceFiles {
                ln("\t\t\(id("watch_build_\(f)")) /* \(f) in Sources */ = {isa = PBXBuildFile; fileRef = \(id("watch_file_\(f)")); };")
            }
            ln("\t\t\(id("watch_build_assets")) /* Assets.xcassets in Resources */ = {isa = PBXBuildFile; fileRef = \(id("watch_file_assets")); };")
            // Embed watch app in iOS target
            ln("\t\t\(id("embed_watch_product")) /* \(appName) Watch.app in Embed Watch Content */ = {isa = PBXBuildFile; fileRef = \(id("watch_product")); settings = {ATTRIBUTES = (RemoveHeadersOnCopy, ); }; };")
        }
        ln("/* End PBXBuildFile section */")

        // === PBXContainerItemProxy (watch dependency) ===
        if watchOS {
            ln("")
            ln("/* Begin PBXContainerItemProxy section */")
            ln("\t\t\(id("watch_proxy")) /* PBXContainerItemProxy */ = {")
            ln("\t\t\tisa = PBXContainerItemProxy;")
            ln("\t\t\tcontainerPortal = \(id("project")) /* Project object */;")
            ln("\t\t\tproxyType = 1;")
            ln("\t\t\tremoteGlobalIDString = \(id("watch_target"));")
            ln("\t\t\tremoteInfo = \"\(appName) Watch\";")
            ln("\t\t};")
            ln("/* End PBXContainerItemProxy section */")
        }

        // === PBXCopyFilesBuildPhase (Embed Frameworks + Embed Watch Content) ===
        ln("")
        ln("/* Begin PBXCopyFilesBuildPhase section */")
        // Embed Frameworks
        ln("\t\t\(id("embed_phase")) /* Embed Frameworks */ = {")
        ln("\t\t\tisa = PBXCopyFilesBuildPhase;")
        ln("\t\t\tbuildActionMask = 2147483647;")
        ln("\t\t\tdstPath = \"\";")
        ln("\t\t\tdstSubfolderSpec = 10;")
        ln("\t\t\tfiles = (")
        for fw in frameworks {
            ln("\t\t\t\t\(id("embed_fw_\(fw)")) /* \(fw).xcframework */,")
        }
        ln("\t\t\t);")
        ln("\t\t\tname = \"Embed Frameworks\";")
        ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
        ln("\t\t};")
        // Embed Watch Content
        if watchOS {
            ln("\t\t\(id("embed_watch_phase")) /* Embed Watch Content */ = {")
            ln("\t\t\tisa = PBXCopyFilesBuildPhase;")
            ln("\t\t\tbuildActionMask = 2147483647;")
            ln("\t\t\tdstPath = \"$(CONTENTS_FOLDER_PATH)/Watch\";")
            ln("\t\t\tdstSubfolderSpec = 16;")
            ln("\t\t\tfiles = (")
            ln("\t\t\t\t\(id("embed_watch_product")) /* \(appName) Watch.app */,")
            ln("\t\t\t);")
            ln("\t\t\tname = \"Embed Watch Content\";")
            ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
            ln("\t\t};")
        }
        ln("/* End PBXCopyFilesBuildPhase section */")

        // === PBXFileReference ===
        ln("")
        ln("/* Begin PBXFileReference section */")
        // Main target product
        ln("\t\t\(id("product")) /* \(appName).app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = \"\(appName).app\"; sourceTree = BUILT_PRODUCTS_DIR; };")
        // Main sources
        for f in sourceFiles {
            ln("\t\t\(id("file_\(f)")) /* \(f) */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = \"\(f)\"; sourceTree = \"<group>\"; };")
        }
        for f in metalFiles {
            ln("\t\t\(id("file_\(f)")) /* \(f) */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.metal; path = \"\(f)\"; sourceTree = \"<group>\"; };")
        }
        ln("\t\t\(id("file_bridging")) /* BridgingHeader.h */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = \"BridgingHeader.h\"; sourceTree = \"<group>\"; };")
        ln("\t\t\(id("file_assets")) /* Assets.xcassets */ = {isa = PBXFileReference; lastKnownFileType = folder.assetcatalog; path = Assets.xcassets; sourceTree = \"<group>\"; };")
        ln("\t\t\(id("file_resources")) /* Resources */ = {isa = PBXFileReference; lastKnownFileType = folder; path = Resources; sourceTree = \"<group>\"; };")
        ln("\t\t\(id("file_infoplist")) /* Info.plist */ = {isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = Info.plist; sourceTree = \"<group>\"; };")
        ln("\t\t\(id("file_entitlements")) /* \(appName).entitlements */ = {isa = PBXFileReference; lastKnownFileType = text.plist.entitlements; path = \"\(appName).entitlements\"; sourceTree = \"<group>\"; };")
        for fw in frameworks {
            ln("\t\t\(id("file_fw_\(fw)")) /* \(fw).xcframework */ = {isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; path = \"\(fw).xcframework\"; sourceTree = \"<group>\"; };")
        }
        // Headers
        ln("\t\t\(id("file_vmparams")) /* VMParameters.h */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = \"VMParameters.h\"; sourceTree = \"<group>\"; };")
        ln("\t\t\(id("file_motiondata")) /* MotionData.h */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = \"MotionData.h\"; sourceTree = \"<group>\"; };")
        // Watch target
        if watchOS {
            ln("\t\t\(id("watch_product")) /* \(appName) Watch.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = \"\(appName) Watch.app\"; sourceTree = BUILT_PRODUCTS_DIR; };")
            for f in watchSourceFiles {
                ln("\t\t\(id("watch_file_\(f)")) /* \(f) */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = \"\(f)\"; sourceTree = \"<group>\"; };")
            }
            ln("\t\t\(id("watch_file_assets")) /* Assets.xcassets */ = {isa = PBXFileReference; lastKnownFileType = folder.assetcatalog; path = Assets.xcassets; sourceTree = \"<group>\"; };")
            ln("\t\t\(id("watch_file_infoplist")) /* Info.plist */ = {isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = Info.plist; sourceTree = \"<group>\"; };")
        }
        ln("/* End PBXFileReference section */")

        // === PBXFrameworksBuildPhase ===
        ln("")
        ln("/* Begin PBXFrameworksBuildPhase section */")
        ln("\t\t\(id("frameworks_phase")) /* Frameworks */ = {")
        ln("\t\t\tisa = PBXFrameworksBuildPhase;")
        ln("\t\t\tbuildActionMask = 2147483647;")
        ln("\t\t\tfiles = (")
        for fw in frameworks {
            ln("\t\t\t\t\(id("build_fw_\(fw)")) /* \(fw).xcframework */,")
        }
        ln("\t\t\t);")
        ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
        ln("\t\t};")
        // Watch target has no frameworks phase (pure SwiftUI, no xcframeworks)
        if watchOS {
            ln("\t\t\(id("watch_frameworks_phase")) /* Frameworks */ = {")
            ln("\t\t\tisa = PBXFrameworksBuildPhase;")
            ln("\t\t\tbuildActionMask = 2147483647;")
            ln("\t\t\tfiles = (")
            ln("\t\t\t);")
            ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
            ln("\t\t};")
        }
        ln("/* End PBXFrameworksBuildPhase section */")

        // === PBXGroup ===
        ln("")
        ln("/* Begin PBXGroup section */")
        // Root group
        ln("\t\t\(id("group_root")) = {")
        ln("\t\t\tisa = PBXGroup;")
        ln("\t\t\tchildren = (")
        ln("\t\t\t\t\(id("group_sources")) /* Sources */,")
        ln("\t\t\t\t\(id("group_headers")) /* Headers */,")
        ln("\t\t\t\t\(id("group_frameworks")) /* Frameworks */,")
        ln("\t\t\t\t\(id("file_assets")) /* Assets.xcassets */,")
        ln("\t\t\t\t\(id("file_resources")) /* Resources */,")
        ln("\t\t\t\t\(id("file_infoplist")) /* Info.plist */,")
        ln("\t\t\t\t\(id("file_entitlements")) /* \(appName).entitlements */,")
        if watchOS {
            ln("\t\t\t\t\(id("group_watchapp")) /* WatchApp */,")
        }
        ln("\t\t\t\t\(id("group_products")) /* Products */,")
        ln("\t\t\t);")
        ln("\t\t\tsourceTree = \"<group>\";")
        ln("\t\t};")

        // Sources group
        ln("\t\t\(id("group_sources")) /* Sources */ = {")
        ln("\t\t\tisa = PBXGroup;")
        ln("\t\t\tchildren = (")
        for f in sourceFiles {
            ln("\t\t\t\t\(id("file_\(f)")) /* \(f) */,")
        }
        for f in metalFiles {
            ln("\t\t\t\t\(id("file_\(f)")) /* \(f) */,")
        }
        ln("\t\t\t\t\(id("file_bridging")) /* BridgingHeader.h */,")
        ln("\t\t\t);")
        ln("\t\t\tpath = Sources;")
        ln("\t\t\tsourceTree = \"<group>\";")
        ln("\t\t};")

        // Headers group
        ln("\t\t\(id("group_headers")) /* Headers */ = {")
        ln("\t\t\tisa = PBXGroup;")
        ln("\t\t\tchildren = (")
        ln("\t\t\t\t\(id("file_vmparams")) /* VMParameters.h */,")
        ln("\t\t\t\t\(id("file_motiondata")) /* MotionData.h */,")
        ln("\t\t\t);")
        ln("\t\t\tpath = Headers;")
        ln("\t\t\tsourceTree = \"<group>\";")
        ln("\t\t};")

        // Frameworks group
        ln("\t\t\(id("group_frameworks")) /* Frameworks */ = {")
        ln("\t\t\tisa = PBXGroup;")
        ln("\t\t\tchildren = (")
        for fw in frameworks {
            ln("\t\t\t\t\(id("file_fw_\(fw)")) /* \(fw).xcframework */,")
        }
        ln("\t\t\t);")
        ln("\t\t\tpath = Frameworks;")
        ln("\t\t\tsourceTree = \"<group>\";")
        ln("\t\t};")

        // WatchApp group
        if watchOS {
            ln("\t\t\(id("group_watchapp")) /* WatchApp */ = {")
            ln("\t\t\tisa = PBXGroup;")
            ln("\t\t\tchildren = (")
            for f in watchSourceFiles {
                ln("\t\t\t\t\(id("watch_file_\(f)")) /* \(f) */,")
            }
            ln("\t\t\t\t\(id("watch_file_assets")) /* Assets.xcassets */,")
            ln("\t\t\t\t\(id("watch_file_infoplist")) /* Info.plist */,")
            ln("\t\t\t);")
            ln("\t\t\tpath = WatchApp;")
            ln("\t\t\tsourceTree = \"<group>\";")
            ln("\t\t};")
        }

        // Products group
        ln("\t\t\(id("group_products")) /* Products */ = {")
        ln("\t\t\tisa = PBXGroup;")
        ln("\t\t\tchildren = (")
        ln("\t\t\t\t\(id("product")) /* \(appName).app */,")
        if watchOS {
            ln("\t\t\t\t\(id("watch_product")) /* \(appName) Watch.app */,")
        }
        ln("\t\t\t);")
        ln("\t\t\tname = Products;")
        ln("\t\t\tsourceTree = \"<group>\";")
        ln("\t\t};")
        ln("/* End PBXGroup section */")

        // === PBXNativeTarget ===
        ln("")
        ln("/* Begin PBXNativeTarget section */")
        // Main iOS/Mac target
        ln("\t\t\(id("target")) /* \(appName) */ = {")
        ln("\t\t\tisa = PBXNativeTarget;")
        ln("\t\t\tbuildConfigurationList = \(id("target_config_list")) /* Build configuration list for PBXNativeTarget */;")
        ln("\t\t\tbuildPhases = (")
        ln("\t\t\t\t\(id("sources_phase")) /* Sources */,")
        ln("\t\t\t\t\(id("frameworks_phase")) /* Frameworks */,")
        ln("\t\t\t\t\(id("resources_phase")) /* Resources */,")
        ln("\t\t\t\t\(id("embed_phase")) /* Embed Frameworks */,")
        if watchOS {
            ln("\t\t\t\t\(id("embed_watch_phase")) /* Embed Watch Content */,")
        }
        ln("\t\t\t);")
        ln("\t\t\tbuildRules = (")
        ln("\t\t\t);")
        ln("\t\t\tdependencies = (")
        if watchOS {
            ln("\t\t\t\t\(id("watch_dependency")) /* PBXTargetDependency */,")
        }
        ln("\t\t\t);")
        ln("\t\t\tname = \"\(appName)\";")
        ln("\t\t\tproductName = \"\(appName)\";")
        ln("\t\t\tproductReference = \(id("product")) /* \(appName).app */;")
        ln("\t\t\tproductType = \"com.apple.product-type.application\";")
        ln("\t\t};")
        // Watch target
        if watchOS {
            ln("\t\t\(id("watch_target")) /* \(appName) Watch */ = {")
            ln("\t\t\tisa = PBXNativeTarget;")
            ln("\t\t\tbuildConfigurationList = \(id("watch_config_list")) /* Build configuration list for watch target */;")
            ln("\t\t\tbuildPhases = (")
            ln("\t\t\t\t\(id("watch_sources_phase")) /* Sources */,")
            ln("\t\t\t\t\(id("watch_frameworks_phase")) /* Frameworks */,")
            ln("\t\t\t\t\(id("watch_resources_phase")) /* Resources */,")
            ln("\t\t\t);")
            ln("\t\t\tbuildRules = (")
            ln("\t\t\t);")
            ln("\t\t\tdependencies = (")
            ln("\t\t\t);")
            ln("\t\t\tname = \"\(appName) Watch\";")
            ln("\t\t\tproductName = \"\(appName) Watch\";")
            ln("\t\t\tproductReference = \(id("watch_product")) /* \(appName) Watch.app */;")
            ln("\t\t\tproductType = \"com.apple.product-type.application\";")
            ln("\t\t};")
        }
        ln("/* End PBXNativeTarget section */")

        // === PBXProject ===
        ln("")
        ln("/* Begin PBXProject section */")
        ln("\t\t\(id("project")) /* Project object */ = {")
        ln("\t\t\tisa = PBXProject;")
        ln("\t\t\tbuildConfigurationList = \(id("project_config_list")) /* Build configuration list for PBXProject */;")
        ln("\t\t\tcompatibilityVersion = \"Xcode 14.0\";")
        ln("\t\t\tdevelopmentRegion = en;")
        ln("\t\t\thasScannedForEncodings = 0;")
        ln("\t\t\tknownRegions = (en, Base);")
        ln("\t\t\tmainGroup = \(id("group_root"));")
        ln("\t\t\tproductRefGroup = \(id("group_products")) /* Products */;")
        ln("\t\t\tprojectDirPath = \"\";")
        ln("\t\t\tprojectRoot = \"\";")
        ln("\t\t\ttargets = (")
        ln("\t\t\t\t\(id("target")) /* \(appName) */,")
        if watchOS {
            ln("\t\t\t\t\(id("watch_target")) /* \(appName) Watch */,")
        }
        ln("\t\t\t);")
        ln("\t\t};")
        ln("/* End PBXProject section */")

        // === PBXResourcesBuildPhase ===
        ln("")
        ln("/* Begin PBXResourcesBuildPhase section */")
        // Main target
        ln("\t\t\(id("resources_phase")) /* Resources */ = {")
        ln("\t\t\tisa = PBXResourcesBuildPhase;")
        ln("\t\t\tbuildActionMask = 2147483647;")
        ln("\t\t\tfiles = (")
        ln("\t\t\t\t\(id("build_assets")) /* Assets.xcassets */,")
        ln("\t\t\t\t\(id("build_resources")) /* Resources */,")
        ln("\t\t\t);")
        ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
        ln("\t\t};")
        // Watch target
        if watchOS {
            ln("\t\t\(id("watch_resources_phase")) /* Resources */ = {")
            ln("\t\t\tisa = PBXResourcesBuildPhase;")
            ln("\t\t\tbuildActionMask = 2147483647;")
            ln("\t\t\tfiles = (")
            ln("\t\t\t\t\(id("watch_build_assets")) /* Assets.xcassets */,")
            ln("\t\t\t);")
            ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
            ln("\t\t};")
        }
        ln("/* End PBXResourcesBuildPhase section */")

        // === PBXSourcesBuildPhase ===
        ln("")
        ln("/* Begin PBXSourcesBuildPhase section */")
        // Main target
        ln("\t\t\(id("sources_phase")) /* Sources */ = {")
        ln("\t\t\tisa = PBXSourcesBuildPhase;")
        ln("\t\t\tbuildActionMask = 2147483647;")
        ln("\t\t\tfiles = (")
        for f in sourceFiles {
            ln("\t\t\t\t\(id("build_\(f)")) /* \(f) */,")
        }
        for f in metalFiles {
            ln("\t\t\t\t\(id("build_\(f)")) /* \(f) */,")
        }
        ln("\t\t\t);")
        ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
        ln("\t\t};")
        // Watch target
        if watchOS {
            ln("\t\t\(id("watch_sources_phase")) /* Sources */ = {")
            ln("\t\t\tisa = PBXSourcesBuildPhase;")
            ln("\t\t\tbuildActionMask = 2147483647;")
            ln("\t\t\tfiles = (")
            for f in watchSourceFiles {
                ln("\t\t\t\t\(id("watch_build_\(f)")) /* \(f) */,")
            }
            ln("\t\t\t);")
            ln("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
            ln("\t\t};")
        }
        ln("/* End PBXSourcesBuildPhase section */")

        // === PBXTargetDependency ===
        if watchOS {
            ln("")
            ln("/* Begin PBXTargetDependency section */")
            ln("\t\t\(id("watch_dependency")) /* PBXTargetDependency */ = {")
            ln("\t\t\tisa = PBXTargetDependency;")
            ln("\t\t\ttarget = \(id("watch_target")) /* \(appName) Watch */;")
            ln("\t\t\ttargetProxy = \(id("watch_proxy")) /* PBXContainerItemProxy */;")
            ln("\t\t};")
            ln("/* End PBXTargetDependency section */")
        }

        // === XCBuildConfiguration ===
        ln("")
        ln("/* Begin XCBuildConfiguration section */")

        // Project-level Debug
        ln("\t\t\(id("project_debug")) /* Debug */ = {")
        ln("\t\t\tisa = XCBuildConfiguration;")
        ln("\t\t\tbuildSettings = {")
        ln("\t\t\t\tALWAYS_SEARCH_USER_PATHS = NO;")
        ln("\t\t\t\tCLANG_ENABLE_MODULES = YES;")
        ln("\t\t\t\tCOPY_PHASE_STRIP = NO;")
        ln("\t\t\t\tDEBUG_INFORMATION_FORMAT = dwarf;")
        ln("\t\t\t\tENABLE_STRICT_OBJC_MSGSEND = YES;")
        ln("\t\t\t\tGCC_OPTIMIZATION_LEVEL = 0;")
        ln("\t\t\t\tIPHONEOS_DEPLOYMENT_TARGET = 16.0;")
        ln("\t\t\t\tMTL_ENABLE_DEBUG_INFO = YES;")
        ln("\t\t\t\tONLY_ACTIVE_ARCH = YES;")
        ln("\t\t\t\tSDKROOT = iphoneos;")
        ln("\t\t\t\tSWIFT_ACTIVE_COMPILATION_CONDITIONS = DEBUG;")
        ln("\t\t\t\tSWIFT_OPTIMIZATION_LEVEL = \"-Onone\";")
        ln("\t\t\t};")
        ln("\t\t\tname = Debug;")
        ln("\t\t};")

        // Project-level Release
        ln("\t\t\(id("project_release")) /* Release */ = {")
        ln("\t\t\tisa = XCBuildConfiguration;")
        ln("\t\t\tbuildSettings = {")
        ln("\t\t\t\tALWAYS_SEARCH_USER_PATHS = NO;")
        ln("\t\t\t\tCLANG_ENABLE_MODULES = YES;")
        ln("\t\t\t\tCOPY_PHASE_STRIP = NO;")
        ln("\t\t\t\tDEBUG_INFORMATION_FORMAT = \"dwarf-with-dsym\";")
        ln("\t\t\t\tENABLE_NS_ASSERTIONS = NO;")
        ln("\t\t\t\tENABLE_STRICT_OBJC_MSGSEND = YES;")
        ln("\t\t\t\tIPHONEOS_DEPLOYMENT_TARGET = 16.0;")
        ln("\t\t\t\tMTL_ENABLE_DEBUG_INFO = NO;")
        ln("\t\t\t\tSDKROOT = iphoneos;")
        ln("\t\t\t\tSWIFT_COMPILATION_MODE = wholemodule;")
        ln("\t\t\t\tSWIFT_OPTIMIZATION_LEVEL = \"-O\";")
        ln("\t\t\t\tVALIDATE_PRODUCT = YES;")
        ln("\t\t\t};")
        ln("\t\t\tname = Release;")
        ln("\t\t};")

        // Main target Debug
        ln("\t\t\(id("target_debug")) /* Debug */ = {")
        ln("\t\t\tisa = XCBuildConfiguration;")
        ln("\t\t\tbuildSettings = {")
        ln("\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;")
        ln("\t\t\t\tASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME = AccentColor;")
        ln("\t\t\t\tCODE_SIGN_ENTITLEMENTS = \"\(appName).entitlements\";")
        ln("\t\t\t\tCODE_SIGN_STYLE = Automatic;")
        if !teamID.isEmpty {
            ln("\t\t\t\tDEVELOPMENT_TEAM = \(teamID);")
        }
        ln("\t\t\t\tINFOPLIST_FILE = Info.plist;")
        ln("\t\t\t\tLD_RUNPATH_SEARCH_PATHS = (\"$(inherited)\", \"@executable_path/Frameworks\");")
        ln("\t\t\t\tPRODUCT_BUNDLE_IDENTIFIER = \"\(bundleID)\";")
        ln("\t\t\t\tPRODUCT_NAME = \"$(TARGET_NAME)\";")
        ln("\t\t\t\tSUPPORTS_MACCATALYST = YES;")
        ln("\t\t\t\tSWIFT_EMIT_LOC_STRINGS = YES;")
        ln("\t\t\t\tSWIFT_OBJC_BRIDGING_HEADER = \"Sources/BridgingHeader.h\";")
        ln("\t\t\t\tSWIFT_VERSION = 5.0;")
        ln("\t\t\t\tTARGETED_DEVICE_FAMILY = \"1,2,6\";")
        ln("\t\t\t\tFRAMEWORK_SEARCH_PATHS = (\"$(inherited)\", \"$(PROJECT_DIR)/Frameworks\");")
        ln("\t\t\t\tHEADER_SEARCH_PATHS = (\"$(inherited)\", \"$(PROJECT_DIR)/Headers\");")
        ln("\t\t\t\tOTHER_LDFLAGS = (\"$(inherited)\", \"-lc++\", \"-framework\", CoreAudio, \"-framework\", AudioToolbox, \"-framework\", CoreMIDI, \"-framework\", CoreGraphics, \"-framework\", CoreText, \"-framework\", QuartzCore, \"-framework\", Security);")
        ln("\t\t\t};")
        ln("\t\t\tname = Debug;")
        ln("\t\t};")

        // Main target Release
        ln("\t\t\(id("target_release")) /* Release */ = {")
        ln("\t\t\tisa = XCBuildConfiguration;")
        ln("\t\t\tbuildSettings = {")
        ln("\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;")
        ln("\t\t\t\tASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME = AccentColor;")
        ln("\t\t\t\tCODE_SIGN_ENTITLEMENTS = \"\(appName).entitlements\";")
        ln("\t\t\t\tCODE_SIGN_STYLE = Automatic;")
        if !teamID.isEmpty {
            ln("\t\t\t\tDEVELOPMENT_TEAM = \(teamID);")
        }
        ln("\t\t\t\tINFOPLIST_FILE = Info.plist;")
        ln("\t\t\t\tLD_RUNPATH_SEARCH_PATHS = (\"$(inherited)\", \"@executable_path/Frameworks\");")
        ln("\t\t\t\tPRODUCT_BUNDLE_IDENTIFIER = \"\(bundleID)\";")
        ln("\t\t\t\tPRODUCT_NAME = \"$(TARGET_NAME)\";")
        ln("\t\t\t\tSUPPORTS_MACCATALYST = YES;")
        ln("\t\t\t\tSWIFT_EMIT_LOC_STRINGS = YES;")
        ln("\t\t\t\tSWIFT_OBJC_BRIDGING_HEADER = \"Sources/BridgingHeader.h\";")
        ln("\t\t\t\tSWIFT_VERSION = 5.0;")
        ln("\t\t\t\tTARGETED_DEVICE_FAMILY = \"1,2,6\";")
        ln("\t\t\t\tFRAMEWORK_SEARCH_PATHS = (\"$(inherited)\", \"$(PROJECT_DIR)/Frameworks\");")
        ln("\t\t\t\tHEADER_SEARCH_PATHS = (\"$(inherited)\", \"$(PROJECT_DIR)/Headers\");")
        ln("\t\t\t\tOTHER_LDFLAGS = (\"$(inherited)\", \"-lc++\", \"-framework\", CoreAudio, \"-framework\", AudioToolbox, \"-framework\", CoreMIDI, \"-framework\", CoreGraphics, \"-framework\", CoreText, \"-framework\", QuartzCore, \"-framework\", Security);")
        ln("\t\t\t};")
        ln("\t\t\tname = Release;")
        ln("\t\t};")

        // Watch target Debug
        if watchOS {
            ln("\t\t\(id("watch_debug")) /* Debug */ = {")
            ln("\t\t\tisa = XCBuildConfiguration;")
            ln("\t\t\tbuildSettings = {")
            ln("\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;")
            ln("\t\t\t\tASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME = AccentColor;")
            ln("\t\t\t\tCODE_SIGN_STYLE = Automatic;")
            if !teamID.isEmpty {
                ln("\t\t\t\tDEVELOPMENT_TEAM = \(teamID);")
            }
            ln("\t\t\t\tINFOPLIST_FILE = \"WatchApp/Info.plist\";")
            ln("\t\t\t\tPRODUCT_BUNDLE_IDENTIFIER = \"\(config.watchBundleID)\";")
            ln("\t\t\t\tPRODUCT_NAME = \"$(TARGET_NAME)\";")
            ln("\t\t\t\tSDKROOT = watchos;")
            ln("\t\t\t\tSWIFT_EMIT_LOC_STRINGS = YES;")
            ln("\t\t\t\tSWIFT_VERSION = 5.0;")
            ln("\t\t\t\tTARGETED_DEVICE_FAMILY = 4;")
            ln("\t\t\t\tWATCHOS_DEPLOYMENT_TARGET = 10.0;")
            ln("\t\t\t};")
            ln("\t\t\tname = Debug;")
            ln("\t\t};")

            // Watch target Release
            ln("\t\t\(id("watch_release")) /* Release */ = {")
            ln("\t\t\tisa = XCBuildConfiguration;")
            ln("\t\t\tbuildSettings = {")
            ln("\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;")
            ln("\t\t\t\tASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME = AccentColor;")
            ln("\t\t\t\tCODE_SIGN_STYLE = Automatic;")
            if !teamID.isEmpty {
                ln("\t\t\t\tDEVELOPMENT_TEAM = \(teamID);")
            }
            ln("\t\t\t\tINFOPLIST_FILE = \"WatchApp/Info.plist\";")
            ln("\t\t\t\tPRODUCT_BUNDLE_IDENTIFIER = \"\(config.watchBundleID)\";")
            ln("\t\t\t\tPRODUCT_NAME = \"$(TARGET_NAME)\";")
            ln("\t\t\t\tSDKROOT = watchos;")
            ln("\t\t\t\tSWIFT_EMIT_LOC_STRINGS = YES;")
            ln("\t\t\t\tSWIFT_VERSION = 5.0;")
            ln("\t\t\t\tTARGETED_DEVICE_FAMILY = 4;")
            ln("\t\t\t\tVALIDATE_PRODUCT = YES;")
            ln("\t\t\t\tWATCHOS_DEPLOYMENT_TARGET = 10.0;")
            ln("\t\t\t};")
            ln("\t\t\tname = Release;")
            ln("\t\t};")
        }
        ln("/* End XCBuildConfiguration section */")

        // === XCConfigurationList ===
        ln("")
        ln("/* Begin XCConfigurationList section */")
        ln("\t\t\(id("project_config_list")) /* Build configuration list for PBXProject */ = {")
        ln("\t\t\tisa = XCConfigurationList;")
        ln("\t\t\tbuildConfigurations = (")
        ln("\t\t\t\t\(id("project_debug")) /* Debug */,")
        ln("\t\t\t\t\(id("project_release")) /* Release */,")
        ln("\t\t\t);")
        ln("\t\t\tdefaultConfigurationIsVisible = 0;")
        ln("\t\t\tdefaultConfigurationName = Release;")
        ln("\t\t};")
        ln("\t\t\(id("target_config_list")) /* Build configuration list for PBXNativeTarget */ = {")
        ln("\t\t\tisa = XCConfigurationList;")
        ln("\t\t\tbuildConfigurations = (")
        ln("\t\t\t\t\(id("target_debug")) /* Debug */,")
        ln("\t\t\t\t\(id("target_release")) /* Release */,")
        ln("\t\t\t);")
        ln("\t\t\tdefaultConfigurationIsVisible = 0;")
        ln("\t\t\tdefaultConfigurationName = Release;")
        ln("\t\t};")
        if watchOS {
            ln("\t\t\(id("watch_config_list")) /* Build configuration list for watch target */ = {")
            ln("\t\t\tisa = XCConfigurationList;")
            ln("\t\t\tbuildConfigurations = (")
            ln("\t\t\t\t\(id("watch_debug")) /* Debug */,")
            ln("\t\t\t\t\(id("watch_release")) /* Release */,")
            ln("\t\t\t);")
            ln("\t\t\tdefaultConfigurationIsVisible = 0;")
            ln("\t\t\tdefaultConfigurationName = Release;")
            ln("\t\t};")
        }
        ln("/* End XCConfigurationList section */")

        ln("")
        ln("\t};")
        ln("\trootObject = \(id("project")) /* Project object */;")
        ln("}")

        return lines.joined(separator: "\n")
    }
}
