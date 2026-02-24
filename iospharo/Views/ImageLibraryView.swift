/*
 * ImageLibraryView.swift
 *
 * Main screen showing the image library. Lists all downloaded/imported images,
 * provides download and import actions, and allows launching or deleting images.
 */

import SwiftUI
import UniformTypeIdentifiers

struct ImageLibraryView: View {
    @EnvironmentObject var imageManager: ImageManager
    @EnvironmentObject var bridge: PharoBridge

    @State private var showingNewImage = false
    @State private var showingFileImporter = false
    @State private var showingSettings = false
    @State private var imageToDelete: PharoImage?

    var body: some View {
        NavigationView {
            Group {
                if imageManager.images.isEmpty && !imageManager.isDownloading {
                    emptyState
                } else {
                    imageList
                }
            }
            .navigationTitle("Pharo Images")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Menu {
                        Button {
                            showingNewImage = true
                        } label: {
                            Label("Download New", systemImage: "arrow.down.circle")
                        }

                        Button {
                            showingFileImporter = true
                        } label: {
                            Label("Import from Files", systemImage: "folder")
                        }
                    } label: {
                        Image(systemName: "plus")
                    }
                }

                ToolbarItem(placement: .navigationBarLeading) {
                    Button {
                        showingSettings = true
                    } label: {
                        Image(systemName: "gear")
                    }
                }
            }
            .sheet(isPresented: $showingNewImage) {
                NewImageView()
            }
            .sheet(isPresented: $showingSettings) {
                SettingsView()
            }
            .fileImporter(
                isPresented: $showingFileImporter,
                allowedContentTypes: [UTType(filenameExtension: "image") ?? .data],
                allowsMultipleSelection: false
            ) { result in
                switch result {
                case .success(let urls):
                    if let url = urls.first {
                        imageManager.importImage(from: url)
                    }
                case .failure(let error):
                    imageManager.errorMessage = "Import failed: \(error.localizedDescription)"
                }
            }
            .alert("Delete Image?", isPresented: .init(
                get: { imageToDelete != nil },
                set: { if !$0 { imageToDelete = nil } }
            )) {
                Button("Delete", role: .destructive) {
                    if let image = imageToDelete {
                        imageManager.deleteImage(image)
                    }
                    imageToDelete = nil
                }
                Button("Cancel", role: .cancel) {
                    imageToDelete = nil
                }
            } message: {
                if let image = imageToDelete {
                    Text("This will permanently remove \"\(image.name)\" and all its files.")
                }
            }
        }
        // Error banner
        .overlay(alignment: .bottom) {
            if let error = imageManager.errorMessage {
                HStack {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.yellow)
                    Text(error)
                        .font(.caption)
                }
                .padding()
                .background(.ultraThinMaterial)
                .cornerRadius(10)
                .padding()
                .onTapGesture {
                    imageManager.errorMessage = nil
                }
            }
        }
    }

    // MARK: - Empty State

    private var emptyState: some View {
        VStack(spacing: 24) {
            Spacer()

            Image(systemName: "cube.box")
                .font(.system(size: 64))
                .foregroundColor(.secondary)

            Text("No Pharo Images")
                .font(.title2)
                .foregroundColor(.primary)

            Text("Download a Pharo image to get started")
                .font(.body)
                .foregroundColor(.secondary)

            VStack(spacing: 12) {
                Button {
                    if let template = ImageTemplate.builtIn.first {
                        imageManager.downloadTemplate(template)
                    }
                } label: {
                    Label("Download Pharo 13", systemImage: "arrow.down.circle.fill")
                        .font(.headline)
                        .frame(maxWidth: 280)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)

                Button {
                    showingFileImporter = true
                } label: {
                    Label("Import from Files", systemImage: "folder")
                        .frame(maxWidth: 280)
                }
                .buttonStyle(.bordered)
                .controlSize(.large)
            }

            Spacer()
        }
        .padding()
    }

    // MARK: - Image List

    private var imageList: some View {
        List {
            if imageManager.isDownloading {
                Section {
                    DownloadProgressRow()
                }
            }

            Section {
                ForEach(imageManager.images) { image in
                    Button {
                        launchImage(image)
                    } label: {
                        ImageRow(image: image)
                    }
                    .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                        Button(role: .destructive) {
                            imageToDelete = image
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
                }
            }
        }
    }

    // MARK: - Actions

    private func launchImage(_ image: PharoImage) {
        imageManager.markLaunched(image)
        imageManager.selectedImageID = image.id
        if bridge.loadImage(at: image.imagePath) {
            bridge.start()
        }
    }
}
