"
iOS Pharo Image Setup Script

Run this script in a Pharo image to prepare it for iOS deployment.
It installs the iOSWorldRenderer and configures startup.

Usage:
  1. Open a Pharo image on desktop
  2. Open a Playground
  3. File in iOSWorldRenderer.st first
  4. Then run this script
  5. Save the image
"

"Step 1: Install the iOSWorldRenderer as the world renderer"
| renderer |
renderer := iOSWorldRenderer new.
renderer world: World.
World worldState instVarNamed: 'worldRenderer' put: renderer.
renderer activate.

"Step 2: Register startup action to ensure renderer is active on image load"
SessionManager default
    registerUserClassNamed: #iOSWorldRenderer
    atPriority: 90.

"Step 3: Add startup method to iOSWorldRenderer"
iOSWorldRenderer class compile: '
startUp: resuming
    "Called on image startup - ensure we are the active renderer"
    resuming ifTrue: [
        | renderer |
        renderer := self new.
        renderer world: World.
        World worldState instVarNamed: ''worldRenderer'' put: renderer.
        renderer activate.
        World fullRepaintNeeded ]'.

iOSWorldRenderer class compile: '
shutDown: quitting
    "Called on image shutdown"
    quitting ifTrue: [ World worldState worldRenderer deactivate ]'.

"Step 4: Force initial display"
World fullRepaintNeeded.
World displayWorld.

Transcript show: 'iOS World Renderer installed successfully!'; cr.
