"Launchpad (ba-st/Launchpad, release-candidate), test helper of Buoy:
Buoy-Metaprogramming-Pharo-Extensions defines OSPlatform>>environmentAt:put:,
environmentAt:ifPresent:ifAbsent: and removeEnvironmentKey: with 'self
environment'.  OSPlatform>>environment is deprecated in Pharo 14 (the method is
in Deprecated14) and its message says 'Use OSEnvironment current instead'.
LaunchpadRootCommandTest>>set:to:during: and ConfigurationFromEnvironmentProviderTest
call these three methods, so a Deprecation reaches the test (seen:
testHandlingStartGreeterApplicationWithMixedConfiguration).  The three methods
answer through OSEnvironment current, which is what 'environment' answers.
The same change belongs in Buoy (see ba-st-launchpad.md)."

fix value: #OSPlatform value: #environmentAt:ifPresent:ifAbsent:
	value: [ :source | source includesSubstring: 'self environment' ]
	value:
'environmentAt: aVariableName ifPresent: aBlock ifAbsent: anAbsentBlock

	^ OSEnvironment current
		  at: aVariableName
		  ifPresent: aBlock
		  ifAbsent: anAbsentBlock'.

fix value: #OSPlatform value: #environmentAt:put:
	value: [ :source | source includesSubstring: 'self environment' ]
	value:
'environmentAt: variableName put: value

	OSEnvironment current at: variableName put: value'.

fix value: #OSPlatform value: #removeEnvironmentKey:
	value: [ :source | source includesSubstring: 'self environment' ]
	value:
'removeEnvironmentKey: variableName

	OSEnvironment current removeKey: variableName'.
