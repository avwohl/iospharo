/*
 * ExportAppSheet.swift
 *
 * SwiftUI sheet for configuring and exporting a standalone iOS/macOS app
 * from a Pharo image. Generates an Xcode project and opens it in Xcode.
 */

import SwiftUI

struct ExportAppSheet: View {
    let image: PharoImage

    @Environment(\.dismiss) var dismiss
    @State private var appName: String = ""
    @State private var bundleID: String = ""
    @State private var teamID: String = ""
    @State private var exportMacOS = true
    @State private var exportIOS = true
    @State private var exportWatchOS = false
    @State private var kioskMode = true
    @State private var stripImage = true
    @State private var appIconURL: URL?
    @State private var appIconImage: UIImage?
    @State private var showingIconPicker = false
    @State private var outputURL: URL?
    @State private var showingFolderPicker = false

    @State private var isExporting = false
    @State private var exportPhase: String = ""
    @State private var exportError: String?
    @State private var exportSuccess = false
    @State private var generatedProjectURL: URL?

    private var sanitizedAppName: String {
        AppExporter.sanitizeName(appName.isEmpty ? image.name : appName)
    }

    private var defaultBundleID: String {
        let name = sanitizedAppName.lowercased()
        return "com.example.\(name)"
    }

    var body: some View {
        NavigationView {
            Form {
                Section("App Configuration") {
                    HStack {
                        Text("App Name")
                            .frame(width: 120, alignment: .leading)
                        TextField("MyPharoApp", text: $appName)
                            .textFieldStyle(.roundedBorder)
                    }

                    HStack {
                        Text("Bundle ID")
                            .frame(width: 120, alignment: .leading)
                        TextField(defaultBundleID, text: $bundleID)
                            .textFieldStyle(.roundedBorder)
                            .autocapitalization(.none)
                            .disableAutocorrection(true)
                    }

                    HStack {
                        Text("Team ID")
                            .frame(width: 120, alignment: .leading)
                        TextField("Optional — for code signing", text: $teamID)
                            .textFieldStyle(.roundedBorder)
                            .autocapitalization(.none)
                            .disableAutocorrection(true)
                    }
                }

                Section("Platform") {
                    Toggle("macOS (Mac Catalyst)", isOn: $exportMacOS)
                    Toggle("iOS", isOn: $exportIOS)
                    Toggle("watchOS (Companion)", isOn: $exportWatchOS)
                    if exportWatchOS {
                        Text("Adds a Watch companion app. The Watch app is installed automatically when the iOS app is installed. Pharo does not run on the Watch — this is a placeholder companion.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                Section("Image Options") {
                    Toggle("Kiosk Mode", isOn: $kioskMode)
                    if kioskMode {
                        Text("Hides taskbar, menu bar, and World menu. Your app fills the screen.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        Text("Keeps full Pharo development environment (taskbar, menu bar, World menu).")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }

                    Toggle("Strip Development Tools", isOn: $stripImage)
                    if stripImage {
                        Text("Removes IDE, Iceberg, tests, debugger, and code browser on first launch. Reduces image size by ~30-40 MB.")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    } else {
                        Text("Bundles the full Pharo image as-is (~56 MB).")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                Section("App Icon") {
                    HStack {
                        if let iconImage = appIconImage {
                            Image(uiImage: iconImage)
                                .resizable()
                                .aspectRatio(contentMode: .fit)
                                .frame(width: 60, height: 60)
                                .cornerRadius(12)
                        } else {
                            RoundedRectangle(cornerRadius: 12)
                                .fill(Color.gray.opacity(0.2))
                                .frame(width: 60, height: 60)
                                .overlay(
                                    Image(systemName: "app.dashed")
                                        .font(.title2)
                                        .foregroundColor(.secondary)
                                )
                        }
                        VStack(alignment: .leading, spacing: 4) {
                            if appIconURL != nil {
                                Text("Custom icon selected")
                                Button("Remove") {
                                    appIconURL = nil
                                    appIconImage = nil
                                }
                                .font(.caption)
                            } else {
                                Text("No icon (placeholder)")
                                    .foregroundColor(.secondary)
                            }
                        }
                        Spacer()
                        Button("Choose...") {
                            showingIconPicker = true
                        }
                    }
                    Text("Recommended: 1024x1024 PNG. Xcode will generate all required sizes.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                Section("Output") {
                    HStack {
                        if let url = outputURL {
                            Text(url.path)
                                .font(.caption)
                                .lineLimit(1)
                                .truncationMode(.head)
                        } else {
                            Text("Desktop (default)")
                                .foregroundColor(.secondary)
                        }
                        Spacer()
                        Button("Choose...") {
                            showingFolderPicker = true
                        }
                    }
                }

                Section("Source Image") {
                    HStack {
                        Text("Image")
                            .foregroundColor(.secondary)
                        Spacer()
                        Text(image.name)
                    }
                    HStack {
                        Text("File")
                            .foregroundColor(.secondary)
                        Spacer()
                        Text(image.imageFileName)
                            .font(.caption)
                    }
                    if let size = image.formattedSize {
                        HStack {
                            Text("Size")
                                .foregroundColor(.secondary)
                            Spacer()
                            Text(size)
                        }
                    }
                }

                if isExporting {
                    Section("Progress") {
                        HStack {
                            ProgressView()
                                .padding(.trailing, 8)
                            Text(exportPhase)
                                .foregroundColor(.secondary)
                        }
                    }
                }

                if let error = exportError {
                    Section("Error") {
                        Text(error)
                            .foregroundColor(.red)
                            .font(.caption)
                    }
                }

                if exportSuccess, let projectURL = generatedProjectURL {
                    Section("Done") {
                        VStack(alignment: .leading, spacing: 8) {
                            Label("Project generated successfully", systemImage: "checkmark.circle.fill")
                                .foregroundColor(.green)

                            Text(projectURL.path)
                                .font(.caption)
                                .foregroundColor(.secondary)

                            HStack(spacing: 16) {
                                Button("Open in Xcode") {
                                    openInXcode(projectURL)
                                }
                                .buttonStyle(.borderedProminent)

                                Button("Show in Finder") {
                                    showInFinder(projectURL)
                                }
                                .buttonStyle(.bordered)
                            }
                        }
                    }
                }
            }
            .navigationTitle("Export as App")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }

                ToolbarItem(placement: .confirmationAction) {
                    Button("Export") { startExport() }
                        .disabled(isExporting || exportSuccess || (!exportMacOS && !exportIOS))
                }
            }
            .sheet(isPresented: $showingFolderPicker) {
                FolderPicker { url in
                    outputURL = url
                }
            }
            .sheet(isPresented: $showingIconPicker) {
                ImagePicker { url in
                    appIconURL = url
                    if let url = url, let data = try? Data(contentsOf: url) {
                        appIconImage = UIImage(data: data)
                    }
                }
            }
        }
    }

    // MARK: - Export

    private func startExport() {
        isExporting = true
        exportError = nil
        exportSuccess = false

        let config = ExportConfig(
            appName: sanitizedAppName,
            bundleID: bundleID.isEmpty ? defaultBundleID : bundleID,
            teamID: teamID.isEmpty ? nil : teamID,
            exportMacOS: exportMacOS,
            exportIOS: exportIOS,
            exportWatchOS: exportWatchOS,
            kioskMode: kioskMode,
            stripImage: stripImage,
            appIconURL: appIconURL,
            outputDirectory: outputURL ?? desktopURL,
            sourceImage: image
        )

        Task {
            do {
                let exporter = AppExporter()
                let projectURL = try await exporter.export(config: config) { phase in
                    Task { @MainActor in
                        exportPhase = phase
                    }
                }
                await MainActor.run {
                    generatedProjectURL = projectURL
                    exportSuccess = true
                    isExporting = false
                }
            } catch {
                await MainActor.run {
                    exportError = error.localizedDescription
                    isExporting = false
                }
            }
        }
    }

    private var desktopURL: URL {
        FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
    }

    private func openInXcode(_ projectURL: URL) {
        #if targetEnvironment(macCatalyst)
        // Find the .xcodeproj inside the project directory
        if let xcodeproj = try? FileManager.default.contentsOfDirectory(at: projectURL, includingPropertiesForKeys: nil)
            .first(where: { $0.pathExtension == "xcodeproj" }) {
            UIApplication.shared.open(xcodeproj)
        }
        #endif
    }

    private func showInFinder(_ url: URL) {
        #if targetEnvironment(macCatalyst)
        UIApplication.shared.open(url)
        #endif
    }
}

// MARK: - Folder Picker

struct FolderPicker: UIViewControllerRepresentable {
    let onPick: (URL) -> Void

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder])
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(onPick: onPick) }

    class Coordinator: NSObject, UIDocumentPickerDelegate {
        let onPick: (URL) -> Void
        init(onPick: @escaping (URL) -> Void) { self.onPick = onPick }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            if let url = urls.first {
                onPick(url)
            }
        }
    }
}

// MARK: - Image Picker

import UniformTypeIdentifiers

struct ImagePicker: UIViewControllerRepresentable {
    let onPick: (URL?) -> Void

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.png, .jpeg])
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(onPick: onPick) }

    class Coordinator: NSObject, UIDocumentPickerDelegate {
        let onPick: (URL?) -> Void
        init(onPick: @escaping (URL?) -> Void) { self.onPick = onPick }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            guard let sourceURL = urls.first else {
                onPick(nil)
                return
            }
            // Copy to a temporary location so the security-scoped resource isn't needed later
            let tempDir = FileManager.default.temporaryDirectory
            let destURL = tempDir.appendingPathComponent("AppIcon_\(UUID().uuidString).png")
            do {
                _ = sourceURL.startAccessingSecurityScopedResource()
                defer { sourceURL.stopAccessingSecurityScopedResource() }
                if FileManager.default.fileExists(atPath: destURL.path) {
                    try FileManager.default.removeItem(at: destURL)
                }
                try FileManager.default.copyItem(at: sourceURL, to: destURL)
                onPick(destURL)
            } catch {
                onPick(nil)
            }
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            // Don't clear existing selection on cancel
        }
    }
}
