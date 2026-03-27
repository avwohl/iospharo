/*
 * NewImageView.swift
 *
 * Template picker sheet for downloading a new Pharo image.
 * Shows official releases and a custom URL option.
 */

import SwiftUI

struct NewImageView: View {
    @EnvironmentObject var imageManager: ImageManager
    @Environment(\.dismiss) var dismiss

    @State private var customURL: String = ""
    @State private var showingCustomURL = false

    var body: some View {
        NavigationView {
            List {
                Section("Official Releases") {
                    ForEach(ImageTemplate.builtIn) { template in
                        Button {
                            imageManager.downloadTemplate(template)
                            dismiss()
                        } label: {
                            HStack {
                                Image(systemName: "arrow.down.circle")
                                    .foregroundColor(.blue)
                                    .frame(width: 30)
                                VStack(alignment: .leading) {
                                    Text(template.label)
                                        .foregroundColor(.primary)
                                }
                                Spacer()
                            }
                        }
                        .disabled(imageManager.isDownloading)
                    }
                }

                Section("Custom") {
                    if showingCustomURL {
                        VStack(spacing: 12) {
                            TextField("https://example.com/image.zip", text: $customURL)
                                .textContentType(.URL)
                                .keyboardType(.URL)
                                .textInputAutocapitalization(.never)
                                .disableAutocorrection(true)

                            Button("Download") {
                                guard let url = URL(string: customURL), !customURL.isEmpty else { return }
                                imageManager.downloadCustomURL(url)
                                dismiss()
                            }
                            .disabled(customURL.isEmpty || imageManager.isDownloading)
                        }
                    } else {
                        Button {
                            showingCustomURL = true
                        } label: {
                            HStack {
                                Image(systemName: "link")
                                    .foregroundColor(.blue)
                                    .frame(width: 30)
                                Text("Download from URL")
                                    .foregroundColor(.primary)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Download Image")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button { dismiss() } label: {
                        #if targetEnvironment(macCatalyst)
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                        #else
                        Text("Cancel")
                        #endif
                    }
                }
            }
        }
    }
}
