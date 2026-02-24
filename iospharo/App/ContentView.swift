/*
 * ContentView.swift
 *
 * Main content view — routes between the image library and the Pharo canvas.
 */

import SwiftUI

// MARK: - Content View

struct ContentView: View {

    @EnvironmentObject var bridge: PharoBridge
    @EnvironmentObject var imageManager: ImageManager

    var body: some View {
        ZStack {
            if bridge.isRunning {
                // Pharo is running — full-screen canvas
                pharoCanvas
            } else {
                // Image library (handles empty state, downloads, list)
                ImageLibraryView()
            }

            // Error overlay for VM errors
            if let error = bridge.errorMessage {
                errorOverlay(message: error)
            }
        }
        .onAppear {
            imageManager.load()
        }
    }

    // MARK: - Views

    private var pharoCanvas: some View {
        ZStack {
            PharoCanvasView(bridge: bridge)
                .ignoresSafeArea()

            #if !targetEnvironment(macCatalyst)
            MiddleClickButton(isActive: $bridge.middleClickActive)
            #endif
        }
    }

    private func errorOverlay(message: String) -> some View {
        VStack {
            Spacer()

            HStack {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundColor(.yellow)
                Text(message)
                    .foregroundColor(.white)
            }
            .padding()
            .background(Color.red.opacity(0.8))
            .cornerRadius(10)
            .padding()
        }
    }
}

// MARK: - Diagnostics View

struct DiagnosticsView: View {
    @Environment(\.dismiss) var dismiss
    @State private var results: [(String, Bool, String)] = []

    var body: some View {
        NavigationView {
            List {
                Section("VM Core Tests") {
                    testRow("Memory Allocation", testMemory)
                    testRow("Integer Tagging", testIntegerTag)
                    testRow("ASLR Info", testASLR)
                }

                #if PHARO_IOS_OOP_WRAPPER
                Section("C++ Oop Tests") {
                    testRow("Space Encoding", testSpaceEncoding)
                    testRow("Pointer Roundtrip", testPointerRoundtrip)
                }
                #else
                Section("Build Mode") {
                    Text("Standard C mode (not C++ Oop)")
                        .foregroundColor(.secondary)
                }
                #endif

                if !results.isEmpty {
                    Section("Results") {
                        ForEach(results, id: \.0) { name, passed, detail in
                            HStack {
                                Image(systemName: passed ? "checkmark.circle.fill" : "xmark.circle.fill")
                                    .foregroundColor(passed ? .green : .red)
                                VStack(alignment: .leading) {
                                    Text(name)
                                    Text(detail)
                                        .font(.caption)
                                        .foregroundColor(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle("VM Diagnostics")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func testRow(_ name: String, _ test: @escaping () -> (Bool, String)) -> some View {
        Button(name) {
            let (passed, detail) = test()
            results.append((name, passed, detail))
        }
    }

    private func testMemory() -> (Bool, String) {
        let size = 1024 * 1024
        guard let ptr = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
              ptr != MAP_FAILED else {
            return (false, "mmap failed")
        }
        ptr.storeBytes(of: UInt64(0xDEADBEEF), as: UInt64.self)
        let val = ptr.load(as: UInt64.self)
        munmap(ptr, size)
        return (val == 0xDEADBEEF, "mmap/munmap working")
    }

    private func testIntegerTag() -> (Bool, String) {
        let value: Int64 = 42
        let encoded = (value << 3) | 1
        let decoded = encoded >> 3
        return (decoded == value, "42 -> 0x\(String(encoded, radix: 16)) -> \(decoded)")
    }

    private func testASLR() -> (Bool, String) {
        var stackVar: Int = 0
        let stackAddr = withUnsafePointer(to: &stackVar) { UInt(bitPattern: $0) }
        return (true, "Stack @ 0x\(String(stackAddr, radix: 16).prefix(6))...")
    }

    private func testSpaceEncoding() -> (Bool, String) {
        let addr: UInt64 = 0x100000008
        let encoded = addr | (1 << 1)
        let space = (encoded >> 1) & 0x3
        return (space == 1, "Old space encoding correct")
    }

    private func testPointerRoundtrip() -> (Bool, String) {
        let size = 1024
        guard let ptr = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
              ptr != MAP_FAILED else {
            return (false, "Alloc failed")
        }
        defer { munmap(ptr, size) }

        let addr = UInt64(UInt(bitPattern: ptr))
        let encoded = addr | (1 << 1)
        let decoded = UnsafeMutableRawPointer(bitPattern: UInt(encoded & ~UInt64(0x7)))

        ptr.storeBytes(of: UInt64(0xCAFEBABE), as: UInt64.self)
        let readBack = decoded?.load(as: UInt64.self) ?? 0
        return (readBack == 0xCAFEBABE, readBack == 0xCAFEBABE ? "Roundtrip OK" : "Corrupted")
    }
}

// MARK: - Floating Middle-Click Button (iOS only)

#if !targetEnvironment(macCatalyst)
struct MiddleClickButton: View {
    @Binding var isActive: Bool
    @State private var offset: CGSize = .zero
    @State private var dragOffset: CGSize = .zero

    var body: some View {
        VStack {
            Spacer()
            HStack {
                Spacer()
                Button(action: {
                    isActive.toggle()
                }) {
                    Image(systemName: "computermouse.fill")
                        .font(.system(size: 18))
                        .foregroundColor(isActive ? .white : .white.opacity(0.7))
                        .frame(width: 44, height: 44)
                        .background(isActive ? Color.blue : Color.black.opacity(0.4))
                        .clipShape(Circle())
                        .overlay(
                            Circle()
                                .stroke(isActive ? Color.white : Color.clear, lineWidth: 2)
                        )
                }
                .offset(x: offset.width + dragOffset.width,
                        y: offset.height + dragOffset.height)
                .gesture(
                    DragGesture()
                        .onChanged { value in
                            dragOffset = value.translation
                        }
                        .onEnded { value in
                            offset.width += value.translation.width
                            offset.height += value.translation.height
                            dragOffset = .zero
                        }
                )
                .padding(.trailing, 16)
                .padding(.bottom, 80)
            }
        }
        .allowsHitTesting(true)
    }
}
#endif

// MARK: - Preview

#Preview {
    ContentView()
        .environmentObject(PharoBridge.shared)
        .environmentObject(ImageManager())
}
