"
iOS OSWindow Driver Installation Script

Run this in a Pharo 10+ image to add iOS display support.
After installation, save the image for use on iOS.

Usage:
  1. Open Pharo on desktop
  2. File in OSiOSDriver.st
  3. Run this script
  4. Save the image
  5. Bundle the image with your iOS app
"

"Register the driver classes with the system"
OSWindowDriver allSubclasses
    detect: [:c | c name = #OSiOSDriver]
    ifNone: [self error: 'OSiOSDriver not loaded. File in OSiOSDriver.st first.'].

"Add iOS driver to the driver selection mechanism"
OSWindowDriver class compile: '
driverClass
    "Return the appropriate driver for the current platform"
    | candidates |
    candidates := self allSubclasses select: [:c | c isSupported].
    candidates isEmpty ifTrue: [^ OSNullWindowDriver].
    "Prefer iOS driver if available and supported"
    candidates
        detect: [:c | c name = #OSiOSDriver and: [c isSupported]]
        ifFound: [:c | ^ c].
    "Otherwise use default selection"
    ^ (candidates sorted: [:a :b | a priority > b priority]) first'.

"Register for startup"
SessionManager default
    registerSystemClassNamed: #OSiOSDriver
    atPriority: 50.

Transcript
    show: 'iOS OSWindow Driver installed!'; cr;
    show: 'Save this image for iOS deployment.'; cr.
