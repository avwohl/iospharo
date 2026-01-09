import SwiftUI

struct ContentView: View {
    @EnvironmentObject var vmController: VMController
    @State private var imagePath: String = "/Users/wohl/src/iospharo/test-images/Pharo-UI-Ready.image"
    @State private var autoStarted: Bool = false

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
        .onAppear {
            // Write debug info to file
            let debugLog = "/tmp/pharomac_debug.log"
            FileManager.default.createFile(atPath: debugLog, contents: nil)
            let log = FileHandle(forWritingAtPath: debugLog)
            log?.write("[DEBUG] onAppear called\n".data(using: .utf8)!)

            // Auto-start with test image for debugging
            if !autoStarted && !imagePath.isEmpty {
                autoStarted = true
                log?.write("[DEBUG] Auto-starting VM...\n".data(using: .utf8)!)
                vmController.initialize()
                log?.write("[DEBUG] VM initialized, loading image: \(imagePath)\n".data(using: .utf8)!)
                vmController.loadImage(path: imagePath)
                log?.write("[DEBUG] Image loaded, starting VM...\n".data(using: .utf8)!)
                vmController.start()
                log?.write("[DEBUG] VM started: \(vmController.isRunning)\n".data(using: .utf8)!)
            }
            log?.closeFile()
        }
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
