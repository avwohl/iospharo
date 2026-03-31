/*
 * AcknowledgementsView.swift
 *
 * Displays credits and open source license information for all
 * third-party code used in iospharo.
 */

import SwiftUI

struct AcknowledgementsView: View {

    var body: some View {
        List {
            Section {
                Text("iospharo is built on the work of the Pharo and Squeak communities and uses the following open source software.")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
                    .padding(.vertical, 4)
            }

            acknowledgementSection(
                name: "Pharo",
                license: "MIT",
                copyright: "2008\u{2013}2019 The Pharo Project, Inria\n1996\u{2013}2008 Viewpoints Research Institute\n1996 Apple Inc.",
                url: "https://pharo.org",
                description: "The Smalltalk environment this VM executes. The VM's interpreter, object memory, and primitives are a clean C++ reimplementation based on the Pharo/Squeak architecture and specification."
            )

            acknowledgementSection(
                name: "OpenSmalltalk-VM / Pharo-VM",
                license: "MIT",
                copyright: "2013 3D Immersive Collaboration Consulting, LLC",
                url: "https://github.com/OpenSmalltalk/opensmalltalk-vm",
                description: "VMMaker-generated plugin code (Balloon Engine, DSA, JPEG, SSL) and VM headers are taken from this project."
            )

            acknowledgementSection(
                name: "Independent JPEG Group (libjpeg 6b)",
                license: "IJG License",
                copyright: "1991\u{2013}1998 Thomas G. Lane",
                url: "http://www.ijg.org",
                description: "JPEG image codec. This software is based in part on the work of the Independent JPEG Group."
            )

            acknowledgementSection(
                name: "libffi",
                license: "MIT",
                copyright: "1996\u{2013}2024 Anthony Green, Red Hat, Inc and others",
                url: "https://github.com/libffi/libffi",
                description: "Foreign Function Interface library used for FFI callouts."
            )

            acknowledgementSection(
                name: "SDL2",
                license: "zlib",
                copyright: "1997\u{2013}2023 Sam Lantinga",
                url: "https://www.libsdl.org",
                description: "Simple DirectMedia Layer. The Pharo image's display driver calls SDL2 via FFI."
            )

            acknowledgementSection(
                name: "cairo",
                license: "LGPL-2.1 / MPL-1.1",
                copyright: "Various contributors",
                url: "https://cairographics.org",
                description: "2D graphics library used by Pharo for vector rendering."
            )

            acknowledgementSection(
                name: "FreeType",
                license: "FreeType License (FTL)",
                copyright: "1996\u{2013}2024 David Turner, Robert Wilhelm, Werner Lemberg",
                url: "https://freetype.org",
                description: "Font rasterization library."
            )

            acknowledgementSection(
                name: "HarfBuzz",
                license: "MIT",
                copyright: "Various contributors",
                url: "https://github.com/harfbuzz/harfbuzz",
                description: "Text shaping engine."
            )

            acknowledgementSection(
                name: "pixman",
                license: "MIT",
                copyright: "Various contributors",
                url: "https://cairographics.org",
                description: "Low-level pixel manipulation library."
            )

            acknowledgementSection(
                name: "libpng",
                license: "libpng license",
                copyright: "1995\u{2013}2024 The PNG Reference Library Authors",
                url: "http://libpng.org",
                description: "PNG image format support."
            )

            acknowledgementSection(
                name: "OpenSSL",
                license: "Apache-2.0",
                copyright: "1998\u{2013}2024 The OpenSSL Project Authors",
                url: "https://www.openssl.org",
                description: "Cryptography and TLS library."
            )

            acknowledgementSection(
                name: "libssh2",
                license: "BSD-3-Clause",
                copyright: "2004\u{2013}2024 Sara Golemon, Daniel Stenberg, and others",
                url: "https://www.libssh2.org",
                description: "SSH client library."
            )

            acknowledgementSection(
                name: "libgit2",
                license: "GPL-2.0 with linking exception",
                copyright: "libgit2 contributors",
                url: "https://libgit2.org",
                description: "Git library for repository operations."
            )
        }
        .navigationTitle("Acknowledgements")
        .navigationBarTitleDisplayMode(.inline)
    }

    @ViewBuilder
    private func acknowledgementSection(
        name: String,
        license: String,
        copyright: String,
        url: String,
        description: String
    ) -> some View {
        Section(name) {
            Text(description)
                .font(.subheadline)

            HStack {
                Text("License")
                    .foregroundColor(.secondary)
                Spacer()
                Text(license)
            }
            .font(.subheadline)

            HStack(alignment: .top) {
                Text("Copyright")
                    .foregroundColor(.secondary)
                Spacer()
                Text(copyright)
                    .multilineTextAlignment(.trailing)
            }
            .font(.caption)

            if let link = URL(string: url) {
                Link(destination: link) {
                    HStack {
                        Text(url)
                            .font(.caption)
                        Spacer()
                        Image(systemName: "arrow.up.right.square")
                            .foregroundColor(.secondary)
                    }
                }
            }
        }
    }
}
