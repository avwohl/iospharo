/*
 * ImageLibraryView.swift
 *
 * Main screen showing the image library. Lists all downloaded/imported images,
 * provides download and import actions, and allows launching or deleting images.
 */

import SwiftUI
import UniformTypeIdentifiers

enum SortOrder: String, CaseIterable {
    case name = "Name"
    case dateCreated = "Date Added"
    case lastUsed = "Last Used"
    case size = "Size"
}

struct ImageLibraryView: View {
    @EnvironmentObject var imageManager: ImageManager
    @EnvironmentObject var bridge: PharoBridge

    @State private var showingNewImage = false
    @State private var showingFileImporter = false
    @State private var showingSettings = false
    @State private var imageToDelete: PharoImage?
    @State private var sortOrder: SortOrder = .name
    @State private var imageToRename: PharoImage?
    @State private var renameText: String = ""
    @State private var imageToShare: PharoImage?

    private var sortedImages: [PharoImage] {
        switch sortOrder {
        case .name:
            return imageManager.images.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        case .dateCreated:
            return imageManager.images.sorted { $0.createdAt > $1.createdAt }
        case .lastUsed:
            return imageManager.images.sorted {
                ($0.lastLaunchedAt ?? .distantPast) > ($1.lastLaunchedAt ?? .distantPast)
            }
        case .size:
            return imageManager.images.sorted {
                ($0.imageSizeBytes ?? 0) > ($1.imageSizeBytes ?? 0)
            }
        }
    }

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
                    HStack(spacing: 12) {
                        Button {
                            showingSettings = true
                        } label: {
                            Image(systemName: "gear")
                        }

                        Menu {
                            Picker("Sort by", selection: $sortOrder) {
                                ForEach(SortOrder.allCases, id: \.self) { order in
                                    Text(order.rawValue).tag(order)
                                }
                            }
                        } label: {
                            Image(systemName: "line.3.horizontal.decrease.circle")
                        }
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
            .alert("Rename Image", isPresented: .init(
                get: { imageToRename != nil },
                set: { if !$0 { imageToRename = nil } }
            )) {
                TextField("Name", text: $renameText)
                Button("Rename") {
                    if let image = imageToRename, !renameText.isEmpty {
                        imageManager.renameImage(image, to: renameText)
                    }
                    imageToRename = nil
                }
                Button("Cancel", role: .cancel) {
                    imageToRename = nil
                }
            } message: {
                Text("Enter a new name for this image.")
            }
            .sheet(item: $imageToShare) { image in
                ShareSheet(activityItems: [image.directoryURL])
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

            disclaimerBlock

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
                ForEach(sortedImages) { image in
                    Button {
                        launchImage(image)
                    } label: {
                        ImageRow(image: image)
                    }
                    .contextMenu {
                        Button {
                            launchImage(image)
                        } label: {
                            Label("Launch", systemImage: "play.fill")
                        }

                        Button {
                            renameText = image.name
                            imageToRename = image
                        } label: {
                            Label("Rename", systemImage: "pencil")
                        }

                        Button {
                            imageManager.duplicateImage(image)
                        } label: {
                            Label("Duplicate", systemImage: "doc.on.doc")
                        }

                        Button {
                            imageToShare = image
                        } label: {
                            Label("Share", systemImage: "square.and.arrow.up")
                        }

                        Divider()

                        Button(role: .destructive) {
                            imageToDelete = image
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
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

            Section {
                disclaimerBlock
            }
            .listRowBackground(Color.clear)
        }
    }

    // MARK: - Disclaimer

    private var disclaimerBlock: some View {
        VStack(spacing: 4) {
            HStack(spacing: 0) {
                Text("Experimental release — not affiliated with or endorsed by ")
                    .font(.caption)
                    .foregroundColor(.secondary)
                Link("Pharo.org", destination: URL(string: "https://pharo.org")!)
                    .font(.caption)
            }
            .multilineTextAlignment(.center)
            HStack(spacing: 12) {
                Link("GitHub", destination: URL(string: "https://github.com/avwohl/iospharo")!)
                Text("·")
                    .foregroundColor(.secondary)
                Link("Report a Bug", destination: URL(string: "https://github.com/avwohl/iospharo/issues")!)
            }
            .font(.caption)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 8)
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

// MARK: - Share Sheet (UIActivityViewController wrapper)

struct ShareSheet: UIViewControllerRepresentable {
    let activityItems: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: activityItems, applicationActivities: nil)
    }

    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}
