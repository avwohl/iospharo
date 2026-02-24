/*
 * DownloadProgressRow.swift
 *
 * Inline progress indicator shown in the image library while a download is active.
 */

import SwiftUI

struct DownloadProgressRow: View {
    @EnvironmentObject var imageManager: ImageManager

    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
                .frame(width: 36)

            VStack(alignment: .leading, spacing: 4) {
                Text(imageManager.statusMessage ?? "Downloading...")
                    .font(.body)

                ProgressView(value: imageManager.downloadProgress)
                    .progressViewStyle(LinearProgressViewStyle(tint: .blue))
            }

            Button(role: .destructive) {
                imageManager.cancelDownload()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .foregroundColor(.secondary)
            }
            .buttonStyle(.plain)
        }
        .padding(.vertical, 4)
    }
}
