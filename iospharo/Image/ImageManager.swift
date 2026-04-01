/*
 * ImageManager.swift
 *
 * Multi-image library manager. Downloads, extracts, imports, and catalogs
 * Pharo images. Each image lives in its own subdirectory under Documents/Images/.
 * The catalog is persisted to Documents/image-library.json.
 */

import Foundation
import Combine
import ZIPFoundation

/// A downloadable Pharo image template
struct ImageTemplate: Identifiable {
    let id: String          // e.g. "130"
    let label: String       // e.g. "Pharo 13 (latest)"
    let version: String     // e.g. "130"
    let url: URL

    static let builtIn: [ImageTemplate] = [
        ImageTemplate(
            id: "130",
            label: "Pharo 13 (stable)",
            version: "130",
            url: URL(string: "https://files.pharo.org/get-files/130/pharoImage-arm64.zip")!
        ),
        ImageTemplate(
            id: "140",
            label: "Pharo 14 (dev)",
            version: "140",
            url: URL(string: "https://files.pharo.org/get-files/140/pharoImage-arm64.zip")!
        ),
    ]
}

/// Manages a library of Pharo images
@MainActor
class ImageManager: ObservableObject {

    // MARK: - Published Properties

    @Published var images: [PharoImage] = []
    @Published var selectedImageID: UUID?

    @Published var isDownloading = false
    @Published var downloadProgress: Double = 0
    @Published var statusMessage: String?
    @Published var errorMessage: String?

    // MARK: - Backward-compat computed properties (used by ContentView/PharoBridge)

    var hasImage: Bool { selectedImage != nil }

    var imagePath: String? { selectedImage?.imagePath }

    var imageName: String? { selectedImage?.imageFileName }

    var selectedImage: PharoImage? {
        if let id = selectedImageID {
            return images.first { $0.id == id }
        }
        return images.first
    }

    // MARK: - Private Properties

    private var downloadTask: URLSessionDownloadTask?
    private var downloadSession: URLSession?
    private var progressObservation: NSKeyValueObservation?
    private let fileManager = FileManager.default

    private var documentsDirectory: URL {
        guard let docs = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first else {
            fatalError("Documents directory unavailable")
        }
        return docs
    }

    private var catalogURL: URL {
        documentsDirectory.appendingPathComponent("image-library.json")
    }

    // MARK: - Load / Save

    /// Load the image catalog from disk, scan for uncatalogued images, migrate legacy files
    func load() {
        #if DEBUG
        fputs("[LIB] load() starting\n", stderr)
        #endif

        // Ensure Images/ directory exists
        do {
            try fileManager.createDirectory(at: PharoImage.imagesRoot, withIntermediateDirectories: true)
        } catch {
            errorMessage = "Cannot create Images directory: \(error.localizedDescription)"
        }

        // Load catalog JSON
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        if let data = try? Data(contentsOf: catalogURL),
           let saved = try? decoder.decode([PharoImage].self, from: data) {
            images = saved
            #if DEBUG
            fputs("[LIB] loaded \(saved.count) images from catalog\n", stderr)
            #endif
        }

        // Migrate legacy loose .image files from Documents root
        migrateLegacyFiles()

        // Scan for uncatalogued images in Images/ subdirectories
        scanForUncatalogued()

        // Remove catalog entries whose files no longer exist on disk
        images.removeAll { access($0.imagePath, R_OK) != 0 }

        save()

        #if DEBUG
        fputs("[LIB] load() done, \(images.count) images total\n", stderr)
        fflush(stderr)
        #endif
    }

    func save() {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = .prettyPrinted
        do {
            let data = try encoder.encode(images)
            try data.write(to: catalogURL, options: .atomic)
        } catch {
            #if DEBUG
            fputs("[LIB] save() failed: \(error)\n", stderr)
            #endif
        }
    }

    // MARK: - Migration

    /// Move loose .image files from Documents/ root into Images/<slug>/
    private func migrateLegacyFiles() {
        let docsPath = documentsDirectory.path
        guard let dp = opendir(docsPath) else { return }
        defer { closedir(dp) }

        while let entry = readdir(dp) {
            let name = withUnsafePointer(to: entry.pointee.d_name) { ptr in
                String(cString: UnsafeRawPointer(ptr).assumingMemoryBound(to: CChar.self))
            }
            guard name.hasSuffix(".image") else { continue }
            guard !name.hasPrefix(".") else { continue }

            let imageURL = documentsDirectory.appendingPathComponent(name)
            // Already catalogued?
            if images.contains(where: { $0.imageURL == imageURL }) { continue }

            let baseName = (name as NSString).deletingPathExtension
            let slug = makeSlug(from: baseName)
            let destDir = PharoImage.imagesRoot.appendingPathComponent(slug, isDirectory: true)
            try? fileManager.createDirectory(at: destDir, withIntermediateDirectories: true)

            // Move the .image file and any companion files (.changes, .sources)
            for ext in ["image", "changes", "sources"] {
                let src = documentsDirectory.appendingPathComponent("\(baseName).\(ext)")
                let dst = destDir.appendingPathComponent("\(baseName).\(ext)")
                if fileManager.fileExists(atPath: src.path) {
                    try? fileManager.moveItem(at: src, to: dst)
                    #if DEBUG
                    fputs("[LIB] migrated \(baseName).\(ext) → Images/\(slug)/\n", stderr)
                    #endif
                }
            }

            // Also move any WorkingImage marker
            let workingMarker = documentsDirectory.appendingPathComponent("WorkingImage")
            if fileManager.fileExists(atPath: workingMarker.path) {
                try? fileManager.removeItem(at: workingMarker)
            }

            var entry = PharoImage.create(
                name: baseName,
                directoryName: slug,
                imageFileName: name,
                pharoVersion: guessPharoVersion(from: baseName)
            )
            entry.refreshSize()
            images.append(entry)
        }
    }

    /// Scan Images/ subdirectories for .image files not in the catalog
    private func scanForUncatalogued() {
        let rootPath = PharoImage.imagesRoot.path
        guard let dp = opendir(rootPath) else { return }
        defer { closedir(dp) }

        while let entry = readdir(dp) {
            let dirName = withUnsafePointer(to: entry.pointee.d_name) { ptr in
                String(cString: UnsafeRawPointer(ptr).assumingMemoryBound(to: CChar.self))
            }
            guard !dirName.hasPrefix(".") else { continue }
            guard entry.pointee.d_type == DT_DIR else { continue }

            // Already catalogued for this directory?
            if images.contains(where: { $0.directoryName == dirName }) { continue }

            // Look for a .image file inside
            let subDirURL = PharoImage.imagesRoot.appendingPathComponent(dirName)
            if let imageFileName = findFirstImageFile(in: subDirURL) {
                var img = PharoImage.create(
                    name: dirName,
                    directoryName: dirName,
                    imageFileName: imageFileName,
                    pharoVersion: guessPharoVersion(from: imageFileName)
                )
                img.refreshSize()
                images.append(img)
                #if DEBUG
                fputs("[LIB] found uncatalogued image: \(dirName)/\(imageFileName)\n", stderr)
                #endif
            }
        }
    }

    // MARK: - Download

    /// Download a template image
    func downloadTemplate(_ template: ImageTemplate) {
        downloadImage(from: template.url, version: template.version, label: template.label)
    }

    /// Download from a custom URL
    func downloadCustomURL(_ url: URL) {
        downloadImage(from: url, version: nil, label: url.lastPathComponent)
    }

    /// Legacy compatibility: download the default image
    func downloadDefaultImage() {
        if let template = ImageTemplate.builtIn.first {
            downloadTemplate(template)
        }
    }

    /// Legacy compatibility: download by version key
    func downloadImage(version: String) {
        if let template = ImageTemplate.builtIn.first(where: { $0.version == version }) {
            downloadTemplate(template)
        } else {
            errorMessage = "Unknown Pharo version: \(version)"
        }
    }

    private func downloadImage(from url: URL, version: String?, label: String) {
        guard !isDownloading else {
            errorMessage = "Download already in progress"
            return
        }

        isDownloading = true
        downloadProgress = 0
        statusMessage = "Starting download..."
        errorMessage = nil

        let session = URLSession(configuration: .default)
        downloadSession = session
        let capturedVersion = version
        downloadTask = session.downloadTask(with: url) { [weak self] tempURL, response, error in
            Task { @MainActor in
                self?.handleDownloadComplete(tempURL: tempURL, response: response, error: error, version: capturedVersion, label: label)
            }
        }

        progressObservation = downloadTask?.progress.observe(\.fractionCompleted) { [weak self] progress, _ in
            Task { @MainActor in
                self?.downloadProgress = progress.fractionCompleted
                self?.statusMessage = "Downloading... \(Int(progress.fractionCompleted * 100))%"
            }
        }

        downloadTask?.resume()
    }

    func cancelDownload() {
        downloadTask?.cancel()
        downloadTask = nil
        downloadSession?.invalidateAndCancel()
        downloadSession = nil
        progressObservation = nil
        isDownloading = false
        statusMessage = nil
    }

    // MARK: - Import

    /// Import a .image file (and companions) from a security-scoped URL (Files app)
    func importImage(from url: URL) {
        let gotAccess = url.startAccessingSecurityScopedResource()
        defer { if gotAccess { url.stopAccessingSecurityScopedResource() } }

        let baseName = (url.lastPathComponent as NSString).deletingPathExtension
        let slug = makeSlug(from: baseName) + "-" + UUID().uuidString.prefix(4).lowercased()
        let destDir = PharoImage.imagesRoot.appendingPathComponent(slug, isDirectory: true)

        do {
            try fileManager.createDirectory(at: destDir, withIntermediateDirectories: true)

            // Copy the .image file
            let destImage = destDir.appendingPathComponent(url.lastPathComponent)
            try fileManager.copyItem(at: url, to: destImage)

            // Copy companion files (.changes, .sources) from same directory
            let sourceDir = url.deletingLastPathComponent()
            for ext in ["changes", "sources"] {
                let companion = sourceDir.appendingPathComponent("\(baseName).\(ext)")
                if fileManager.fileExists(atPath: companion.path) {
                    let dest = destDir.appendingPathComponent("\(baseName).\(ext)")
                    try fileManager.copyItem(at: companion, to: dest)
                }
            }

            var entry = PharoImage.create(
                name: baseName,
                directoryName: slug,
                imageFileName: url.lastPathComponent,
                pharoVersion: guessPharoVersion(from: baseName)
            )
            entry.refreshSize()
            images.append(entry)
            selectedImageID = entry.id
            save()
            #if DEBUG
            fputs("[LIB] imported image: \(slug)/\(url.lastPathComponent)\n", stderr)
            #endif
        } catch {
            errorMessage = "Import failed: \(error.localizedDescription)"
        }
    }

    // MARK: - Delete

    func deleteImage(_ image: PharoImage) {
        // Remove from disk
        try? fileManager.removeItem(at: image.directoryURL)

        // Clear auto-launch if this was the auto-launch image
        if UserDefaults.standard.string(forKey: "autoLaunchImageID") == image.id.uuidString {
            UserDefaults.standard.removeObject(forKey: "autoLaunchImageID")
        }

        // Remove from catalog
        images.removeAll { $0.id == image.id }
        if selectedImageID == image.id {
            selectedImageID = images.first?.id
        }
        save()
        #if DEBUG
        fputs("[LIB] deleted image: \(image.directoryName)\n", stderr)
        #endif
    }

    // MARK: - Rename

    func renameImage(_ image: PharoImage, to newName: String) {
        guard let idx = images.firstIndex(where: { $0.id == image.id }) else { return }
        images[idx].name = newName
        save()
    }

    // MARK: - Duplicate

    func duplicateImage(_ image: PharoImage) {
        let slug = makeSlug(from: image.name + "-copy") + "-" + UUID().uuidString.prefix(4).lowercased()
        let destDir = PharoImage.imagesRoot.appendingPathComponent(slug, isDirectory: true)

        do {
            try fileManager.copyItem(at: image.directoryURL, to: destDir)

            var entry = PharoImage.create(
                name: "\(image.name) (copy)",
                directoryName: slug,
                imageFileName: image.imageFileName,
                pharoVersion: image.pharoVersion
            )
            entry.refreshSize()
            images.append(entry)
            save()
            #if DEBUG
            fputs("[LIB] duplicated image: \(image.directoryName) → \(slug)\n", stderr)
            #endif
        } catch {
            errorMessage = "Duplicate failed: \(error.localizedDescription)"
        }
    }

    // MARK: - Total Size

    /// Sum all file sizes in the image's directory
    func totalSizeForImage(_ image: PharoImage) -> Int64? {
        guard let contents = try? fileManager.contentsOfDirectory(
            at: image.directoryURL, includingPropertiesForKeys: [.fileSizeKey]
        ) else { return nil }

        var total: Int64 = 0
        for url in contents {
            if let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize {
                total += Int64(size)
            }
        }
        return total > 0 ? total : nil
    }

    // MARK: - Launch tracking

    func markLaunched(_ image: PharoImage) {
        guard let idx = images.firstIndex(where: { $0.id == image.id }) else { return }
        images[idx].lastLaunchedAt = Date()
        selectedImageID = image.id
        save()
    }

    // MARK: - Download Completion

    private func handleDownloadComplete(tempURL: URL?, response: URLResponse?, error: Error?, version: String?, label: String) {
        defer {
            isDownloading = false
            progressObservation = nil
            downloadTask = nil
            downloadSession?.finishTasksAndInvalidate()
            downloadSession = nil
        }

        if let error = error {
            if (error as NSError).code == NSURLErrorCancelled {
                statusMessage = "Download cancelled"
            } else {
                errorMessage = "Download failed: \(error.localizedDescription)"
            }
            return
        }

        guard let tempURL = tempURL else {
            errorMessage = "No file received"
            return
        }

        statusMessage = "Extracting..."

        do {
            let slug = makeSlug(from: label) + "-" + UUID().uuidString.prefix(4).lowercased()
            let destDir = PharoImage.imagesRoot.appendingPathComponent(slug, isDirectory: true)
            try fileManager.createDirectory(at: destDir, withIntermediateDirectories: true)

            try extractImage(from: tempURL, to: destDir)

            // Clean up the download temp file (50-70MB ZIP sitting in tmp/)
            try? fileManager.removeItem(at: tempURL)

            // Find the extracted .image file
            if let imageFileName = findFirstImageFile(in: destDir) {
                // Demote any existing images that still carry this template label.
                // E.g., if you download "Pharo 13 (latest)" again, the previous one
                // reverts to its actual image filename so you don't get duplicates.
                demoteMatchingNames(label)

                var entry = PharoImage.create(
                    name: label,
                    directoryName: slug,
                    imageFileName: imageFileName,
                    pharoVersion: version ?? guessPharoVersion(from: imageFileName)
                )
                entry.refreshSize()
                images.append(entry)
                selectedImageID = entry.id
                save()
                statusMessage = nil
                #if DEBUG
                fputs("[LIB] downloaded and catalogued: \(slug)/\(imageFileName)\n", stderr)
                #endif
            } else {
                errorMessage = "No .image file found in download"
                try? fileManager.removeItem(at: destDir)
            }
        } catch {
            errorMessage = "Extraction failed: \(error.localizedDescription)"
        }
    }

    // MARK: - Extraction

    private func extractImage(from zipURL: URL, to destination: URL) throws {
        let zipData = try Data(contentsOf: zipURL, options: .mappedIfSafe)
        let isZip = zipData.prefix(4) == Data([0x50, 0x4B, 0x03, 0x04])

        if isZip {
            try extractWithZIPFoundation(from: zipURL, to: destination)
        } else {
            // Not a zip — assume it's the image directly
            let destPath = destination.appendingPathComponent("Pharo.image")
            try fileManager.copyItem(at: zipURL, to: destPath)
        }
    }

    private func extractWithZIPFoundation(from zipURL: URL, to destination: URL) throws {
        try fileManager.createDirectory(at: destination, withIntermediateDirectories: true)
        try fileManager.unzipItem(at: zipURL, to: destination)
    }

    // MARK: - Helpers

    /// Find the first .image file in a directory using POSIX opendir (no file coordination)
    private func findFirstImageFile(in directory: URL) -> String? {
        let dirPath = directory.path
        guard let dp = opendir(dirPath) else { return nil }
        defer { closedir(dp) }

        while let entry = readdir(dp) {
            let name = withUnsafePointer(to: entry.pointee.d_name) { ptr in
                String(cString: UnsafeRawPointer(ptr).assumingMemoryBound(to: CChar.self))
            }
            if name.hasSuffix(".image") && !name.hasPrefix(".") {
                // Verify readable
                let fullPath = directory.appendingPathComponent(name).path
                if access(fullPath, R_OK) == 0 {
                    return name
                }
            }
        }
        return nil
    }

    /// Rename any existing images whose name matches `label` to their actual
    /// image filename (minus extension). Called before adding a new download so
    /// that only the newest entry keeps the template label like "Pharo 13 (latest)".
    private func demoteMatchingNames(_ label: String) {
        for i in images.indices where images[i].name == label {
            let baseName = (images[i].imageFileName as NSString).deletingPathExtension
            images[i].name = baseName
        }
    }

    /// Create a filesystem-safe slug from a name
    private func makeSlug(from name: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        let slug = name.unicodeScalars.map { allowed.contains($0) ? String($0) : "-" }.joined()
        // Collapse multiple hyphens and trim
        return slug.components(separatedBy: "-").filter { !$0.isEmpty }.joined(separator: "-").prefix(60).lowercased()
    }

    /// Guess Pharo version from a filename like "Pharo12.0-SNAPSHOT-64bit-..."
    private func guessPharoVersion(from name: String) -> String? {
        guard let regex = try? NSRegularExpression(pattern: "[Pp]haro\\s*(\\d{1,2})", options: []),
              let match = regex.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)),
              let range = Range(match.range(at: 1), in: name) else {
            return nil
        }
        let major = String(name[range])
        return "\(major)0"  // e.g. "12" → "120"
    }

    // MARK: - Legacy compat

}
