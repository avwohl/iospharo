import SwiftUI

struct ContentView: View {
    @EnvironmentObject var vmController: VMController
    @State private var imagePath: String = ""

    var body: some View {
        VStack {
            if vmController.isRunning {
                PharoCanvasView()
                    .environmentObject(vmController)
            } else {
                VStack(spacing: 20) {
                    Text("Pharo VM")
                        .font(.largeTitle)

                    Text(vmController.statusMessage)
                        .foregroundColor(.secondary)

                    HStack {
                        TextField("Image path", text: $imagePath)
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 400)

                        Button("Browse...") {
                            browseForImage()
                        }
                    }

                    HStack(spacing: 20) {
                        Button("Initialize") {
                            vmController.initialize()
                        }
                        .disabled(vmController.isLoaded)

                        Button("Load Image") {
                            vmController.loadImage(path: imagePath)
                        }
                        .disabled(imagePath.isEmpty)

                        Button("Start") {
                            vmController.start()
                        }
                        .disabled(!vmController.isLoaded)
                    }
                }
                .padding(40)
            }
        }
        .frame(minWidth: 800, minHeight: 600)
    }

    private func browseForImage() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "image")!]
        panel.allowsMultipleSelection = false

        if panel.runModal() == .OK, let url = panel.url {
            imagePath = url.path
        }
    }
}
