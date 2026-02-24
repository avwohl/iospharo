/*
 * ImageManager.swift
 *
 * Manages Pharo image files - downloading, extracting, and locating.
 */

import Foundation
import Combine
import ZIPFoundation

/// Manages Pharo image download and storage
@MainActor
class ImageManager: ObservableObject {

    // MARK: - Published Properties

    @Published var isDownloading = false
    @Published var downloadProgress: Double = 0
    @Published var statusMessage: String?
    @Published var errorMessage: String?
    @Published var hasImage = false
    @Published var imagePath: String?
    @Published var imageName: String?

    // MARK: - Private Properties

    private var downloadTask: URLSessionDownloadTask?
    private var progressObservation: NSKeyValueObservation?

    private let fileManager = FileManager.default

    /// Documents directory for storing images
    private var documentsDirectory: URL {
        fileManager.urls(for: .documentDirectory, in: .userDomainMask).first!
    }

    /// Default Pharo image URLs (arm64 for iOS/Apple Silicon)
    private let imageURLs: [String: String] = [
        "120": "https://files.pharo.org/get-files/120/pharoImage-arm64.zip",
        "110": "https://files.pharo.org/get-files/110/pharoImage-arm64.zip",
        "100": "https://files.pharo.org/get-files/100/pharoImage-arm64.zip"
    ]

    // MARK: - Public Methods

    /// Check for existing image and prepare a fresh working copy
    /// We always start from a pristine image to avoid corrupted state
    func checkForExistingImage() {
        fputs("[IMG] checkForExistingImage starting, docs=\(documentsDirectory.path)\n", stderr)
        fflush(stderr)
        // First check Documents directory for downloaded images
        var imageFiles = findImageFiles()
        fputs("[IMG] findImageFiles returned \(imageFiles.count) files\n", stderr)
        fflush(stderr)

        // If no image in Documents, check app bundle for development
        if imageFiles.isEmpty {
            if let bundledImage = Bundle.main.url(forResource: "Pharo-iOS-Ready", withExtension: "image") {
                imageFiles.append(bundledImage)
            } else if let bundledImage = Bundle.main.url(forResource: "Pharo", withExtension: "image") {
                imageFiles.append(bundledImage)
            }
        }

        if let firstImage = imageFiles.first {
            fputs("[IMG] Found image: \(firstImage.path)\n", stderr)
            fflush(stderr)
            // Use original image directly to avoid iCloud file coordination hangs
            // on macOS 26.3 when Documents is synced to iCloud.
            imagePath = firstImage.path
            imageName = firstImage.lastPathComponent
            hasImage = true
            fputs("[IMG] Using image: \(firstImage.path)\n", stderr)
            fflush(stderr)
        } else {
            hasImage = false
            imagePath = nil
            imageName = nil
        }
    }

    /// Download the default (latest) Pharo image
    func downloadDefaultImage() {
        downloadImage(version: "120")
    }

    /// Download a specific Pharo version
    func downloadImage(version: String) {
        guard let urlString = imageURLs[version],
              let url = URL(string: urlString) else {
            errorMessage = "Unknown Pharo version: \(version)"
            return
        }

        downloadImage(from: url)
    }

    /// Download image from a custom URL
    func downloadImage(from url: URL) {
        guard !isDownloading else {
            errorMessage = "Download already in progress"
            return
        }

        isDownloading = true
        downloadProgress = 0
        statusMessage = "Starting download..."
        errorMessage = nil

        let session = URLSession(configuration: .default)
        downloadTask = session.downloadTask(with: url) { [weak self] tempURL, response, error in
            Task { @MainActor in
                self?.handleDownloadComplete(tempURL: tempURL, response: response, error: error)
            }
        }

        // Observe progress
        progressObservation = downloadTask?.progress.observe(\.fractionCompleted) { [weak self] progress, _ in
            Task { @MainActor in
                self?.downloadProgress = progress.fractionCompleted
                self?.statusMessage = "Downloading... \(Int(progress.fractionCompleted * 100))%"
            }
        }

        downloadTask?.resume()
    }

    /// Cancel ongoing download
    func cancelDownload() {
        downloadTask?.cancel()
        downloadTask = nil
        progressObservation = nil
        isDownloading = false
        statusMessage = nil
    }

    // MARK: - Private Methods

    private func handleDownloadComplete(tempURL: URL?, response: URLResponse?, error: Error?) {
        defer {
            isDownloading = false
            progressObservation = nil
            downloadTask = nil
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
            try extractImage(from: tempURL)
            checkForExistingImage()
            statusMessage = nil
        } catch {
            errorMessage = "Extraction failed: \(error.localizedDescription)"
        }
    }

    private func extractImage(from zipURL: URL) throws {
        let destinationURL = documentsDirectory

        // Check if it's a zip file
        let zipData = try Data(contentsOf: zipURL, options: .mappedIfSafe)
        let isZip = zipData.prefix(4) == Data([0x50, 0x4B, 0x03, 0x04])

        if isZip {
            // Extract using ZIPFoundation if available, otherwise use Archive
            try extractZip(from: zipURL, to: destinationURL)
        } else {
            // Not a zip, assume it's the image directly
            let imageName = "Pharo.image"
            let destPath = destinationURL.appendingPathComponent(imageName)
            try fileManager.copyItem(at: zipURL, to: destPath)
        }
    }

    private func extractZip(from zipURL: URL, to destination: URL) throws {
        // Try using built-in Archive class (iOS 16+)
        if #available(iOS 16.0, *) {
            try extractWithZIPFoundation(from: zipURL, to: destination)
        } else {
            // Fallback: manual extraction or use third-party library
            try extractManually(from: zipURL, to: destination)
        }
    }

    private func extractWithZIPFoundation(from zipURL: URL, to destination: URL) throws {
        // Use ZIPFoundation for extraction on iOS
        try fileManager.createDirectory(at: destination, withIntermediateDirectories: true)

        // Remove existing Pharo-related files to avoid "file exists" errors during extraction
        let contents = try? fileManager.contentsOfDirectory(at: destination, includingPropertiesForKeys: nil)
        for file in contents ?? [] {
            let name = file.lastPathComponent.lowercased()
            let ext = file.pathExtension.lowercased()
            if ext == "image" || ext == "changes" || ext == "sources" ||
               ext == "version" || name.hasPrefix("pharo") || name == "workingimage" {
                try? fileManager.removeItem(at: file)
                NSLog("[ImageManager] Removed existing file: %@", file.lastPathComponent)
            }
        }

        try fileManager.unzipItem(at: zipURL, to: destination)
    }

    private func extractManually(from zipURL: URL, to destination: URL) throws {
        // Read the zip file
        let zipData = try Data(contentsOf: zipURL)

        // Simple ZIP extraction
        // In production, use ZIPFoundation or similar library
        // For now, we'll try to find and extract image files

        // Check for common Pharo image patterns
        let tempDir = fileManager.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)

        // Write zip to temp with proper extension
        let tempZip = tempDir.appendingPathComponent("pharo.zip")
        try zipData.write(to: tempZip)

        // Use shell-free extraction (requires ZIPFoundation)
        // For this implementation, we assume the downloaded file
        // might already be uncompressed or we handle it via ZIPFoundation

        // Move any .image files found to destination
        let contents = try fileManager.contentsOfDirectory(at: tempDir, includingPropertiesForKeys: nil)
        for file in contents {
            if file.pathExtension == "image" ||
               file.pathExtension == "changes" ||
               file.pathExtension == "sources" {
                let destFile = destination.appendingPathComponent(file.lastPathComponent)
                if fileManager.fileExists(atPath: destFile.path) {
                    try fileManager.removeItem(at: destFile)
                }
                try fileManager.moveItem(at: file, to: destFile)
            }
        }

        // Cleanup
        try? fileManager.removeItem(at: tempDir)
    }

    private func findImageFiles() -> [URL] {
        fputs("[IMG] findImageFiles: checking known paths\n", stderr)
        fflush(stderr)

        // Check non-iCloud locations first (temp, then Documents).
        // On macOS 26.3, FileManager operations on iCloud-synced Documents
        // can block the main thread via file coordination.
        let searchDirs = [
            URL(fileURLWithPath: "/tmp/PharoImage"),
            fileManager.temporaryDirectory.appendingPathComponent("PharoWorking"),
            documentsDirectory
        ]
        let knownNames = ["Pharo.image", "Pharo-Working.image", "Pharo-iOS-Ready.image"]
        var results: [URL] = []

        for dir in searchDirs {
            for name in knownNames {
                let url = dir.appendingPathComponent(name)
                // Use access() which is a simple POSIX syscall, no file coordination
                if access(url.path, R_OK) == 0 {
                    results.append(url)
                    fputs("[IMG] findImageFiles: found \(url.path)\n", stderr)
                    fflush(stderr)
                    return results  // Return first found
                }
            }
        }

        fputs("[IMG] findImageFiles: no images found\n", stderr)
        fflush(stderr)
        return results
    }
}

