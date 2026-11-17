"moosetechnology/FAST-JAVA.

Group 1 (9 tests: 8 errors and 1 failure; 6 methods of FAST).  FAST (v3) sends #children to its
entities in FASTCoreVisitor>>visitFASTTEntity:.  Famix removed #children from
the entities in October 2025 (replacement: #containedEntities), and only the
package Famix-Deprecated still has a #children that forwards to
#containedEntities.  The baseline of FAST-Java loads the Famix groups Core and
Basic, and these groups do not have Famix-Deprecated.  Each visit of an entity
that FASTJavaVisitor does not visit with a method of its own (FASTJavaEmptyDim
Expression, FASTJavaVarDeclStatement, FASTJavaExpressionStatement,
FASTJavaAnnotation, and others) ends in 'FASTxxx does not understand #children'.
The repair sends #containedEntities, the name that the current Famix has, in
the 5 FAST methods that still send the old names (#children, #childrenSelectors,
#parentSelectors).  The first method is the one that the tests need; the other
four have the same defect and no test in this package runs them.

Group 2 (JavaSmaCCParamsMethodTest>>testBlockScope and >>testScope).  The two
tests call 'self skip' at their start, and the code that was to follow is a
comment that uses #fastScope, a method that no longer exists (the scope work
moved to FASTJavaLocalResolverVisitor).  The repair makes the two tests check
the same facts with the resolver: the parameters a, b and c of the method are
the declarations of their uses in the body, and the body block declares no
variable of its own (x is bound to a non-local declaration).

Group 3 (JavaSmaCCReturnAttributeAccessTest>>testReturnExpressionType).  The test
calls 'self skip' since 2023 and expects FASTJavaQualifiedName for the expression
'a.b' of 'return a.b'.  The importer (visitQualifiedName:) makes a
FASTJavaClassProperty (type a, fieldName b) for a qualified name inside a method
and a FASTJavaQualifiedName only outside a type declaration.  Tests of other
classes assert the same (JavaSmaCCForEachTest>>testForEachListType,
JavaSmaCCCClassPropertyTest).  The expectation of the test was wrong.  The
repair expects FASTJavaClassProperty and the field name 'b'."

fix value: #FASTCoreVisitor value: #visitFASTTEntity:
	value: [ :source | source includesSubstring: 'children' ]
	value:
'visitFASTTEntity: aFASTTEntity
	^ aFASTTEntity containedEntities do: [ :child | child accept: self ]'.

fix value: #FASTTEntity value: #orderedChildren
	value: [ :source | source includesSubstring: 'self children' ]
	value:
'orderedChildren
	^ self containedEntities asOrderedCollection sort: #startPos ascending'.

fix value: #FASTTEntity value: #removeAllPotentialChildrenNode
	value: [ :source | source includesSubstring: 'childrenSelectors' ]
	value:
'removeAllPotentialChildrenNode
	^ self containedEntitySelectors do: [ :selector | self perform: selector , '':'' withArguments: { nil } ]'.

fix value: #FASTTEntity value: #removeAllPotentialParentNode
	value: [ :source | source includesSubstring: 'parentSelectors' ]
	value:
'removeAllPotentialParentNode
	^ self containerSelectors do: [ :selector | self perform: selector , '':'' withArguments: { nil } ]'.

fix value: #FASTTEntity value: #currentParentSelector
	value: [ :source | source includesSubstring: 'parentSelectors' ]
	value:
'currentParentSelector
	self containerSelectors do: [ :accessor | (self perform: accessor) ifNotNil: [ :r | ^ accessor ] ].
	^ nil'.

fix value: #FASTTWithComments value: #numberOfComments
	value: [ :source | source includesSubstring: 'self children' ]
	value:
'numberOfComments
	<FMProperty: #numberOfComments type: #Number>
	<derived>
	<FMComment: ''The number of comments in a class''>
	^ self
		lookUpPropertyNamed: #numberOfComments
		computedAs: [ self comments size
				+
					(self containedEntities asArray
						inject: 0
						into: [ :sum :el | el numberOfComments + sum ]) ]'.

fix value: #JavaSmaCCParamsMethodTest value: #testBlockScope
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testBlockScope
	"the body block declares no variable of its own: the use of x is bound to a non-local declaration"

	| use |
	FASTJavaLocalResolverVisitor new on: fastMethod.
	use := (fastModel allWithType: FASTJavaVariableExpression) asOrderedCollection
		detect: [ :each | each name = ''x'' ].
	self assert: use localDeclaration isNonLocalDeclaration'.

fix value: #JavaSmaCCParamsMethodTest value: #testScope
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testScope
	"the parameters a, b and c are declared in the scope of the method: each use in the body is bound to its parameter"

	| parameterVariables uses |
	FASTJavaLocalResolverVisitor new on: fastMethod.
	parameterVariables := fastMethod parameters collect: #variable.
	uses := (fastModel allWithType: FASTJavaVariableExpression) asOrderedCollection
		reject: [ :each | parameterVariables includes: each ].
	#(''a'' ''b'' ''c'') do: [ :name |
		| use |
		use := uses detect: [ :each | each name = name ].
		self deny: use localDeclaration isNonLocalDeclaration.
		self assert: use localDeclaration variable name equals: name.
		self
			assert: use localDeclaration
			equals: (fastMethod parameters detect: [ :each | each variable name = name ]) ]'.

fix value: #JavaSmaCCReturnAttributeAccessTest value: #testReturnExpressionType
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testReturnExpressionType
	| expression |
	expression := (fastModel allWithType: FASTJavaReturnStatement) anyOne expression.
	self assert: expression class equals: FASTJavaClassProperty.
	self assert: expression fieldName equals: ''b'''.
