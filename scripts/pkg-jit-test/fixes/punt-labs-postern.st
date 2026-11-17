"punt-labs/postern: repairs for the six tests that did not pass.

1. PosternHelpTest>>canParseExpression: and canParseMethod: catch CodeError.
   Pharo 14 signals OCCodeError (the superclass of CodeError) from the parser
   (OCNotice>>signalError), so the handler never runs: the first snippet of a
   code fence that is a method definition (for example 'fromJson: aDictionary
   ...') does not parse as an expression, the OCCodeError escapes, and
   testCodeFencesParse ends with 'Variable or expression expected' before the
   method parse runs.  The two helpers now catch OCCodeError and use OCParser
   (RBParser is a deprecated alias in Pharo 14).

2. PosternHelp (the help text) was written for Pharo 12.  Review for Pharo 14:
   RPackageOrganizer does not exist (use 'self packageOrganizer', Object>>
   packageOrganizer), RBRenameMethodRefactoring does not exist (the class is
   ReRenameMethodRefactoring; RBBrowserEnvironment and RBNamespace still
   exist), TestRunner does not exist (NewTestRunner).  The text says Pharo 12
   in six places; it now says Pharo 14.  PosternHelp class>>expectedPharoVersion
   (read by testPharoVersionPin) is now 'Pharo-14.0.0', the start of
   SystemVersion current imageVersionString on Pharo 14.

3. PosternServerTest>>testDelegateNormalizesLfToCr evaluated
   '| x |' LF 'x := 42' LF 'x' : no period after 42.  Pharo reads that as
   'x := 42 x' (unary send of #x to 42), the evaluation ends with a
   doesNotUnderstand, and the printed stack has no '42'.  LF and CR are both
   white space to the Pharo scanner, so the test could not show the
   normalisation anyway.  The repair adds the period and a second check where
   an LF inside a string literal must reach the compiler as CR.

4. PosternDashboardTest>>testTwoDashboardsOnSameServerShareTheLogger and
   testWindowClosedReleasesViewResources skipped when PosternDashboard
   findServer found no server with a PosternDelegate (no test starts one).
   Each test now starts its own server with PosternServer startOn: (ports 8501
   and 8502, loopback) and stops the server at the end.  The assertions are
   the same.

5. PosternSyncReferenceCommitCommandTest>>testExecuteCoreSetsReferenceCommit
   looked for an Iceberg repository named 'claude-agent-sdk-smalltalk', the
   private repository of the author, and skipped without text when absent.  The
   test now uses the repository of Postern itself, registered by the load as
   'postern'."

Smalltalk globals at: #PosternPatchBlock put: [ :clsName :sel :pairs | | cls |
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifTrue: [
			fix value: clsName value: sel
				value: [ :source | pairs anySatisfy: [ :p | source includesSubstring: p key ] ]
				value: (pairs inject: (cls >> sel) sourceCode into: [ :s :p | s copyReplaceAll: p key with: p value ]) ]
		ifFalse: [ fix value: clsName value: sel value: [ :s | false ] value: '' ] ].

"1. parser helpers of the help tests"
(Smalltalk globals at: #PosternPatchBlock) value: #PosternHelpTest value: #canParseExpression:
	value: { 'on: CodeError' -> 'on: OCCodeError'. 'RBParser' -> 'OCParser' }.
(Smalltalk globals at: #PosternPatchBlock) value: #PosternHelpTest value: #canParseMethod:
	value: { 'on: CodeError' -> 'on: OCCodeError'. 'RBParser' -> 'OCParser' }.

"2. help content for Pharo 14, and the version pin"
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #api
	value: { 'RPackageOrganizer default' -> 'self packageOrganizer'. 'Pharo 12' -> 'Pharo 14' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #browse
	value: { 'RPackageOrganizer default' -> 'self packageOrganizer'. 'Pharo 12' -> 'Pharo 14' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #git
	value: { 'RPackageOrganizer default' -> 'self packageOrganizer'.
		'repo workingCopy changes.' -> 'repo workingCopy diffToReferenceCommit.'. 'Pharo 12' -> 'Pharo 14' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #lessons
	value: { 'RPackageOrganizer default' -> 'self packageOrganizer'.
		'RBRenameMethodRefactoring' -> 'ReRenameMethodRefactoring'. 'Pharo 12' -> 'Pharo 14' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #pharo
	value: { 'Pharo 12' -> 'Pharo 14' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #tableOfContents
	value: { 'Pharo 12' -> 'Pharo 14' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #testing
	value: { 'TestRunner runAll' -> 'NewTestRunner new runAll' }.
(Smalltalk globals at: #PosternPatchBlock) value: #'PosternHelp class' value: #expectedPharoVersion
	value: { 'Pharo-12.0.0' -> 'Pharo-14.0.0' }.

"3. the LF to CR test"
fix value: #PosternServerTest value: #testDelegateNormalizesLfToCr
	value: [ :source | source includesSubstring: '42'' , String lf' ]
	value:
'testDelegateNormalizesLfToCr
	| delegate input result literalInput literalResult |
	delegate := PosternDelegate new.
	input := ''| x |'' , String lf , ''x := 42.'' , String lf , ''x''.
	result := delegate evaluate: input.
	self assert: (result includesSubstring: ''42'').
	"an LF inside a string literal must reach the compiler as a CR"
	literalInput := (String with: $'') , ''a'' , String lf , ''b'' , (String with: $'') , '' includes: Character cr''.
	literalResult := delegate evaluate: literalInput.
	self assert: (literalResult includesSubstring: ''true'')'.

"4. the two dashboard tests start their own server"
fix value: #PosternDashboardTest value: #testTwoDashboardsOnSameServerShareTheLogger
	value: [ :source | source includesSubstring: 'findServer' ]
	value:
'testTwoDashboardsOnSameServerShareTheLogger
	"Round 7 - System Browser pattern. Multiple dashboards on the same
	eval server share the underlying logger. Closing one window
	unsubscribes only that view; the logger keeps running and the
	other window keeps observing it. The load-bearing invariant is
	identity: m1 logger == m2 logger when both wrap the same server.
	The test starts its own eval server on loopback."

	| server m1 m2 d1 d2 sharedLogger |
	server := PosternServer startOn: 8501.
	[
		m1 := PosternDashboardModel on: server.
		m2 := PosternDashboardModel on: server.
		self
			assert: m1 logger == m2 logger
			description: ''two models on the same server must share one logger''.
		d1 := PosternDashboard openOn: m1.
		d2 := PosternDashboard openOn: m2.
		[
			sharedLogger := m1 logger.
			self assert: sharedLogger isRunning.
			d1 windowClosed.
			Smalltalk garbageCollect.
			self
				assert: sharedLogger isRunning
				description: ''shared logger must survive d1 close''.
			self
				assert: m2 logger == sharedLogger
				description: ''d2 still references the same logger''
		] ensure: [
			d1 ifNotNil: [ d1 window ifNotNil: [ :w | w isClosed ifFalse: [ w close ] ] ].
			d2 ifNotNil: [ d2 window ifNotNil: [ :w | w isClosed ifFalse: [ w close ] ] ] ]
	] ensure: [ server stop ]'.

fix value: #PosternDashboardTest value: #testWindowClosedReleasesViewResources
	value: [ :source | source includesSubstring: 'findServer' ]
	value:
'testWindowClosedReleasesViewResources
	"Round 7 - windowClosed cleans up THIS view resources only:
	the refresh process this view forked, and this view subscription
	on the shared logger announcer. The shared logger itself is owned
	by the server delegate and keeps running.
	The test starts its own eval server on loopback."

	| server model live |
	server := PosternServer startOn: 8502.
	[
		model := PosternDashboardModel on: server.
		live := PosternDashboard openOn: model.
		[
			self
				assert: live isRefreshProcessAlive
				description: ''view must have a running refresh process''.
			live windowClosed.
			Smalltalk garbageCollect.
			self
				deny: live isRefreshProcessAlive
				description: ''view refresh process must be terminated''.
			self
				assert: model logger isRunning
				description: ''shared logger must survive - owned by the server''
		] ensure: [
			live ifNotNil: [ live window ifNotNil: [ :w | w isClosed ifFalse: [ w close ] ] ] ]
	] ensure: [ server stop ]'.

"5. the sync command test uses the Postern repository"
(Smalltalk globals at: #PosternPatchBlock) value: #PosternSyncReferenceCommitCommandTest value: #testExecuteCoreSetsReferenceCommit
	value: { 'claude-agent-sdk-smalltalk' -> 'postern' }.

Smalltalk globals removeKey: #PosternPatchBlock ifAbsent: [ ].
