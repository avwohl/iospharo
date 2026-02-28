/*
 * AutoLaunchSplashView.swift
 *
 * 3-second countdown splash shown before auto-launching a Pharo image.
 * Gives the user an escape hatch to cancel and return to the image library
 * (e.g. if the image is damaged or they want to pick a different one).
 */

import SwiftUI

struct AutoLaunchSplashView: View {
    let imageName: String
    let onLaunch: () -> Void
    let onCancel: () -> Void

    @State private var countdown: Int = 3

    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 24) {
            Spacer()

            Image(systemName: "cube.box.fill")
                .font(.system(size: 56))
                .foregroundColor(.accentColor)

            Text("Auto-launching")
                .font(.title2)
                .foregroundColor(.primary)

            Text(imageName)
                .font(.headline)
                .foregroundColor(.secondary)

            Text("\(countdown)")
                .font(.system(size: 48, weight: .bold, design: .rounded))
                .foregroundColor(.accentColor)

            Button {
                onCancel()
            } label: {
                Label("Show Library", systemImage: "list.bullet")
                    .font(.headline)
                    .frame(maxWidth: 280)
            }
            .buttonStyle(.bordered)
            .controlSize(.large)

            Spacer()
        }
        .padding()
        .onReceive(timer) { _ in
            if countdown > 1 {
                countdown -= 1
            } else {
                onLaunch()
            }
        }
    }
}
