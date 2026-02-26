/*
 * SettingsView.swift
 *
 * Settings sheet with diagnostics and about information.
 * Download buttons are now in NewImageView/ImageLibraryView.
 */

import SwiftUI

struct SettingsView: View {

    @Environment(\.dismiss) var dismiss
    @State private var showingDiagnostics = false

    var body: some View {
        NavigationView {
            List {
                Section("Developer Tools") {
                    Button("VM Diagnostics") {
                        showingDiagnostics = true
                    }
                }

                Section("About") {
                    HStack {
                        Text("VM Version")
                        Spacer()
                        Text(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "Unknown")
                            .foregroundColor(.gray)
                    }

                    Text("This is an experimental, community-built Pharo VM for iOS. It is not affiliated with, endorsed by, or supported by Pharo.org.")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    Link(destination: URL(string: "https://github.com/avwohl/iospharo")!) {
                        HStack {
                            Label("GitHub Project", systemImage: "link")
                            Spacer()
                            Image(systemName: "arrow.up.right.square")
                                .foregroundColor(.secondary)
                        }
                    }

                    Link(destination: URL(string: "https://github.com/avwohl/iospharo/issues")!) {
                        HStack {
                            Label("Report a Bug", systemImage: "ladybug")
                            Spacer()
                            Image(systemName: "arrow.up.right.square")
                                .foregroundColor(.secondary)
                        }
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showingDiagnostics) {
                DiagnosticsView()
            }
        }
    }
}
