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

    /// Check for existing image in Documents
    func checkForExistingImage() {
        let imageFiles = findImageFiles()

        if let firstImage = imageFiles.first {
            imagePath = firstImage.path
            imageName = firstImage.lastPathComponent
            hasImage = true
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
        do {
            let contents = try fileManager.contentsOfDirectory(
                at: documentsDirectory,
                includingPropertiesForKeys: [.isRegularFileKey],
                options: .skipsHiddenFiles
            )

            return contents.filter { $0.pathExtension == "image" }
                .sorted { $0.lastPathComponent < $1.lastPathComponent }
        } catch {
            return []
        }
    }
}

// MARK: - Minimal ZIP Extraction

extension ImageManager {

    /// Extract a ZIP archive manually (simplified implementation)
    /// In production, use ZIPFoundation package
    func simpleUnzip(data: Data, to destination: URL) throws {
        // ZIP file format constants
        let localFileHeaderSignature: UInt32 = 0x04034b50

        var offset = 0

        while offset + 30 < data.count {
            // Read signature
            let signature = data.subdata(in: offset..<offset+4).withUnsafeBytes {
                $0.load(as: UInt32.self)
            }

            guard signature == localFileHeaderSignature else {
                break
            }

            // Parse local file header
            let compressedSize = Int(data.subdata(in: offset+18..<offset+22).withUnsafeBytes {
                $0.load(as: UInt32.self)
            })
            let _ = Int(data.subdata(in: offset+22..<offset+26).withUnsafeBytes {
                $0.load(as: UInt32.self)
            })
            let fileNameLength = Int(data.subdata(in: offset+26..<offset+28).withUnsafeBytes {
                $0.load(as: UInt16.self)
            })
            let extraFieldLength = Int(data.subdata(in: offset+28..<offset+30).withUnsafeBytes {
                $0.load(as: UInt16.self)
            })

            // Get filename
            let fileNameData = data.subdata(in: offset+30..<offset+30+fileNameLength)
            guard let fileName = String(data: fileNameData, encoding: .utf8) else {
                offset += 30 + fileNameLength + extraFieldLength + compressedSize
                continue
            }

            // Skip directories
            guard !fileName.hasSuffix("/") else {
                offset += 30 + fileNameLength + extraFieldLength + compressedSize
                continue
            }

            // Extract only image-related files
            let ext = (fileName as NSString).pathExtension.lowercased()
            if ext == "image" || ext == "changes" || ext == "sources" {
                let dataStart = offset + 30 + fileNameLength + extraFieldLength
                let fileData = data.subdata(in: dataStart..<dataStart+compressedSize)

                // For uncompressed files (method 0), write directly
                // For deflated files (method 8), would need decompression
                let compressionMethod = data.subdata(in: offset+8..<offset+10).withUnsafeBytes {
                    $0.load(as: UInt16.self)
                }

                if compressionMethod == 0 {
                    let destFile = destination.appendingPathComponent(
                        (fileName as NSString).lastPathComponent
                    )
                    try fileData.write(to: destFile)
                }
            }

            offset += 30 + fileNameLength + extraFieldLength + compressedSize
        }
    }
}
