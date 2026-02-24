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
                        Text("1.0.0")
                            .foregroundColor(.gray)
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
