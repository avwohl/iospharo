"Evref-BL/Github-Pharo-API: GithubUsersTest>>testGet, testGetUnknowUsers and
testGetUserId were copied from the tests of Evref-BL/Gitlab-Pharo-API and
still build their API object with 'GitlabApi new'.  The Github baseline does
not load GitlabApi (and GitlabApi has no #users that fits).  The class that
the Github package has is GithubApi (users answers GithubUsers).  The sibling
tests (GithubOrganizationsTest and others) use GithubApi with the same
stubbed ZnClient, and they pass.  testGetUsername only sends testGet, so
the same repair fixes that test.  The repair replaces the class name in each
of the three test methods.  No request leaves the image: ZnClient>>get is
stubbed by Mocketry in each test."

Smalltalk globals at: #PkgFixRewrite put: [ :clsName :sel :oldText :newText | | cls |
	cls := resolve value: clsName.
	(cls notNil and: [ cls includesSelector: sel ])
		ifTrue: [
			fix value: clsName value: sel
				value: [ :source | source includesSubstring: oldText ]
				value: ((cls >> sel) sourceCode copyReplaceAll: oldText with: newText) ]
		ifFalse: [ fix value: clsName value: sel value: [ :source | false ] value: '' ] ].

#(testGet testGetUnknowUsers testGetUserId) do: [ :selector |
	(Smalltalk globals at: #PkgFixRewrite) value: #GithubUsersTest value: selector value: 'GitlabApi' value: 'GithubApi' ].
Smalltalk globals removeKey: #PkgFixRewrite ifAbsent: [ nil ].
