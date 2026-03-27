/*
 * ImageLibraryView.swift
 *
 * Main screen showing the image library in a Pharo Launcher-style table layout.
 * Features: search filter, sortable column headers, detail panel, context menu.
 */

import SwiftUI
import UniformTypeIdentifiers

enum SortOrder: String, CaseIterable {
    case name = "Name"
    case version = "Version"
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
    @State private var sortAscending: Bool = true
    @State private var imageToRename: PharoImage?
    @State private var renameText: String = ""
    @State private var imageToShare: PharoImage?
    @State private var filterText: String = ""
    @State private var selectedImageID: UUID?
    @State private var imageToExport: PharoImage?
    @AppStorage("autoLaunchImageID") private var autoLaunchImageID: String?

    private var filteredImages: [PharoImage] {
        let base = filterText.isEmpty
            ? imageManager.images
            : imageManager.images.filter { $0.name.localizedCaseInsensitiveContains(filterText) }

        return base.sorted { a, b in
            let result: Bool
            switch sortOrder {
            case .name:
                result = a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
            case .version:
                result = (a.pharoVersion ?? "") < (b.pharoVersion ?? "")
            case .dateCreated:
                result = a.createdAt < b.createdAt
            case .lastUsed:
                result = (a.fileModificationDate ?? .distantPast) < (b.fileModificationDate ?? .distantPast)
            case .size:
                result = (a.imageSizeBytes ?? 0) < (b.imageSizeBytes ?? 0)
            }
            return sortAscending ? result : !result
        }
    }

    private var selectedImage: PharoImage? {
        guard let id = selectedImageID else { return nil }
        return imageManager.images.first { $0.id == id }
    }

    var body: some View {
        NavigationView {
            VStack(spacing: 0) {
                if imageManager.images.isEmpty && !imageManager.isDownloading {
                    emptyState
                } else {
                    // Project info bar
                    HStack(spacing: 4) {
                        Text("Pharo Smalltalk — Experimental release, not endorsed by")
                            .foregroundColor(.secondary)
                        Link("Pharo.org", destination: URL(string: "https://pharo.org")!)
                        Spacer()
                        Link("GitHub", destination: URL(string: "https://github.com/avwohl/iospharo")!)
                        Text("·").foregroundColor(.secondary)
                        Text("v\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") (\(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"))")
                            .foregroundColor(.secondary)
                        Link("Changes", destination: URL(string: "https://github.com/avwohl/iospharo/blob/main/docs/changes.md")!)
                        Text("·").foregroundColor(.secondary)
                        Link("Report a Bug", destination: URL(string: "https://github.com/avwohl/iospharo/issues")!)
                    }
                    .font(.caption)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 4)
                    .background(Color(.secondarySystemBackground))

                    // Search filter bar
                    HStack {
                        Image(systemName: "magnifyingglass")
                            .foregroundColor(.secondary)
                        TextField("Filter images...", text: $filterText)
                            .textFieldStyle(.plain)
                        if !filterText.isEmpty {
                            Button {
                                filterText = ""
                            } label: {
                                Image(systemName: "xmark.circle.fill")
                                    .foregroundColor(.secondary)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 8)
                    .background(Color(.systemBackground))

                    Divider()

                    // Column headers
                    columnHeaders

                    Divider()

                    // Download progress
                    if imageManager.isDownloading {
                        DownloadProgressRow()
                            .padding(.horizontal, 12)
                            .padding(.vertical, 6)
                        Divider()
                    }

                    // Image table
                    imageTable

                    Divider()

                    // Detail panel
                    detailPanel
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
            #if targetEnvironment(macCatalyst)
            .sheet(item: $imageToExport) { image in
                ExportAppSheet(image: image)
            }
            #endif
        }
        .navigationViewStyle(.stack)
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

    // MARK: - Column Headers

    private var columnHeaders: some View {
        HStack(spacing: 0) {
            columnHeaderButton("Name", order: .name)
                .frame(maxWidth: .infinity, alignment: .leading)

            columnHeaderButton("Version", order: .version)
                .frame(width: ImageTableLayout.versionWidth, alignment: .leading)

            columnHeaderButton("Size", order: .size)
                .frame(width: ImageTableLayout.sizeWidth, alignment: .trailing)

            columnHeaderButton("Last Modified", order: .lastUsed)
                .frame(width: ImageTableLayout.lastModifiedWidth, alignment: .leading)
                .padding(.leading, 12)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Color(.secondarySystemBackground))
    }

    private func columnHeaderButton(_ title: String, order: SortOrder) -> some View {
        Button {
            if sortOrder == order {
                sortAscending.toggle()
            } else {
                sortOrder = order
                sortAscending = true
            }
        } label: {
            HStack(spacing: 2) {
                Text(title)
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.primary)
                if sortOrder == order {
                    Image(systemName: sortAscending ? "chevron.up" : "chevron.down")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundColor(.accentColor)
                }
            }
        }
        .buttonStyle(.plain)
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
                ForEach(ImageTemplate.builtIn) { template in
                    Button {
                        imageManager.downloadTemplate(template)
                    } label: {
                        Label("Download \(template.label)", systemImage: "arrow.down.circle.fill")
                            .font(.headline)
                            .frame(maxWidth: 280)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                }

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

    // MARK: - Image Table

    private var imageTable: some View {
        ScrollView {
            LazyVStack(spacing: 0) {
                ForEach(filteredImages) { image in
                    ImageRow(
                        image: image,
                        isSelected: image.id == selectedImageID,
                        isAutoLaunch: autoLaunchImageID == image.id.uuidString
                    )
                        .onTapGesture {
                            selectedImageID = image.id
                        }
                        .contextMenu {
                            Button {
                                launchImage(image)
                            } label: {
                                Label("Launch", systemImage: "play.fill")
                            }

                            if autoLaunchImageID == image.id.uuidString {
                                Button {
                                    autoLaunchImageID = nil
                                } label: {
                                    Label("Clear Auto-Launch", systemImage: "star.slash")
                                }
                            } else {
                                Button {
                                    autoLaunchImageID = image.id.uuidString
                                } label: {
                                    Label("Set as Auto-Launch", systemImage: "star.fill")
                                }
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

                            Button {
                                showInFiles(image)
                            } label: {
                                Label("Show in Files", systemImage: "folder")
                            }

                            #if targetEnvironment(macCatalyst)
                            Button {
                                imageToExport = image
                            } label: {
                                Label("Export as App...", systemImage: "shippingbox")
                            }
                            #endif

                            Divider()

                            Button(role: .destructive) {
                                imageToDelete = image
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                        }

                    Divider()
                        .padding(.leading, 12)
                }
            }
        }
        .frame(maxHeight: .infinity)
    }

    // MARK: - Detail Panel

    private var detailPanel: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let image = selectedImage {
                // Info bar + action buttons (buttons first so they're always visible)
                Text("\(image.name), \(image.imageFileName)")
                    .font(.caption)
                    .foregroundColor(.white)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Color.gray)

                // Action buttons — above details so they're never pushed off screen
                HStack(spacing: 16) {
                    Button {
                        launchImage(image)
                    } label: {
                        Label("Launch", systemImage: "play.fill")
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)

                    Button {
                        renameText = image.name
                        imageToRename = image
                    } label: {
                        Label("Rename", systemImage: "pencil")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)

                    Button {
                        imageToShare = image
                    } label: {
                        Label("Share", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)

                    Button {
                        showInFiles(image)
                    } label: {
                        Label("Show in Files", systemImage: "folder")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)

                    if autoLaunchImageID == image.id.uuidString {
                        Button {
                            autoLaunchImageID = nil
                        } label: {
                            Label("Auto-Launch", systemImage: "star.fill")
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                        .tint(.orange)
                    } else {
                        Button {
                            autoLaunchImageID = image.id.uuidString
                        } label: {
                            Label("Auto-Launch", systemImage: "star")
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    }

                    Spacer()

                    Button(role: .destructive) {
                        imageToDelete = image
                    } label: {
                        Label("Delete", systemImage: "trash")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
                .padding(.horizontal, 12)
                .padding(.bottom, 4)

                // Details grid (scrollable on small screens)
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 4) {
                        detailRow("Image file", value: image.imageFileName)
                        detailRow("Location", value: image.directoryURL.path)
                        if image.pharoVersion != nil {
                            detailRow("Pharo version", value: image.versionLabel)
                        }
                        if let totalSize = imageManager.totalSizeForImage(image) {
                            let formatter = ByteCountFormatter()
                            detailRow("Total size", value: formatter.string(fromByteCount: totalSize))
                        }
                        if let modified = image.fileModificationDate {
                            detailRow("Last modified", value: formatDate(modified))
                        }
                        detailRow("Created", value: formatDate(image.createdAt))
                        if let launched = image.lastLaunchedAt {
                            detailRow("Last launched", value: formatDate(launched))
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 4)
                }
                .frame(maxHeight: 120)
            } else {
                Text("Select an image to see details, or right-click for options")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            }
        }
        .background(Color(.secondarySystemBackground))
    }

    private func detailRow(_ label: String, value: String) -> some View {
        HStack(alignment: .top) {
            Text(label)
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(width: 100, alignment: .leading)
            Text(value)
                .font(.caption)
        }
    }

    private func formatDate(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    // MARK: - Disclaimer

    private var disclaimerBlock: some View {
        VStack(spacing: 2) {
            Text(disclaimerAttributed)
                .font(.caption)
                .multilineTextAlignment(.center)
            HStack(spacing: 12) {
                Link("GitHub", destination: URL(string: "https://github.com/avwohl/iospharo")!)
                Text("·")
                    .foregroundColor(.secondary)
                Text("v\(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?") (\(Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"))")
                    .foregroundColor(.secondary)
                Link("Changes", destination: URL(string: "https://github.com/avwohl/iospharo/blob/main/docs/changes.md")!)
                Text("·")
                    .foregroundColor(.secondary)
                Link("Report a Bug", destination: URL(string: "https://github.com/avwohl/iospharo/issues")!)
            }
            .font(.caption)
        }
        .frame(maxWidth: .infinity)
    }

    private var disclaimerAttributed: AttributedString {
        var prefix = AttributedString("Experimental release — not endorsed by ")
        prefix.foregroundColor = .secondary
        var link = AttributedString("Pharo.org")
        link.link = URL(string: "https://pharo.org")
        return prefix + link
    }

    // MARK: - Actions

    private func launchImage(_ image: PharoImage) {
        imageManager.markLaunched(image)
        imageManager.selectedImageID = image.id
        if bridge.loadImage(at: image.imagePath) {
            bridge.start()
        }
    }

    private func showInFiles(_ image: PharoImage) {
        let url = image.directoryURL
        #if targetEnvironment(macCatalyst)
        // On Mac Catalyst, opening a file:// URL opens Finder
        UIApplication.shared.open(url)
        #else
        // On iPad, present a document picker rooted at the image directory
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let rootVC = scene.windows.first?.rootViewController else { return }
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item, .folder])
        picker.directoryURL = url
        picker.allowsMultipleSelection = false
        rootVC.present(picker, animated: true)
        #endif
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
