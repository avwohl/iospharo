/*
 * ImageRow.swift
 *
 * A single row in the image library list showing image name, version, size, and last used.
 */

import SwiftUI

struct ImageRow: View {
    let image: PharoImage

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "cube.box.fill")
                .font(.title2)
                .foregroundColor(.blue)
                .frame(width: 36)

            VStack(alignment: .leading, spacing: 4) {
                Text(image.name)
                    .font(.body)
                    .lineLimit(1)

                HStack(spacing: 8) {
                    if let version = image.pharoVersion {
                        Text(versionLabel(version))
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }

                    if let size = image.formattedSize {
                        Text(size)
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }

                    if let lastUsed = image.lastLaunchedAt {
                        Text(lastUsed, style: .relative)
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
            }

            Spacer()

            Image(systemName: "chevron.right")
                .font(.caption)
                .foregroundColor(.secondary.opacity(0.5))
        }
        .padding(.vertical, 4)
    }

    private func versionLabel(_ version: String) -> String {
        switch version {
        case "130": return "Pharo 13"
        case "120": return "Pharo 12"
        case "110": return "Pharo 11"
        case "100": return "Pharo 10"
        default: return "Pharo \(version)"
        }
    }
}
