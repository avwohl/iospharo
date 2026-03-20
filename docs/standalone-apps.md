# Standalone App Building: Survey & Status

2026-03-20

A survey of how interpreted-language systems (Prograph, various Smalltalks,
and this project) build standalone distributable applications -- including
multi-platform support and Apple App Store acceptance.

---

## 1. Prograph CPX (1995)

Prograph was a visual dataflow language for Mac and Windows. Its approach
to standalone apps is the most relevant precedent for this project because
it shipped compiled apps from an interpreted visual language on Apple platforms.

**Architecture: Hybrid Interpreter + Compiler**

- Development: programs run through the Prograph interpreter in the IDE
- Deployment: a code compiler produces a standalone executable
- The compiled app does NOT include the interpreter, debugger, or IDE
- Only the runtime execution engine + needed framework classes are included

**Tree Shaking / Dead Code Elimination**

Prograph had aggressive class-level tree shaking in 1995:

    "With the ABCs, you use only the code you need -- if an ABC class
    is not needed in the final version of your program, it is removed
    by the Prograph Compiler to make the compiled program smaller."

The Application Builder Classes (ABCs) framework had 147 classes derived
from 60 base classes. At compile time, the compiler analyzed which classes
were actually referenced and stripped the rest. This anticipated modern
JavaScript bundler tree-shaking by ~20 years.

**C Integration**

The Prograph C Tools Kit allowed linking hand-written C code for
performance-critical sections. External methods could call C/Pascal
code or system routines (Mac Toolbox, Windows API) at compile time.

**App Store Relevance**

Prograph predated app stores (1995), but its approach -- compiling to
native standalone executables with no interpreter dependency -- would
satisfy modern App Store requirements. The resulting apps were standard
Mac/Windows binaries.


## 2. Smalltalk Implementations

### Spectrum of Approaches (least to most native)

    Pharo/Squeak/Cuis   VisualWorks   Dolphin     Smalltalk/X   Lowtalk
    VM + full image      VM + stripped  VM in .exe  STC to C      No VM,
                         image          + method    to native,    native
                                        stripping   runtime lib   object
                                                    still needed  files

    Amber/PharoJS                      TruffleSqueak
    Transpile to JS,                   Native Image of interpreter
    no VM at all                       via GraalVM (no JVM needed)


### 2.1 Pharo (our base)

- Ships VM binary + .image + .changes + .sources
- PharoApplicationGenerator wraps these in platform bundles (.app, .dmg)
- Minimal Image: stripped kernel-only image for server deployments (~12-14 MB in Docker)
- `Smalltalk cleanUp: true` removes caches, test packages, dead instances
- No automated tree-shaking or reachability analysis
- VM (~15-30 MB) is always required, cannot be eliminated

PharoJS transpiles Pharo to JavaScript for browser/Cordova deployment,
eliminating the VM entirely -- but only for web-style UI apps.


### 2.2 Squeak

- Same model as Pharo (its ancestor): VM + image
- `Smalltalk majorShrink` for heuristic image reduction (not true reachability)
- Plopp 3D painting software: notable commercial Squeak app, shipped as VM+image bundle
- Scratch (MIT) was originally built on Squeak
- Cog JIT would need to be disabled for iOS (interpreter-only mode needed)


### 2.3 VisualWorks (Cincom)

- Most mature commercial deployment tooling
- Runtime Packager: scans for unreferenced parcels, provides stripping UI
- Deploy from clean "base runtime image" (no IDE) + only needed parcels
- StoreCI: automated build from Store repository to deployable parcel set
- VM always required (proprietary, commercially licensed)
- JP Morgan's Kapital: derivatives risk management, all VisualWorks + GemStone,
  running in production across NY/London/Glasgow/Mumbai/Hong Kong/Tokyo


### 2.4 Dolphin Smalltalk

- Best dead-code elimination of any mainstream Smalltalk
- Lagoon Deployment Wizard (Application Deployment Wizard):
  - Removes unreferenced classes
  - Removes methods whose selectors are never sent
  - Folds duplicate string literals
  - Produces a single .exe (VM DLL embedded)
- Conservative: keeps any method sharing a selector with a reachable send
  (can't prove unreachability due to dynamic dispatch)
- Windows only, now open-source on GitHub


### 2.5 Smalltalk/X (eXept)

- True native code compilation -- unique in the Smalltalk world
- STC (Smalltalk-to-C Compiler):
  - Compiles .st files to C, then C to native object files
  - Can generate shared libraries or standalone executables
  - Compiled code contains no bytecode (not trivially decompilable)
  - `-C` flag shows intermediate C output
- Hybrid execution: compiled native code + interpreted bytecode coexist
- Runtime library still needed for message dispatch, GC, object model
- But bytecode interpreter can be eliminated for fully-compiled code
- Used commercially: expecco test automation built on ST/X
- Theoretically the most App Store-friendly (native code, no interpretation)
  but no iOS toolchain exists


### 2.6 Amber Smalltalk / PharoJS

- Transpile Smalltalk to JavaScript -- no VM at all
- Amber: self-hosting compiler, outputs static JS/HTML/CSS
- PharoJS: compiles Pharo code to JS, mobile deploy via Cordova
- Web apps only; full Morphic/Spec UI not supported
- App Store: wraps as web app via Cordova/Capacitor, JS runs in platform engine


### 2.7 GNU Smalltalk

- Command-line scripting oriented, Unix philosophy
- Embeddable as C library (libgst) with custom main()
- No native compilation, no dead code elimination
- Primarily for scripting/education


### 2.8 GemStone/S

- Server-side object database, not a standalone app platform
- Multi-user transactional Smalltalk with persistent objects
- Client dev via Pharo/Squeak/VisualWorks + GLASS
- OOCL's IRIS-2: 1.5 billion objects across 150 offices
- Free Community Edition available


### 2.9 Cuis Smalltalk

- Squeak fork, same VM+image model
- Philosophy: keep core small from the start (vs strip a large image)
- Base image significantly smaller than Pharo/Squeak
- Used in education (University of Buenos Aires)


## 3. Research / Academic Projects

### Strongtalk (Sun, 1994-1997)
- Advanced type-feedback JIT, ran Smalltalk faster than any other implementation
- Optional static type system (first for Smalltalk)
- Team went on to create HotSpot JVM, V8, and Dart
- Open-sourced 2006, not maintained

### TruffleSqueak (GraalVM)
- Squeak on GraalVM Truffle framework
- GraalVM native-image AOT-compiles the interpreter to a standalone binary
- Result: native Smalltalk interpreter (no JVM), but image still interpreted
- Supports Apple Silicon

### Zag Smalltalk + LLVM (IWST 2024)
- From-scratch VM in Zig with LLVM JIT backend
- Methods stored as ASTs, converted to threaded code or JIT on first execution
- Continuation-passing style execution with tail calls

### LLST (LLVM Little Smalltalk)
- C++ rewrite of Little Smalltalk with LLVM JIT
- Claims up to 50x speedup over bytecode VM
- Binary compatible with original Little Smalltalk images

### Lowtalk (Ronsaldo)
- Smalltalk dialect designed to work without a VM
- Compiler in Pharo, generates SSA IR via Slovim (Pharo LLVM-like IR)
- Outputs standard relocatable object files for platform linker
- Supports native C types + dynamic object types
- Experimental, intended for game development

### Slang / OpenSmalltalk VM
- The VM itself is written in Smalltalk (subset called Slang)
- Transpiled to C by VMMaker, compiled to native code
- Proves Smalltalk-to-C works at scale (~100K lines generated C)
- But Slang is restricted: no closures, limited polymorphism, explicit typing


## 4. What iospharo Has Today

The project already has an "Export as App" feature (added build 73, 2026-03-05).

**Current capability:**

- Right-click image in library -> "Export as App..." (Mac Catalyst only)
- Generates a complete standalone Xcode project
- User configures: app name, bundle ID, team ID, platforms (iOS/Mac/watchOS), kiosk mode
- Generated project includes: Swift sources, Metal shaders, xcframeworks, embedded image
- Custom app icon picker (PNG/JPEG, Xcode auto-scales to all sizes)
- Strip Development Tools option removes IDE packages on first launch (~30-40 MB savings)
- watchOS companion target: iOS build embeds a watch app automatically

**What gets bundled:**

    Sources/         6 Swift files + Metal shader (generated from templates)
    Headers/         VMParameters.h, MotionData.h (stubs)
    Frameworks/      PharoVMCore.xcframework (VM + cairo/freetype/harfbuzz/etc.)
                     SDL2.xcframework
                     libffi.xcframework
    Resources/       Pharo.image, Pharo.changes, Pharo.sources, startup.st
    Assets.xcassets/ User-provided app icon (or placeholder)
    WatchApp/        (optional) SwiftUI companion app for Apple Watch
    Info.plist       Bundle config (iOS 16.0+ deployment target)
    .entitlements    Sandbox + network permissions
    .pbxproj         Hand-generated (no xcodegen dependency)
    build.sh         CLI build convenience script

**Kiosk mode** hides TaskbarMorph, MenubarMorph, and World menu pragmas via
startup.st, then garbage-collects 3x to reduce memory.

**Image stripping** (optional, on by default): startup.st removes Iceberg,
Calypso, NewTools, tests, debugger, Metacello, and other dev packages on
first launch, then runs `Smalltalk cleanUp: true` and 3x GC.

**watchOS companion** (optional): A second Xcode target builds a simple
SwiftUI watch app. The iOS target has a dependency on it and embeds it
via "Embed Watch Content". Pharo does not run on watchOS — the companion
is a placeholder that could be extended with WatchConnectivity later.


## 5. Platform Support: iOS, Mac, and watchOS

### 5.1 Current State

The exported app currently supports two platforms from a single Xcode target:

- **iOS** (iPhone + iPad) -- arm64, iOS 16.0+
- **Mac Catalyst** -- arm64 + x86_64, macOS 14.0+

Both build from the same target because Mac Catalyst uses the iOS SDK.
Platform-specific code is gated with `#if targetEnvironment(macCatalyst)`.

### 5.2 How Other Systems Handle Multiple Targets

**Mac Catalyst (what we use):**
- One Xcode target builds for both iPad and Mac
- Same iOS SDK, macOS gets a UIKit translation layer
- Intel + Apple Silicon Macs supported
- Swift Playgrounds uses this approach

**"Designed for iPad" (zero-effort Mac):**
- Any iPad app is automatically available on Apple Silicon Macs
- Exact same iOS binary runs unmodified, UI scaled to 77%
- No Intel Mac support, no Mac customization
- Pythonista and Codea use this (no Mac-specific work)

**Separate native macOS target:**
- Built against the macOS SDK with AppKit
- Full Mac-native controls and behavior
- Requires maintaining a separate target
- UTM does this: iOS version uses interpretation, Mac version uses Hypervisor.framework

**Game engines (Unity, Unreal):**
- Separate build targets per platform, each generates its own Xcode project
- Neither supports Mac Catalyst
- No watchOS support from either

### 5.3 Multi-Target Architecture for Export

To support all Apple platforms, the exported project would need:

    Target                  Platforms              SDK          Notes
    ------                  ---------              ---          -----
    Main App                iPhone, iPad, Mac      iOS (Catalyst) Current setup, one target
    Watch App               Apple Watch            watchOS        Separate target required
    (Optional) Native Mac   macOS standalone       macOS          Only if Catalyst isn't enough

watchOS always requires its own target -- Apple does not allow a single
target to build for both iOS and watchOS.

**Shared code approach:** The VM xcframework already builds for multiple
architectures. Adding a watchOS slice (arm64) to `build-xcframework.sh`
would let the same C++ VM compile for watchOS. The Swift app code would
need `#if os(watchOS)` conditionals for the different UI (no Metal on
watchOS -- SpriteKit only for rendering).

**Universal Purchase:** If the iOS and Mac versions share the same bundle ID
(which they do via Catalyst), customers buy once for all platforms. A watchOS
version could be added to the same App Store listing.

### 5.4 watchOS Feasibility

**Hardware constraints (Apple Watch Series 10 / Ultra 2):**

    RAM:            1 GB total, ~30-50 MB per foreground app (jetsam limit)
    Storage:        64 GB
    App size limit: 75 MB total bundle
    Architecture:   arm64 (Series 4+), arm64_32 (Series 3, being phased out)
    Rendering:      SpriteKit only (no Metal, no SceneKit)
    JIT:            Not allowed (same as iOS)

**What fits:**

    VM binary:          ~2-5 MB     (fits easily)
    Standard image:     ~56 MB      (too large for 30-50 MB RAM limit)
    Minimal image:      ~10-15 MB   (might fit, very tight)
    Frameworks:         ~25 MB      (would need stripping)
    Total bundle:       ~40-45 MB   (fits 75 MB limit if image is minimal)

**Verdict:** Theoretically possible with a heavily stripped minimal image
and a SpriteKit-based renderer (no Metal on watchOS). The ~30-50 MB RAM
limit is the hard constraint -- the VM heap would need to stay very small.
No known Smalltalk has ever run on watchOS. A Lua interpreter has been
demonstrated (watchos-lua on GitHub), proving embedded interpreters work
on watchOS in principle.

**Practical use case for watchOS:** A Pharo app on a watch would need to
be extremely focused -- a single glanceable view, complication data,
or a simple utility. The full Pharo IDE obviously cannot run on a watch.
A kiosk-mode app with a stripped image containing only the app logic
could potentially work.


## 6. Apple App Store Acceptance

### 6.1 Will Apple Accept Apps Built by Export?

**Yes.** There is strong precedent and no policy conflict. Here's why:

**Guideline 2.5.2 (the key rule):**

    "Apps should be self-contained in their bundles, and may not read or
    write data outside the designated container area, nor may they
    download, install, or execute code which introduces or changes
    features or functionality of the app, including other apps."

An exported app satisfies every requirement:
- The VM binary is bundled in the app (self-contained)
- The Pharo image is bundled as a resource (self-contained)
- No code is downloaded at runtime
- The app's features are fixed at build time
- In kiosk mode, the user cannot even access the Smalltalk IDE

**JIT is forbidden, but we don't use JIT.** Our VM is a pure bytecode
interpreter (switch-dispatch). Apple's restriction is on generating
native machine code at runtime (W^X memory pages). Interpreting
bytecodes through pre-compiled C++ functions is explicitly allowed.

**Educational exception (added June 2017):** Apps that teach, develop,
or allow students to test executable code may even download code,
provided the source is viewable and editable. This gives additional
cover if the exported app exposes Smalltalk as a user-facing feature.

### 6.2 Precedent Apps on the App Store

These apps all embed language VMs/interpreters and are approved:

    App             Language    Approach              On Store Since
    ---             --------    --------              --------------
    Pythonista 3    Python      CPython 3.10 embedded ~2013
    Codea           Lua         Lua interpreter       2011
    iSH Shell       x86 Linux   Usermode emulation    2019
    a-Shell         Multi       C, Python, Lua, etc.  2019
    Play.js         JavaScript  Node.js embedded      ~2019
    Scriptable      JavaScript  JavaScriptCore        ~2018
    Delta           NES/SNES    CPU interpretation     2024
    PPSSPP          PSP         CPU interpretation     2024
    RetroArch       Multi       Multi-system interp.   2024

iSH and a-Shell both faced 2.5.2 removal threats over wget/curl/pip
(downloading executable code). Both won their appeals. The issue was
downloading code, not embedding an interpreter -- which was never
questioned.

**Smalltalk specifically:** John McIntosh had Squeak-based apps on the
App Store years ago. The constraint was "no programmability for the end
user" (i.e., kiosk mode) and no downloading code.

### 6.3 Potential Risks and Mitigations

**Risk 1: "The app is just a wrapper around an interpreter"**

Apple sometimes rejects apps that are thin wrappers with no native
functionality. Mitigation: the exported app has a full native Swift UI
(Metal renderer, touch handling, keyboard support), not just a web view
or script runner. The Pharo image provides the application logic, but
the app itself is a substantial native binary.

**Risk 2: Code downloading**

If the Pharo app loads packages from the network at runtime (e.g.,
Metacello load scripts), Apple could argue this violates 2.5.2.
Mitigation: kiosk mode disables the IDE and package manager. The
exported image should have all dependencies pre-loaded.

**Risk 3: "Minimum functionality" (guideline 4.2)**

Apple rejects apps that don't do enough. An exported app that just
shows a Playground or empty Morphic world could be rejected.
Mitigation: the exported app should have a clear purpose (a game,
tool, utility) built in Smalltalk, not just be an empty environment.

**Risk 4: Binary size**

The full stack is ~96 MB. Apple's limit is 4 GB for iOS, 200 MB for
over-cellular download without Wi-Fi. The app is well within limits.
App Store thinning (bitcode slicing) will reduce the download size
further since the xcframework contains multiple architectures.

**Risk 5: "Vibe coding" crackdown (March 2026)**

Apple recently began blocking AI coding apps that use LLMs to generate
and execute code. This targets AI-generated code changing app
functionality, NOT traditional interpreter apps. Pythonista, Codea,
and iSH are unaffected. Our exported apps are also unaffected --
the Pharo code is fixed at build time, not generated by AI at runtime.

### 6.4 Recommendations for Export

To maximize App Store acceptance:

1. **Enable kiosk mode by default** -- hide IDE, package manager, code browser
2. **Pre-load all dependencies** -- no Metacello loads at runtime
3. **Bundle everything** -- no network fetches for code or images
4. **Use a real bundle ID** -- not com.example.anything
5. **Provide a custom app icon** -- placeholder icons trigger rejection
6. **Write a clear App Store description** -- describe what the app does,
   not that it runs on a Smalltalk VM
7. **Image cleanup** -- strip IDE, tests, VCS from the bundled image
   (reduces size and removes any code-editing capability)


## 7. Analysis: What Could Be Done Better

### Current state: VM + full image (Pharo/Squeak tier)

The exported app bundles:
- Full C++ VM (~15 MB in xcframework)
- Full Pharo 13 image (~56 MB)
- Supporting frameworks (~25 MB)
- Total: ~96 MB before App Store thinning

### Tier 1: Image stripping (VisualWorks/Dolphin approach)

Remove unused code from the image before bundling. Pharo has some support:

- `Smalltalk cleanUp: true` -- removes caches, logs, dead instances
- Unload packages: `(RPackageOrganizer default packageNamed: 'IDE') removeFromSystem`
- Remove test classes, development tools, code browsers, VCS (Iceberg)

Dolphin's approach (selector-based reachability) is the gold standard but
requires careful handling of Smalltalk's dynamic dispatch. A conservative
approach: identify the root classes/methods the app uses, then remove
packages that are never referenced. This could cut the image from ~56 MB
to ~10-15 MB for a typical app.

**Feasibility: Medium.** Pharo images are deeply interconnected. Removing
packages can trigger cascade failures from unexpected dependencies.
A package-level approach (remove IDE, Iceberg, tests, debugger, etc.)
is safer than method-level stripping. Could be automated in startup.st
or as a pre-export step.

### Tier 2: AOT compilation (Smalltalk/X approach)

Compile Smalltalk methods to C or LLVM IR, then to native code. This
eliminates the bytecode interpreter for compiled methods.

**Feasibility: Low for this project.** Would require:
- A Smalltalk-to-C compiler (STC exists for ST/X but not for Pharo/Spur)
- Handling Pharo's object model (Spur format, become:, etc.)
- Preserving dynamic dispatch (vtable or inline cache approach)
- The runtime library (GC, object model, FFI) would still be needed

This is years of work and would diverge from standard Pharo compatibility.

### Tier 3: Transpilation (Amber/PharoJS approach)

Compile Smalltalk to another language (JS, Swift, C). No VM needed at all.

**Feasibility: Very low for general Pharo apps.** PharoJS works for
web-style apps but cannot handle Morphic, BitBlt, or the full Pharo
class library. A Swift transpiler would be a multi-year research project.

### Recommended next steps

1. **Image cleanup in export** -- DONE (build 99):
   "Strip Development Tools" toggle in Export sheet. When enabled,
   startup.st unloads Iceberg, Calypso, NewTools, tests, debugger,
   Metacello, and other dev packages on first launch, then runs
   `Smalltalk cleanUp: true` and 3x GC. Estimated savings: ~30-40 MB.

2. **Package-level stripping UI** (medium effort, future):
   Let users select which packages to keep in the export sheet.
   Show package sizes and dependency warnings. Currently the strip
   toggle is all-or-nothing; finer control would let users keep
   specific tools they need.

3. **Minimal image export** (medium effort, future):
   Start from a Pharo minimal image instead of the full IDE image.
   Load only the packages the app needs. This mirrors Cuis's philosophy.

4. **Custom app icon** -- DONE (build 99):
   "App Icon" section in Export sheet with a file picker for PNG/JPEG.
   Selected icon is copied into Assets.xcassets/AppIcon.appiconset with
   all required size entries pointing to the single file (Xcode scales).
   Preview shown in the export sheet.

5. **watchOS target** (high effort, experimental):
   Add a watchOS slice to the xcframework, create a minimal
   SpriteKit-based renderer, and require a stripped image <15 MB.


## 8. Prograph vs. iospharo Comparison

    Aspect              Prograph (1995)         iospharo (2026)
    ----                --------                --------
    Language type       Visual dataflow         Bytecode (Sista V1)
    Development env     IDE with interpreter    Pharo IDE in-image
    Deployment          Compiled native exe     VM + image bundle
    Tree shaking        Class-level removal     None (full image)
    VM in output        No (compiled away)      Yes (full C++ VM)
    C integration       Prograph C Tools Kit    FFI (libffi)
    App Store ready     Yes (native binary)     Yes (interpreter OK)
    Output size         Small (stripped)         ~96 MB (full stack)
    Dead code           Automatic               Manual (cleanUp)
    Platforms           Mac, Windows            iOS, iPad, Mac
    watchOS             N/A                     Possible (untested)

Prograph's compiler could eliminate the interpreter entirely because it
compiled dataflow graphs to native code. This isn't directly applicable
to Pharo because Pharo's image model (live objects, become:, reflection,
doesNotUnderstand:) fundamentally relies on the VM's object model and
message dispatch at runtime.

The most practical path for iospharo is image stripping (Tier 1), which
Prograph also did (ABC class removal) but combined with native compilation.
