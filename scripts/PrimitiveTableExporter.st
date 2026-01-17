"PrimitiveTableExporter - Export primitive table metadata as JSON and C++

 Load this file in VMMaker image before generating cointerp.c:
   'scripts/PrimitiveTableExporter.st' asFileReference fileIn.

 Then call:
   PrimitiveTableExporter exportTo: '/path/to/primitives.json'.
   PrimitiveTableExporter exportCppTo: '/path/to/generated_primitives.inc'.
"

Object subclass: #PrimitiveTableExporter
	instanceVariableNames: ''
	classVariableNames: ''
	package: 'VMMaker-iOS'.

PrimitiveTableExporter class >> exportTo: aFilename [
	"Export the primitive table with metadata to JSON.
	 PrimitiveTableExporter exportTo: '/tmp/primitives.json'
	"
	| entries |
	entries := self primitiveTableEntries.

	aFilename asFileReference writeStreamDo: [ :stream |
		stream nextPutAll: '{"generated": "'; nextPutAll: DateAndTime now printString; nextPutAll: '",'; cr.
		stream nextPutAll: ' "source": "VMMaker StackInterpreter initializePrimitiveTable",'; cr.
		stream nextPutAll: ' "primitives": ['; cr.

		entries withIndexDo: [ :entry :index |
			stream nextPutAll: '  '.
			self writeEntry: entry on: stream.
			index < entries size ifTrue: [ stream nextPut: $, ].
			stream cr ].

		stream nextPutAll: ']}'; cr ].

	Transcript show: 'Exported ', entries size printString, ' primitives to ', aFilename; cr.
]

PrimitiveTableExporter class >> exportCppTo: aFilename [
	"Export the primitive table as C++ code for inclusion in Interpreter.cpp
	 PrimitiveTableExporter exportCppTo: '/tmp/generated_primitives.inc'
	"
	| entries implementedCount |
	entries := self primitiveTableEntries.
	implementedCount := 0.

	aFilename asFileReference writeStreamDo: [ :stream |
		stream nextPutAll: '// Generated primitive table - DO NOT EDIT'; cr.
		stream nextPutAll: '// Source: VMMaker StackInterpreter initializePrimitiveTable'; cr.
		stream nextPutAll: '// Generated: '; nextPutAll: DateAndTime now printString; cr.
		stream cr.
		stream nextPutAll: '// Include this file in Interpreter::initializePrimitiveTable()'; cr.
		stream nextPutAll: '// Usage: #include "generated_primitives.inc"'; cr.
		stream cr.

		entries do: [ :entry |
			| num name status cppName |
			num := entry at: #num.
			name := entry at: #name.
			status := entry at: #status.

			"Convert Smalltalk name to C++ method name"
			cppName := self cppNameFor: name status: status.

			stream nextPutAll: 'primitiveTable_['.
			stream print: num.
			stream nextPutAll: '] = '.
			stream nextPutAll: cppName.
			stream nextPutAll: ';'.

			"Add comment with category and original name"
			stream nextPutAll: '  // '.
			stream nextPutAll: (entry at: #category).
			status = 'implemented' ifTrue: [
				implementedCount := implementedCount + 1 ].
			stream cr ].

		stream cr.
		stream nextPutAll: '// Total: '; print: entries size; nextPutAll: ' entries'; cr.
		stream nextPutAll: '// Implemented: '; print: implementedCount; cr ].

	Transcript show: 'Exported C++ primitive table to ', aFilename; cr.
	Transcript show: '  ', entries size printString, ' total, ', implementedCount printString, ' implemented'; cr.
]

PrimitiveTableExporter class >> cppNameFor: primitiveName status: status [
	"Convert a Smalltalk primitive name to C++ method pointer syntax"

	"Unimplemented or quick primitives"
	(status = 'unimplemented' or: [ status = 'quickPrimitive' ])
		ifTrue: [ ^ 'nullptr' ].

	"Actual primitive - prefix with &Interpreter::"
	^ '&Interpreter::', primitiveName
]

PrimitiveTableExporter class >> writeEntry: entry on: stream [
	"Write a single primitive entry as JSON object"
	stream
		nextPutAll: '{"num": '; print: (entry at: #num);
		nextPutAll: ', "name": "'; nextPutAll: (entry at: #name);
		nextPutAll: '", "category": "'; nextPutAll: (entry at: #category);
		nextPutAll: '", "status": "'; nextPutAll: (entry at: #status);
		nextPutAll: '"}'.
]

PrimitiveTableExporter class >> primitiveTableEntries [
	"Extract primitive table entries with metadata.
	 Returns array of dictionaries with keys: num, name, category, status"
	| entries category rawTable |
	entries := OrderedCollection new.
	category := 'Unknown'.

	"Get the raw primitive table specification from StackInterpreter"
	rawTable := self primitiveTableSpec.

	rawTable do: [ :item |
		item isString ifTrue: [
			"It's a category comment"
			category := item ]
		ifFalse: [
			"It's a primitive spec: (num name) or (startNum endNum name)"
			item isArray ifTrue: [
				self processSpec: item category: category into: entries ]]].

	^ entries asArray
]

PrimitiveTableExporter class >> processSpec: spec category: category into: entries [
	"Process a primitive spec like (1 primitiveAdd) or (190 194 primitiveFail)"
	| startNum endNum primName status |

	spec size = 2 ifTrue: [
		"Simple spec: (num name)"
		startNum := spec first.
		endNum := startNum.
		primName := spec second ].

	spec size = 3 ifTrue: [
		"Range spec: (startNum endNum name)"
		startNum := spec first.
		endNum := spec second.
		primName := spec third ].

	spec size < 2 ifTrue: [ ^ self ]. "Skip malformed specs"

	"Determine status"
	status := (primName isNil or: [ primName = 0 or: [ primName == #primitiveFail ]])
		ifTrue: [ 'unimplemented' ]
		ifFalse: [ primName isInteger
			ifTrue: [ 'quickPrimitive' ]
			ifFalse: [ 'implemented' ]].

	"Add entry for each number in range"
	startNum to: endNum do: [ :num |
		entries add: (Dictionary new
			at: #num put: num;
			at: #name put: (primName ifNil: ['nil'] ifNotNil: [ primName asString ]);
			at: #category put: category;
			at: #status put: status;
			yourself) ].
]

PrimitiveTableExporter class >> primitiveTableSpec [
	"Return the raw primitive table specification.
	 This is the literal array from StackInterpreter class >> initializePrimitiveTable"

	^ #(
		"Integer Primitives (0-19)"
		(0 0)
		(1 primitiveAdd)
		(2 primitiveSubtract)
		(3 primitiveLessThan)
		(4 primitiveGreaterThan)
		(5 primitiveLessOrEqual)
		(6 primitiveGreaterOrEqual)
		(7 primitiveEqual)
		(8 primitiveNotEqual)
		(9 primitiveMultiply)
		(10 primitiveDivide)
		(11 primitiveMod)
		(12 primitiveDiv)
		(13 primitiveQuo)
		(14 primitiveBitAnd)
		(15 primitiveBitOr)
		(16 primitiveBitXor)
		(17 primitiveBitShift)
		(18 primitiveMakePoint)
		(19 primitiveFail)

		"LargeInteger Primitives (20-37)"
		(20 primitiveRemLargeIntegers)
		(21 primitiveAddLargeIntegers)
		(22 primitiveSubtractLargeIntegers)
		(23 primitiveLessThanLargeIntegers)
		(24 primitiveGreaterThanLargeIntegers)
		(25 primitiveLessOrEqualLargeIntegers)
		(26 primitiveGreaterOrEqualLargeIntegers)
		(27 primitiveEqualLargeIntegers)
		(28 primitiveNotEqualLargeIntegers)
		(29 primitiveMultiplyLargeIntegers)
		(30 primitiveDivideLargeIntegers)
		(31 primitiveModLargeIntegers)
		(32 primitiveDivLargeIntegers)
		(33 primitiveQuoLargeIntegers)
		(34 primitiveBitAndLargeIntegers)
		(35 primitiveBitOrLargeIntegers)
		(36 primitiveBitXorLargeIntegers)
		(37 primitiveBitShiftLargeIntegers)

		"Float Primitives (38-59)"
		(38 primitiveFloatAt)
		(39 primitiveFloatAtPut)
		(40 primitiveAsFloat)
		(41 primitiveFloatAdd)
		(42 primitiveFloatSubtract)
		(43 primitiveFloatLessThan)
		(44 primitiveFloatGreaterThan)
		(45 primitiveFloatLessOrEqual)
		(46 primitiveFloatGreaterOrEqual)
		(47 primitiveFloatEqual)
		(48 primitiveFloatNotEqual)
		(49 primitiveFloatMultiply)
		(50 primitiveFloatDivide)
		(51 primitiveTruncated)
		(52 primitiveFractionalPart)
		(53 primitiveExponent)
		(54 primitiveTimesTwoPower)
		(55 primitiveSquareRoot)
		(56 primitiveSine)
		(57 primitiveArctan)
		(58 primitiveLogN)
		(59 primitiveExp)

		"Subscript Primitives (60-67)"
		(60 primitiveAt)
		(61 primitiveAtPut)
		(62 primitiveSize)
		(63 primitiveStringAt)
		(64 primitiveStringAtPut)
		(65 67 primitiveFail)

		"StorageManagement Primitives (68-79)"
		(68 primitiveObjectAt)
		(69 primitiveObjectAtPut)
		(70 primitiveNew)
		(71 primitiveNewWithArg)
		(72 primitiveArrayBecomeOneWay)
		(73 primitiveInstVarAt)
		(74 primitiveInstVarAtPut)
		(75 primitiveIdentityHash)
		(76 primitiveStoreStackp)
		(77 primitiveSomeInstance)
		(78 primitiveNextInstance)
		(79 primitiveNewMethod)

		"Control Primitives (80-89)"
		(80 82 primitiveFail)
		(83 primitivePerform)
		(84 primitivePerformWithArgs)
		(85 primitiveSignal)
		(86 primitiveWait)
		(87 primitiveResume)
		(88 primitiveSuspend)
		(89 primitiveFlushCache)

		"PermSpace Primitives (90-93)"
		(90 primitiveMoveToPermSpace)
		(91 primitiveMoveToPermSpaceInBulk)
		(92 primitiveIsInPermSpace)
		(93 primitiveMoveToPermSpaceAllOldObjects)

		"Input/Output Primitives (94-109)"
		(94 96 primitiveFail)
		(97 primitiveSnapshot)
		(98 99 primitiveFail)
		(100 primitivePerformInSuperclass)
		(101 104 primitiveFail)
		(105 primitiveStringReplace)
		(106 109 primitiveFail)

		"System Primitives (110-119)"
		(110 primitiveIdentical)
		(111 primitiveClass)
		(112 primitiveBytesLeft)
		(113 primitiveQuit)
		(114 primitiveFail)
		(115 primitiveChangeClass)
		(116 primitiveFlushCacheByMethod)
		(117 primitiveExternalCall)
		(118 primitiveDoPrimitiveWithArgs)
		(119 primitiveFlushCacheBySelector)

		"Miscellaneous Primitives (120-127)"
		(120 primitiveCalloutToFFI)
		(121 primitiveImageName)
		(122 123 primitiveFail)
		(124 primitiveLowSpaceSemaphore)
		(125 primitiveSignalAtBytesLeft)
		(126 127 primitiveFail)

		"Squeak Miscellaneous Primitives (128-149)"
		(128 primitiveArrayBecome)
		(129 primitiveSpecialObjectsOop)
		(130 primitiveFullGC)
		(131 primitiveIncrementalGC)
		(132 primitiveObjectPointsTo)
		(133 134 primitiveFail)
		(135 primitiveMillisecondClock)
		(136 primitiveSignalAtMilliseconds)
		(137 primitiveSecondsClock)
		(138 primitiveSomeObject)
		(139 primitiveNextObject)
		(140 141 primitiveFail)
		(142 primitiveVMPath)
		(143 primitiveShortAt)
		(144 primitiveShortAtPut)
		(145 primitiveConstantFill)
		(146 147 primitiveFail)
		(148 primitiveClone)
		(149 primitiveGetAttribute)

		"File Primitives - Now Named (150-169)"
		(150 155 primitiveFail)
		(156 primitiveCompareBytes)
		(157 primitiveFail)
		(158 primitiveStringCompareWith)
		(159 primitiveHashMultiply)
		(160 primitiveAdoptInstance)
		(161 primitiveSetOrHasIdentityHash)
		(162 primitiveFail)
		(163 primitiveGetImmutability)
		(164 primitiveSetImmutability)
		(165 primitiveIntegerAt)
		(166 primitiveIntegerAtPut)
		(167 primitiveYield)
		(168 primitiveCopyObject)
		(169 primitiveNotIdentical)

		"SpurMemoryManager Primitives (170-184)"
		(170 primitiveAsCharacter)
		(171 primitiveImmediateAsInteger)
		(172 primitiveFetchNextMourner)
		(173 primitiveSlotAt)
		(174 primitiveSlotAtPut)
		(175 primitiveBehaviorHash)
		(176 primitiveMaxIdentityHash)
		(177 primitiveAllInstances)
		(178 primitiveAllObjects)
		(179 primitiveFail)
		(180 primitiveGrowMemoryByAtLeast)
		(181 primitiveSizeInBytesOfInstance)
		(182 primitiveSizeInBytes)
		(183 primitiveIsPinned)
		(184 primitivePin)

		"CriticalSection Primitives (185-187)"
		(185 primitiveExitCriticalSection)
		(186 primitiveEnterCriticalSection)
		(187 primitiveTestAndSetOwnershipOfCriticalSection)

		"ExecuteMethod Primitives (188-189)"
		(188 primitiveExecuteMethodArgsArray)
		(189 primitiveExecuteMethod)

		"Reserved (190-194)"
		(190 194 primitiveFail)

		"Unwind Primitives (195-199)"
		(195 primitiveFindNextUnwindContext)
		(196 primitiveTerminateTo)
		(197 primitiveFindHandlerContext)
		(198 199 primitiveFail)

		"Closure Primitives (200-212)"
		(200 206 primitiveFail)
		(207 primitiveFullClosureValue)
		(208 primitiveFullClosureValueWithArgs)
		(209 primitiveFullClosureValueNoContextSwitch)
		(210 primitiveContextAt)
		(211 primitiveContextAtPut)
		(212 primitiveContextSize)

		"Context/Method Introspection (213-219)"
		(213 primitiveContextXray)
		(214 primitiveVoidVMState)
		(215 primitiveVoidVMStateForMethod)
		(216 primitiveMethodXray)
		(217 primitiveMethodProfilingData)
		(218 primitiveDoNamedPrimitiveWithArgs)
		(219 primitiveFail)

		"Reserved for Cog (220-229)"
		(220 229 primitiveFail)

		"Process/Time Primitives (230-246)"
		(230 primitiveRelinquishProcessor)
		(231 primitiveFormat)
		(232 239 primitiveFail)
		(240 primitiveUTCMicrosecondClock)
		(241 primitiveLocalMicrosecondClock)
		(242 primitiveSignalAtUTCMicroseconds)
		(243 primitiveUpdateTimezone)
		(244 primitiveUtcAndTimezoneOffset)
		(245 primitiveCoarseUTCMicrosecondClock)
		(246 primitiveCoarseLocalMicrosecondClock)

		"Become Primitives (247-249)"
		(247 primitiveFail)
		(248 primitiveArrayBecomeOneWayNoCopyHash)
		(249 primitiveArrayBecomeOneWayCopyHash)

		"VM Profiling Primitives (250-255)"
		(250 primitiveClearVMProfile)
		(251 primitiveControlVMProfiling)
		(252 primitiveVMProfileSamplesInto)
		(253 primitiveCollectCogCodeConstituents)
		(254 primitiveVMParameter)
		(255 primitiveFail)

		"Quick Primitives (256-519) - Push constants and inst vars"
		(256 263 nil)
		(264 519 nil)

		"Reserved (520-540)"
		(520 540 primitiveFail)

		"SmallFloat Primitives (541-559)"
		(541 primitiveSmallFloatAdd)
		(542 primitiveSmallFloatSubtract)
		(543 primitiveSmallFloatLessThan)
		(544 primitiveSmallFloatGreaterThan)
		(545 primitiveSmallFloatLessOrEqual)
		(546 primitiveSmallFloatGreaterOrEqual)
		(547 primitiveSmallFloatEqual)
		(548 primitiveSmallFloatNotEqual)
		(549 primitiveSmallFloatMultiply)
		(550 primitiveSmallFloatDivide)
		(551 primitiveSmallFloatTruncated)
		(552 primitiveSmallFloatFractionalPart)
		(553 primitiveSmallFloatExponent)
		(554 primitiveSmallFloatTimesTwoPower)
		(555 primitiveSmallFloatSquareRoot)
		(556 primitiveSmallFloatSine)
		(557 primitiveSmallFloatArctan)
		(558 primitiveSmallFloatLogN)
		(559 primitiveSmallFloatExp)

		"Reserved (560-569)"
		(560 569 primitiveFail)

		"External Primitive Support (570-573)"
		(570 primitiveFlushExternalPrimitives)
		(571 primitiveUnloadModule)
		(572 primitiveListBuiltinModule)
		(573 primitiveListExternalModule)

		"SIMD Primitives (574)"
		(574 primitiveFloat64ArrayAdd)

		"Reserved (575-595)"
		(575 595 primitiveFail)

		"OldSpace Allocation (596-597)"
		(596 primitiveNewOldSpace)
		(597 primitiveNewWithArgOldSpace)

		"Pinned Allocation (598-599)"
		(598 primitiveNewPinned)
		(599 primitiveNewWithArgPinned)

		"FFI Byte Access (600-659)"
		(600 primitiveLoadBoolean8FromBytes)
		(601 primitiveLoadUInt8FromBytes)
		(602 primitiveLoadInt8FromBytes)
		(603 primitiveLoadUInt16FromBytes)
		(604 primitiveLoadInt16FromBytes)
		(605 primitiveLoadUInt32FromBytes)
		(606 primitiveLoadInt32FromBytes)
		(607 primitiveLoadUInt64FromBytes)
		(608 primitiveLoadInt64FromBytes)
		(609 primitiveLoadPointerFromBytes)
		(610 primitiveLoadChar8FromBytes)
		(611 primitiveLoadChar16FromBytes)
		(612 primitiveLoadChar32FromBytes)
		(613 primitiveLoadFloat32FromBytes)
		(614 primitiveLoadFloat64FromBytes)
		(615 primitiveStoreBoolean8IntoBytes)
		(616 primitiveStoreUInt8IntoBytes)
		(617 primitiveStoreInt8IntoBytes)
		(618 primitiveStoreUInt16IntoBytes)
		(619 primitiveStoreInt16IntoBytes)
		(620 primitiveStoreUInt32IntoBytes)
		(621 primitiveStoreInt32IntoBytes)
		(622 primitiveStoreUInt64IntoBytes)
		(623 primitiveStoreInt64IntoBytes)
		(624 primitiveStorePointerIntoBytes)
		(625 primitiveStoreChar8IntoBytes)
		(626 primitiveStoreChar16IntoBytes)
		(627 primitiveStoreChar32IntoBytes)
		(628 primitiveStoreFloat32IntoBytes)
		(629 primitiveStoreFloat64IntoBytes)
		(630 primitiveLoadBoolean8FromExternalAddress)
		(631 primitiveLoadUInt8FromExternalAddress)
		(632 primitiveLoadInt8FromExternalAddress)
		(633 primitiveLoadUInt16FromExternalAddress)
		(634 primitiveLoadInt16FromExternalAddress)
		(635 primitiveLoadUInt32FromExternalAddress)
		(636 primitiveLoadInt32FromExternalAddress)
		(637 primitiveLoadUInt64FromExternalAddress)
		(638 primitiveLoadInt64FromExternalAddress)
		(639 primitiveLoadPointerFromExternalAddress)
		(640 primitiveLoadChar8FromExternalAddress)
		(641 primitiveLoadChar16FromExternalAddress)
		(642 primitiveLoadChar32FromExternalAddress)
		(643 primitiveLoadFloat32FromExternalAddress)
		(644 primitiveLoadFloat64FromExternalAddress)
		(645 primitiveStoreBoolean8IntoExternalAddress)
		(646 primitiveStoreUInt8IntoExternalAddress)
		(647 primitiveStoreInt8IntoExternalAddress)
		(648 primitiveStoreUInt16IntoExternalAddress)
		(649 primitiveStoreInt16IntoExternalAddress)
		(650 primitiveStoreUInt32IntoExternalAddress)
		(651 primitiveStoreInt32IntoExternalAddress)
		(652 primitiveStoreUInt64IntoExternalAddress)
		(653 primitiveStoreInt64IntoExternalAddress)
		(654 primitiveStorePointerIntoExternalAddress)
		(655 primitiveStoreChar8IntoExternalAddress)
		(656 primitiveStoreChar16IntoExternalAddress)
		(657 primitiveStoreChar32IntoExternalAddress)
		(658 primitiveStoreFloat32IntoExternalAddress)
		(659 primitiveStoreFloat64IntoExternalAddress)

		"End"
		(660 primitiveFail)
	)
]
