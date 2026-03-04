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
    var isAutoLaunch: Bool = false

    var body: some View {
        HStack(spacing: 0) {
            // Name column (flexible)
            HStack(spacing: 4) {
                if isAutoLaunch {
                    Image(systemName: "star.fill")
                        .font(.system(size: 10))
                        .foregroundColor(.orange)
                }
                Text(image.name)
                    .font(.system(.body, design: .default))
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            // Version column
            Text(image.versionLabel)
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
        if let date = image.fileModificationDate {
            return relativeDate(date)
        }
        return "—"
    }

    private func relativeDate(_ date: Date) -> String {
        let formatter = RelativeDateTimeFormatter()
        formatter.unitsStyle = .full
        return formatter.localizedString(for: date, relativeTo: Date())
    }

}

/// Shared column width constants for header and rows
enum ImageTableLayout {
    static let versionWidth: CGFloat = 90
    static let sizeWidth: CGFloat = 70
    static let lastModifiedWidth: CGFloat = 120
}
