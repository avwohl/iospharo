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
    /// Whether to apply iOS-specific patches (menu bar overflow, etc.) on launch
    var applyIOSPatches: Bool

    // Custom Codable to default applyIOSPatches=true for existing catalog entries
    enum CodingKeys: String, CodingKey {
        case id, name, directoryName, imageFileName, pharoVersion
        case createdAt, lastLaunchedAt, imageSizeBytes, applyIOSPatches
    }

    init(
        id: UUID,
        name: String,
        directoryName: String,
        imageFileName: String,
        pharoVersion: String?,
        createdAt: Date,
        lastLaunchedAt: Date?,
        imageSizeBytes: Int64?,
        applyIOSPatches: Bool = true
    ) {
        self.id = id
        self.name = name
        self.directoryName = directoryName
        self.imageFileName = imageFileName
        self.pharoVersion = pharoVersion
        self.createdAt = createdAt
        self.lastLaunchedAt = lastLaunchedAt
        self.imageSizeBytes = imageSizeBytes
        self.applyIOSPatches = applyIOSPatches
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = try container.decode(UUID.self, forKey: .id)
        name = try container.decode(String.self, forKey: .name)
        directoryName = try container.decode(String.self, forKey: .directoryName)
        imageFileName = try container.decode(String.self, forKey: .imageFileName)
        pharoVersion = try container.decodeIfPresent(String.self, forKey: .pharoVersion)
        createdAt = try container.decode(Date.self, forKey: .createdAt)
        lastLaunchedAt = try container.decodeIfPresent(Date.self, forKey: .lastLaunchedAt)
        imageSizeBytes = try container.decodeIfPresent(Int64.self, forKey: .imageSizeBytes)
        applyIOSPatches = try container.decodeIfPresent(Bool.self, forKey: .applyIOSPatches) ?? true
    }

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
            imageSizeBytes: nil,
            applyIOSPatches: true
        )
    }

    /// Update the file size from disk
    mutating func refreshSize() {
        let attrs = try? FileManager.default.attributesOfItem(atPath: imagePath)
        imageSizeBytes = attrs?[.size] as? Int64
    }

    /// Human-readable file size
    var formattedSize: String? {
        guard let bytes = imageSizeBytes else { return nil }
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }
}
