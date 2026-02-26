/*
 * ImageRow.swift
 *
 * A table-style row in the image library, with columns aligned to the header.
 * Columns: Name (flexible) | Version (fixed) | Size (fixed) | Last Modified (fixed)
 */

import SwiftUI

struct ImageRow: View {
    let image: PharoImage
    let isSelected: Bool

    var body: some View {
        HStack(spacing: 0) {
            // Name column (flexible)
            Text(image.name)
                .font(.system(.body, design: .default))
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: .infinity, alignment: .leading)

            // Version column
            Text(versionLabel(image.pharoVersion))
                .font(.system(.body, design: .default))
                .foregroundColor(.secondary)
                .frame(width: ImageTableLayout.versionWidth, alignment: .leading)

            // Size column
            Text(image.formattedSize ?? "—")
                .font(.system(.body, design: .default))
                .foregroundColor(.secondary)
                .frame(width: ImageTableLayout.sizeWidth, alignment: .trailing)

            // Last modified column
            Text(lastModifiedText)
                .font(.system(.body, design: .default))
                .foregroundColor(.secondary)
                .frame(width: ImageTableLayout.lastModifiedWidth, alignment: .leading)
                .padding(.leading, 12)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(isSelected ? Color.accentColor.opacity(0.15) : Color.clear)
        .contentShape(Rectangle())
    }

    private var lastModifiedText: String {
        if let date = image.lastLaunchedAt {
            return relativeDate(date)
        }
        return relativeDate(image.createdAt)
    }

    private func relativeDate(_ date: Date) -> String {
        let formatter = RelativeDateTimeFormatter()
        formatter.unitsStyle = .full
        return formatter.localizedString(for: date, relativeTo: Date())
    }

    private func versionLabel(_ version: String?) -> String {
        guard let version = version else { return "—" }
        switch version {
        case "130": return "Pharo 13"
        case "120": return "Pharo 12"
        case "110": return "Pharo 11"
        case "100": return "Pharo 10"
        default: return "Pharo \(version)"
        }
    }
}

/// Shared column width constants for header and rows
enum ImageTableLayout {
    static let versionWidth: CGFloat = 90
    static let sizeWidth: CGFloat = 70
    static let lastModifiedWidth: CGFloat = 120
}
