/*
 * PharoImage.swift
 *
 * Model for a single Pharo image in the image library.
 * Each image lives in its own subdirectory under Documents/Images/.
 */

import Foundation

struct PharoImage: Codable, Identifiable, Equatable {

    var id: UUID
    var name: String
    /// Subdirectory name under Images/ (slug)
    var directoryName: String
    /// Actual .image filename inside the directory
    var imageFileName: String
    var pharoVersion: String?
    var createdAt: Date
    var lastLaunchedAt: Date?
    var imageSizeBytes: Int64?

    // MARK: - Paths

    /// Root directory for all images: Documents/Images/
    static var imagesRoot: URL {
        guard let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            fatalError("Documents directory unavailable")
        }
        return docs.appendingPathComponent("Images", isDirectory: true)
    }

    /// This image's directory: Documents/Images/<directoryName>/
    var directoryURL: URL {
        Self.imagesRoot.appendingPathComponent(directoryName, isDirectory: true)
    }

    /// Full path to the .image file
    var imageURL: URL {
        directoryURL.appendingPathComponent(imageFileName)
    }

    /// Absolute path string for passing to the VM
    var imagePath: String {
        imageURL.path
    }

    // MARK: - Convenience Initializer

    /// Create a new PharoImage entry for a freshly downloaded/imported image
    static func create(
        name: String,
        directoryName: String,
        imageFileName: String,
        pharoVersion: String? = nil
    ) -> PharoImage {
        PharoImage(
            id: UUID(),
            name: name,
            directoryName: directoryName,
            imageFileName: imageFileName,
            pharoVersion: pharoVersion,
            createdAt: Date(),
            lastLaunchedAt: nil,
            imageSizeBytes: nil
        )
    }

    /// Update the file size from disk
    mutating func refreshSize() {
        let attrs = try? FileManager.default.attributesOfItem(atPath: imagePath)
        imageSizeBytes = attrs?[.size] as? Int64
    }

    /// Human-readable version label (e.g., "130" -> "Pharo 13")
    var versionLabel: String {
        guard let version = pharoVersion else { return "—" }
        switch version {
        case "140": return "Pharo 14"
        case "130": return "Pharo 13"
        case "120": return "Pharo 12"
        case "110": return "Pharo 11"
        case "100": return "Pharo 10"
        default: return "Pharo \(version)"
        }
    }

    /// File modification date of the .image file on disk
    var fileModificationDate: Date? {
        let attrs = try? FileManager.default.attributesOfItem(atPath: imagePath)
        return attrs?[.modificationDate] as? Date
    }

    /// Human-readable file size
    var formattedSize: String? {
        guard let bytes = imageSizeBytes else { return nil }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }
}
