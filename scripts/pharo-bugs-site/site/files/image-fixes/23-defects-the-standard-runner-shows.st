"Two image defects that show when the tests run with the plain SUnit rules
 (run_sunit.st: no change of the image, SUnit time limits, a Warning is a
 failure).  Measured 2026-11-17 on stock Cog and on our VM.

 R1. OutOfMemoryTest>>testSignalOfTheLowSpaceFromTheVM needs the Low Space
 Watcher process to wait on the low-space semaphore.  Another test can end that
 process for the rest of the run: OupsDebuggerSelectorTest>>tearDown sends
 DebugSession>>terminate, which installs a NEW watcher (Smalltalk
 installLowSpaceWatcher), and the SUnit process monitor terminates that new
 process at the end of the test as a process the test left behind.  The next
 test that needs the watcher then waits until its time limit (TIMEOUT,
 TestTookTooMuchTime, 10 s).  The same two classes in this order give the
 timeout on the two VMs.  Fix: the test class prepares its own precondition, as
 every other test of the class prepares its own heap limit.

 R2. The package Collections-Unordered declares System-Support as a manually
 resolved dependency, and the dependency analyser does not find that dependency
 any more.  The analyser then signals StPotentialOutDatedDependencyWarning, and
 SUnit counts a Warning in a test as a failure
 (StTarjanAlgorithmTest>>testNoOutgoingDependenciesAfterTarjan).  Stock Cog
 fails the same way with the same image.  The older runner resumed the Warning,
 so the failure did not show.  The manifest entry is out of date; remove it."

fix value: #OutOfMemoryTest value: #setUp
	value: [ :source | (source includesSubstring: 'installLowSpaceWatcher') not ]
	value:
'setUp

	super setUp.
	oldValue := Smalltalk vm maxOldSpaceSize: 2 * 1024 * 1024 * 1024.
	"The test needs a running Low Space Watcher process.  An earlier test can
	 leave none (see the process monitor of SUnit)."
	Smalltalk installLowSpaceWatcher'.

fix value: #'ManifestCollectionsUnordered class' value: #manuallyResolvedDependencies
	value: [ :source | source includesSubstring: 'System-Support' ]
	value:
'manuallyResolvedDependencies
	^ #(#''Collections-Streams'' #''Math-Operations-Extensions'')'.
