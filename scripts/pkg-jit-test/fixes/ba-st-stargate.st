"Stargate (ba-st/Stargate, release-candidate), Pharo 14: PharoOperatingSystemInformationProvider
(package Stargate-Application-Info-Pharo) sends Smalltalk os three times and Smalltalk os
environment once.  Pharo 14 has both in the package Deprecated14: SmalltalkImage>>os
('Use OSPlatform current instead') and OSPlatform>>environment ('Use OSEnvironment current
instead').  Each call signals a Deprecation in the tests of Stargate-Application-Info-Tests
(ApplicationInfoRESTfulControllerTest, ApplicationInfoPluginAPITest and the tests that read the
info of the 'os' provider).  The Launchpad row (fixes/ba-st-launchpad.st) had the same kind of
deprecated call of OSPlatform>>environment reach a test.  The four methods answer through
OSPlatform current and OSEnvironment current, which is what the deprecated messages answer.
The same change belongs in Stargate."

fix value: #PharoOperatingSystemInformationProvider value: #environmentInfo
	value: [ :source | source includesSubstring: 'Smalltalk os' ]
	value:
'environmentInfo

	^ OSEnvironment current asDictionary'.

fix value: #PharoOperatingSystemInformationProvider value: #operatingSystemDetails
	value: [ :source | source includesSubstring: 'Smalltalk os' ]
	value:
'operatingSystemDetails

	| details |

	details := OrderedDictionary new.
	OSPlatform current isUnix then: [ self includeLinuxDetailsOn: details ].
	^ details'.

fix value: #PharoOperatingSystemInformationProvider value: #operatingSystemGeneralInfo
	value: [ :source | source includesSubstring: 'Smalltalk os' ]
	value:
'operatingSystemGeneralInfo

	^ OrderedDictionary new
		at: ''platform'' put: OSPlatform current platformName;
		at: ''version'' put: OSPlatform current version;
		at: ''subtype'' put: OSPlatform current subtype;
		yourself'.
