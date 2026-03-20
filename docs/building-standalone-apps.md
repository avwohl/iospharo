# Building a Standalone App from Your Pharo Image

This guide walks you through turning a Pharo image into a standalone iOS or
Mac app that you can share with others or submit to the App Store. It assumes
you know Pharo but have never used Xcode, Apple Developer accounts, or app
distribution before.


## Overview

The "Export as App" feature takes your running Pharo image and wraps it in a
native Apple app. The result is a normal app — an icon on the home screen or
in the Dock — that launches directly into your Pharo code. No Pharo knowledge
is needed by the end user; they just tap the icon.

What gets generated:

    YourApp/
      YourApp.xcodeproj/    Xcode project (like a Pharo .project file)
      Sources/              Swift code that boots the VM + renders via Metal
      Resources/            Your .image, .changes, .sources, and startup.st
      Frameworks/           The VM engine (PharoVMCore) + SDL2 + libffi
      Assets.xcassets/      Your app icon
      WatchApp/             (if watchOS enabled) SwiftUI companion for Apple Watch
      Info.plist            App metadata (name, version, bundle ID)
      build.sh              Command-line build script (optional shortcut)


## Prerequisites

Before you start, you need two things on your Mac:

1.  Xcode (free from the Mac App Store)

    Xcode is Apple's IDE — think of it as the Pharo Launcher but for Swift/C++
    apps. You don't need to learn Swift; Xcode just compiles and signs the
    generated project. Install it, open it once to accept the license, and
    let it install the command-line tools when prompted.

2.  An Apple Developer account (maybe)

    What you want to do          Account needed         Cost
    --------------------------   --------------------   -----------
    Build and run on Mac         Free Apple ID          $0
    Build and run in Simulator   Free Apple ID          $0
    Run on your own iPhone/iPad  Free Apple ID          $0 (*)
    Send to beta testers         Developer Program      $99/year
    Submit to the App Store      Developer Program      $99/year

    (*) Free accounts can install on up to 3 personal devices. Apps
    expire after 7 days and must be reinstalled. The paid account
    removes both limits.

    For the free path: just sign into Xcode > Settings > Accounts >
    add your Apple ID. That's it — you can build, run on simulators,
    and install on your own devices today.

    For distribution: enroll at developer.apple.com/programs. Approval
    takes 24-48 hours. This gives you a Team ID, App Store Connect
    access, TestFlight, and apps that don't expire on devices.

    You can do everything through Step 4 of this guide for free and
    upgrade later when you're ready to share with others.


## Step 1: Prepare Your Image

Open iospharo and launch the image you want to export. Make sure your app
is in the state you want end users to see:

-   If you're building a game, tool, or utility: get it working the way
    you want. Save the image (Cmd+S or World menu > Save).

-   Remove anything you don't want shipped. The export can strip
    development tools automatically (see Step 2), but if you have
    personal code, test data, or credentials in the image, clean those
    out first.

-   Test your app by quitting and relaunching — make sure it comes up
    cleanly from a cold start, since that's what your users will
    experience.


## Step 2: Export

Right-click your image in the iospharo image library and choose
"Export as App..." (this option only appears on Mac, not on iPad/iPhone).

The export sheet has these sections:

    App Configuration
    -----------------
    App Name        The name shown under the icon. Letters and numbers only.
                    Example: "MyGame"

    Bundle ID       A reverse-DNS identifier that uniquely identifies your app
                    worldwide. Example: "com.yourname.mygame"
                    This is like a DNS name for your app. Two apps cannot have
                    the same bundle ID on the App Store. Pick something unique
                    to you. The default "com.example.xxx" is fine for testing
                    but must be changed before App Store submission.

    Team ID         Your 10-character Apple Developer Team ID (e.g. "A1B2C3D4E5").
                    Find it at developer.apple.com > Membership > Team ID.
                    Leave blank for local testing with a free account — Xcode
                    will figure it out automatically.

    Platform
    --------
    macOS           Builds a Mac app (via Mac Catalyst — same code, runs on Mac)
    iOS             Builds an iPhone/iPad app
    watchOS         Adds a companion app for Apple Watch. The watch app is
                    built and embedded automatically when you build the iOS
                    target — one build produces both. Pharo doesn't run on
                    the Watch; this is a placeholder companion that ships
                    alongside your iOS app. You can customize the watch
                    SwiftUI code later in the generated WatchApp/ folder.
                    You can enable all three. iOS + macOS is the common case.

    Image Options
    -------------
    Kiosk Mode      ON (default): Hides the Pharo taskbar, menu bar, and World
                    menu. Your app fills the entire screen. End users never see
                    the Smalltalk IDE. This is what you want for a shipping app.

                    OFF: Keeps the full Pharo environment visible. Useful if your
                    app IS a development tool or you want users to inspect code.

    Strip Dev Tools ON (default): On first launch, the app removes the IDE,
                    debugger, Iceberg (git), test frameworks, code browser, and
                    other development packages. Saves ~30-40 MB.

                    OFF: Ships the full ~56 MB image. Turn this off if your app
                    needs the IDE or if stripping causes issues.

    App Icon
    --------
    Choose a PNG or JPEG image for your app icon. Recommended: 1024x1024
    pixels, square, no transparency. Xcode generates all the smaller sizes
    automatically. If you skip this, the app gets a blank placeholder (which
    Apple will reject if you submit to the App Store).

    Output
    ------
    Where to save the generated project folder. Default is Desktop.

Click "Export". The progress indicator shows each phase (copying image,
generating project files, copying frameworks). When it finishes, you'll
see buttons to "Open in Xcode" or "Show in Finder".


## Step 3: Build in Xcode

Click "Open in Xcode". Xcode opens the generated project. Here's a quick
orientation — you only need to know three things:

    The toolbar at the top
        Left side:  Play button (builds and runs)
        Middle:     Scheme selector (which device to run on)
        Right:      Status indicator (build progress, errors)

    The file list on the left
        Your project structure. You shouldn't need to edit anything.

    The editor in the center
        Shows whatever file you've selected. Again, you don't need to
        edit the Swift code — it's all generated.

To build and run:

1.  In the toolbar, click the device selector (it says something like
    "My Mac (Mac Catalyst)" or "Any iOS Device"). Pick:
    - "My Mac (Mac Catalyst)" to run on your Mac right now
    - A connected iPhone/iPad if you plugged one in
    - A simulator (e.g., "iPhone 16 Pro") for testing without a device

2.  Click the Play button (or press Cmd+R).

3.  First build takes 1-2 minutes (subsequent builds are faster). Xcode
    compiles the Swift code, links the VM framework, copies your image
    into the app bundle, and signs it.

4.  If the build succeeds, the app launches. You should see your Pharo
    image running as a standalone app.

If you see a signing error on first build:

    Xcode may say "Signing requires a development team." Click the
    project name in the file list (top item), then the "Signing &
    Capabilities" tab, and pick your team from the dropdown.
    If you only have a free Apple ID, select "Personal Team".

If you see "No such module" or link errors:

    The frameworks may not have copied correctly. Check that the
    Frameworks/ folder in the project contains PharoVMCore.xcframework,
    SDL2.xcframework, and libffi.xcframework.


## Step 4: Test on a Real Device

Simulators are useful but don't run Metal shaders the same way a real
device does. To test on a real iPhone or iPad:

1.  Connect the device via USB (or Wi-Fi after initial pairing).

2.  On the device: Settings > Privacy & Security > Developer Mode > ON.
    The device will restart. (This is an Apple security requirement.)

3.  In Xcode, select your device from the scheme selector.

4.  Press Play (Cmd+R). Xcode installs the app on the device.

5.  First time only: The device may say "Untrusted Developer". On the
    device, go to Settings > General > VPN & Device Management, tap
    your developer certificate, and tap "Trust".

The app now launches on the device. It works exactly like it did in the
simulator or on Mac.


## Step 5: Distribute to Testers (TestFlight)

TestFlight is Apple's system for sending beta builds to testers. It's free
and works with the $99/year developer account.

Concepts:

    App Store Connect     Apple's web portal for managing apps. Think of
                          it as the "admin panel" for your app listing.
                          (appstoreconnect.apple.com)

    TestFlight            A free iOS/Mac app that testers install to
                          receive your beta builds.

    Archive               A release-ready build of your app (like a
                          Pharo .image snapshot, but compiled native code).

Steps:

1.  Log into App Store Connect (appstoreconnect.apple.com) with your
    Apple Developer account.

2.  Click "My Apps" > the "+" button > "New App".

        Platform:    iOS (and/or macOS)
        Name:        Your app's display name
        Bundle ID:   Must match what you entered in Step 2 (e.g., com.yourname.mygame).
                     If it doesn't appear in the dropdown, go to
                     developer.apple.com > Certificates, Identifiers & Profiles >
                     Identifiers > "+" and register the bundle ID first.
        SKU:         Any unique string (e.g., "mygame-v1"). This is internal only.

3.  Back in Xcode, change the scheme selector from "Debug" to "Release":
    click the scheme name > Edit Scheme > Run > Build Configuration > Release.

4.  In the Xcode menu bar: Product > Archive. This builds a release
    version and opens the Organizer window.

5.  In the Organizer, select your archive and click "Distribute App".
    Choose "TestFlight & App Store" > Upload.

6.  Wait 15-30 minutes for Apple to process the build. You'll get an
    email when it's ready.

7.  In App Store Connect, go to your app > TestFlight tab. You'll see
    your build listed. Add testers by email — they'll get an invitation
    to install via the TestFlight app.

Testers install TestFlight from the App Store, tap your invite link, and
the app appears on their device. You can have up to 10,000 testers.


## Step 6: Submit to the App Store

When you're satisfied with testing:

1.  In App Store Connect, fill out the app listing:
    - Screenshots (required: at least one per device size)
    - Description, keywords, category
    - Privacy policy URL (required even if your app collects nothing —
      a simple page saying "this app collects no data" suffices)
    - App Review notes: Describe what the app does. Don't mention
      "Smalltalk VM" or "bytecode interpreter" — just describe the
      user-facing functionality. ("A puzzle game where you arrange
      colored blocks.")

2.  Select your TestFlight build as the release build.

3.  Click "Submit for Review".

4.  Apple's review typically takes 24-48 hours. They'll either approve
    it or send feedback about what to fix.

Tips for App Store approval:

    - Use kiosk mode. Apple reviewers don't know Smalltalk; if they see
      a code browser, they may reject it as "not enough functionality"
      or flag the code editing as a risk.

    - Provide a real app icon (not the placeholder).

    - Your app must do something useful. An empty Morphic world will be
      rejected under guideline 4.2 (Minimum Functionality).

    - Don't download code at runtime. If your app loads packages from
      the network via Metacello, Apple may reject it under guideline
      2.5.2. Pre-load all dependencies before export.

    - The "Strip Development Tools" option removes the package manager,
      which helps avoid questions about code downloading.


## Appendix A: Glossary for Smalltalk Developers

    Xcode project (.xcodeproj)
        Like a Pharo .project or Metacello baseline — declares what
        source files to compile, what libraries to link, and how to
        build the app. The export generates this for you.

    Bundle ID (e.g., com.yourname.mygame)
        A globally unique reverse-DNS identifier for your app. Like a
        Smalltalk class name but for the entire app on every Apple
        device in the world. Once registered with Apple, it's yours.

    Team ID
        A 10-character string that identifies your Apple Developer
        account. Used for code signing. Found at developer.apple.com
        under Membership.

    Code Signing
        Apple requires every app to be cryptographically signed with
        a certificate tied to your developer account. This is how
        iOS knows who made the app and that it hasn't been tampered
        with. Xcode handles this automatically ("Automatic Signing").

    Provisioning Profile
        A file that ties together your signing certificate, your app's
        bundle ID, and which devices can run it. Xcode manages these
        automatically. You'll rarely interact with them directly.

    Entitlements
        Permissions your app requests (network access, camera, etc.).
        The exported project includes network.client and network.server
        by default (needed for Pharo's network stack). You don't need
        to change these unless your app uses hardware like the camera.

    Archive
        A release build of your app, packaged for distribution. Like
        doing "Save as" on your Pharo image but producing a signed
        native binary instead.

    TestFlight
        Apple's beta distribution service. Free. Testers install the
        TestFlight app, you add them by email, and they get your
        latest builds automatically.

    App Store Connect
        The web portal where you manage your app listing, add
        screenshots, submit for review, and view sales/analytics.
        (appstoreconnect.apple.com)

    Mac Catalyst
        Apple's technology that lets iPad apps run on macOS. The
        exported project uses this: one codebase, one target, runs
        on both iPhone/iPad and Mac. The alternative would be
        maintaining two separate apps.

    xcframework
        A bundle containing compiled libraries for multiple platforms
        (iOS arm64, Mac arm64, Mac x86_64, simulator). Like a Pharo
        package but pre-compiled to native code. PharoVMCore.xcframework
        contains the entire VM plus Cairo, FreeType, HarfBuzz, etc.


## Appendix B: Command-Line Build (No Xcode GUI)

If you prefer the terminal, the generated project includes build.sh:

    cd ~/Desktop/MyGame
    ./build.sh

This calls xcodebuild under the hood. You still need Xcode installed
(for the compiler and SDKs), but you don't need to open the GUI.

For more control:

    # Build for Mac
    xcodebuild \
      -project MyGame.xcodeproj \
      -scheme MyGame \
      -destination 'platform=macOS,variant=Mac Catalyst' \
      -configuration Release \
      build

    # Build for iOS device
    xcodebuild \
      -project MyGame.xcodeproj \
      -scheme MyGame \
      -destination 'generic/platform=iOS' \
      -configuration Release \
      build

    # Archive for distribution
    xcodebuild \
      -project MyGame.xcodeproj \
      -scheme MyGame \
      -destination 'generic/platform=iOS' \
      -configuration Release \
      archive \
      -archivePath ./MyGame.xcarchive


## Appendix C: What Happens at Runtime

When a user launches your exported app, this is the sequence:

1.  The Swift app starts, creates a Metal rendering surface.

2.  PharoBridge loads the embedded Pharo.image from the app bundle.

3.  startup.st runs automatically (Pharo's StartupPreferencesLoader):
    - Patches the GitHub API to avoid a Pharo 13 bug
    - If "Strip Dev Tools" was enabled: removes IDE packages, cleans
      caches, runs garbage collection (first launch only — changes
      are saved back to the image)
    - If kiosk mode: hides taskbar, menu bar, World menu

4.  Your Pharo app code runs. Display output goes through SDL2 stubs
    to the Metal renderer. Touch/mouse/keyboard events flow from
    UIKit through PharoBridge into the Pharo event queue.

5.  The app is a normal iOS/Mac citizen: it suspends on background,
    resumes, handles rotation, keyboard, and multitasking.

The user never sees Smalltalk, bytecodes, or the VM. They see your app.
