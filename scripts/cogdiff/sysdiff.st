"sysdiff.st -- clock, time zone and OS queries for a diff against Cog.  2026-10-17: all
agree but the architecture, attribute 1005 (window system) and the
command-line arguments (attributes 2, 3), which our eval harness does not pass."

| r |
r := OrderedCollection new.

r add: #offset -> ([ DateAndTime now offset printString ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #localTZ -> ([ DateAndTime localTimeZone printString ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #utcVsLocalHours -> ([ ((Time microsecondClockValue - Time primUTCMicrosecondsClock) / 3600000000.0) rounded ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #clockMonotonic -> ([ (Time millisecondClockValue <= Time millisecondClockValue) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #utcPlausible -> ([ ((Time primUTCMicrosecondsClock // 1000000 // 86400) between: 45000 and: 50000) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #platform -> ([ Smalltalk os platformName ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #subtype -> ([ Smalltalk os subtype ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #osVersionClass -> ([ Smalltalk os version class name ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #isMac -> ([ Smalltalk os isMacOS ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #attrs -> ([ ((#(1001 1002 1003 1004 1005 1006 1007 1008 1009 0 1 2 3 4 5 6) collect: [:i | i -> ([Smalltalk vm getSystemAttribute: i] on: Error do: [:e | #err]) ]) collect: [:a | a key -> (a value ifNil: ['nil'] ifNotNil: [:v | v isString ifTrue: [ v size > 0 ] ifFalse: [ v ]]) ]) asArray ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #envHome -> ([ ((Smalltalk os environment at: 'HOME' ifAbsent: [nil]) notNil) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #envMissing -> ([ (Smalltalk os environment at: 'NO_SUCH_VAR_XYZ' ifAbsent: [#absent]) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #imageDirExists -> ([ FileLocator imageDirectory exists ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #vmDirExists -> ([ FileLocator vmDirectory exists ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #workingDir -> ([ (FileSystem workingDirectory exists) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #wordSize -> ([ Smalltalk wordSize ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #endianness -> ([ Smalltalk vm isLittleEndian ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #maxIdentityHash -> ([ Smalltalk vm maxIdentityHash ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #processorCount -> ([ ([ Smalltalk vm parameterAt: 1000 ] on: Error do: [:e | #err ]) class name ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #interpreterClass -> ([ ([ Smalltalk vm interpreterClass ] on: Error do: [:e | #err]) class name ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #canHandleTimezone -> ([ DateAndTime now asUTC offset printString ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #dateNowSane -> ([ (Date today year between: 2025 and: 2035) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #randomSeedPrim -> ([ ([ (Random new next) between: 0 and: 1 ] on: Error do: [:e | #err ]) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #primRandom -> ([ ([ Random seed: 42; yourself. (Random seed: 42) next ] on: Error do: [:e | #err ]) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #sharedRandom -> ([ ((SharedRandom globalGenerator next) between: 0 and: 1) ] on: Error do: [:e | 'ERR ', e class name ]).
r add: #highResClock -> ([ (Time highResClock > 0) ] on: Error do: [:e | 'ERR ', e class name ]).
r asArray
