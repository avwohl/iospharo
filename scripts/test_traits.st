| classes results stream |
"Run Trait test classes individually and capture results with error details."
results := OrderedCollection new.

classes := #(
  TraitPureBehaviorTest TraitTest TraitCompositionTest ClassTraitTest
  MOPTraitTest TraitPrecedenceCompositionTest TraitWithAliasTest
  TraitWithConflictsTest TraitChangesTest TraitMethodDescriptionTest
  TraitOverloadingOfMethodsInTraitedClassTest TraitPackagingTest
  TraitInTraitClassTest TraitWithMethodsInProtocolsTest
  TraitWithSlotsTest TraitWithComplexSlotsTest TraitSlotScopeTest
  TraitFluidClassDefinitionPrinterTest TraitLegacyPharoClassDefinitionPrinterTest
  TraitOldPharoClassDefinitionPrinterTest SlotTraitsTest
).

classes do: [:name |
  | cls suite result |
  cls := Smalltalk globals at: name ifAbsent: [ nil ].
  cls ifNil: [
    results add: name , ': CLASS NOT FOUND'.
  ] ifNotNil: [
    [ suite := cls buildSuite.
      result := suite run.
      results add: name , ': P=' , result passedCount printString
        , ' F=' , result failureCount printString
        , ' E=' , result errorCount printString.
      result errors do: [:each |
        results add: '  ERROR: ' , each printString.
        [ results add: '    ' , each exception messageText ]
          on: Error do: [:e | results add: '    (no message)' ].
      ].
      result failures do: [:each |
        results add: '  FAIL: ' , each printString.
        [ results add: '    ' , each exception messageText ]
          on: Error do: [:e | results add: '    (no message)' ].
      ].
    ] on: Error do: [:e |
      results add: name , ': SETUP ERROR: ' , e messageText.
    ].
  ].
].

"Write results"
stream := (FileSystem workingDirectory / 'trait_test_results.txt') writeStream.
results do: [:line | stream nextPutAll: line; cr ].
stream close.

Stdio stderr nextPutAll: '=== TRAIT TESTS DONE ==='; cr.
Stdio stderr nextPutAll: results size printString , ' result lines'; cr.
