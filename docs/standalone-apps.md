# Standalone App Building: Survey & Status

2026-03-20

A survey of how interpreted-language systems (Prograph, various Smalltalks,
and this project) build standalone distributable applications.

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
- User configures: app name, bundle ID, team ID, platform (iOS/Mac/both), kiosk mode
- Generated project includes: Swift sources, Metal shaders, xcframeworks, embedded image

**What gets bundled:**

    Sources/         6 Swift files + Metal shader (generated from templates)
    Headers/         VMParameters.h, MotionData.h (stubs)
    Frameworks/      PharoVMCore.xcframework (VM + cairo/freetype/harfbuzz/etc.)
                     SDL2.xcframework
                     libffi.xcframework
    Resources/       Pharo.image, Pharo.changes, Pharo.sources, startup.st
    Assets.xcassets/ Placeholder app icon
    Info.plist       Bundle config (iOS 16.0+ deployment target)
    .entitlements    Sandbox + network permissions
    .pbxproj         Hand-generated (no xcodegen dependency)
    build.sh         CLI build convenience script

**Kiosk mode** hides TaskbarMorph, MenubarMorph, and World menu pragmas via
startup.st, then garbage-collects 3x to reduce memory.

**No image stripping.** The full Pharo image is bundled as-is. No dead code
elimination, no class/method removal, no reachability analysis.


## 5. Analysis: What Could Be Done Better

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

1. **Image cleanup in export** (low effort, high impact):
   Add a pre-export step that runs `Smalltalk cleanUp: true` and
   unloads development packages (Iceberg, IDE tools, tests, debugger,
   code browser). This could halve image size with minimal risk.

2. **Package-level stripping UI** (medium effort):
   Let users select which packages to keep in the export sheet.
   Show package sizes and dependency warnings.

3. **Minimal image export** (medium effort):
   Start from a Pharo minimal image instead of the full IDE image.
   Load only the packages the app needs. This mirrors Cuis's philosophy.

4. **Custom app icon** (low effort, nice to have):
   Let users pick an icon image in the export sheet instead of
   requiring manual Assets.xcassets editing.


## 6. Prograph vs. iospharo Comparison

    Aspect              Prograph (1995)         iospharo (2026)
    ----                --------                --------
    Language type       Visual dataflow         Bytecode (Sista V1)
    Development env     IDE with interpreter    Pharo IDE in-image
    Deployment          Compiled native exe     VM + image bundle
    Tree shaking        Class-level removal     None (full image)
    VM in output        No (compiled away)      Yes (full C++ VM)
    C integration       Prograph C Tools Kit    FFI (libffi)
    App Store ready     Yes (native binary)     Yes (interpreter OK per Apple)
    Output size         Small (stripped)         ~96 MB (full stack)
    Dead code           Automatic               Manual (cleanUp)

Prograph's compiler could eliminate the interpreter entirely because it
compiled dataflow graphs to native code. This isn't directly applicable
to Pharo because Pharo's image model (live objects, become:, reflection,
doesNotUnderstand:) fundamentally relies on the VM's object model and
message dispatch at runtime.

The most practical path for iospharo is image stripping (Tier 1), which
Prograph also did (ABC class removal) but combined with native compilation.
