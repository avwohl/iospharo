"mumez/PharoSmalltalkInteropServer: 35 tests error on our VM with 'ConnectionClosed:
Connection aborted to 127.0.0.1:8086' and testStatusStringWhenRunning fails.  Stock Cog
passes all 44.  No VM defect: the socket plugin, Zinc and Teapot work on our VM when the
server runs (measured).

Cause.  The package starts its server (SisServer current) lazily and from the image
startup: SisServer class>>startUp: adds a deferred startup action that creates the
server.  The tests (SisTest) send HTTP requests to SisServer current and have no setUp.
Our test runner runs the whole suite inside the startup handler of the image, before the
deferred startup actions run, so the first test that calls SisServer current starts the
server inside the test.  SUnit (ProcessMonitorTestService) terminates every process that
a test forks, so the Zinc listener process dies at the end of that test.  SisServer current
still answers the stopped server, so every later test gets a refused connection and
testStatusStringWhenRunning sees isRunning=false.  Stock Cog runs the suite from a
command line handler that runs after the deferred actions, so the server was started
outside any test and no test terminates its process.  The same loss happens on any VM
when the first call of SisServer current is made inside a test.

Repair 1 (package test): SisTest>>setUp makes sure that the server runs, and starts it
again when it does not run.  The server starts with DefaultExecutionEnvironment active,
so SUnit does not count the Zinc processes as processes of the test and does not
terminate them.  This matters for testRunClassTest and testRunPackageTest too: the
server runs SUnit inside a request, and the cleanup of that nested run terminated the
server processes of the outer test, which closed the connection of the request.  The
assertions are unchanged.  When the server already runs (Cog) the setUp does nothing.

Repair 2 (package code): testStatusStringWhenRunning only passed when an earlier test
had put a setting into the server.  SisServer>>statusString printed the empty list of
settings as 'settings=#()', because an empty Array prints in the literal form, and a
list with entries as 'settings={...}'.  The method now prints '{}' for no settings, so the
text has one format in every state.  The text for a list with entries is the same as
before, so testStatusStringWhenNotRunning and testPrintOn are unchanged."

add value: #SisTest value: #setUp value: 'running'
	value:
'setUp
	super setUp.
	DefaultExecutionEnvironment beActiveDuring: [
		SisServer current isRunning ifFalse: [ SisServer restart ] ]'.

fix value: #SisServer value: #statusString
	value: [ :source | source includesSubstring: 'associations printString' ]
	value:
'statusString
	| associations |
	associations := self settings settingsDict associations.
	^ ''isRunning={1}, settings={2}'' format: {
			  self isRunning printString.
			  associations isEmpty
				  ifTrue: [ ''{}'' ]
				  ifFalse: [ associations printString ] }'.
