"Redeclare CXString as the 16-byte struct libclang really passes and returns by value:
   typedef struct { const void *data; unsigned private_flags; } CXString;
 The original declaration was a one-pointer FFIExternalObject, so clang_disposeString(self)
 passed only the data pointer in x0 and left x1 (private_flags) uninitialised."
(CXStructure << #CXString)
	package: 'CIG-LibClang';
	install.
CXString class compile: 'fieldsDesc
	^ #(
	void *data;
	uint private_flags;
	)' classified: 'field definition'.
CXString rebuildFieldAccessors.
CXString compile: 'isNull
	^ self data isNull' classified: 'testing'.
CXString compile: 'cString
	^ self clang_getCString utf8StringFromCString' classified: 'accessing'.
CXString compile: 'clang_disposeString
	self ffiCall: #(void clang_disposeString(CXString self))' classified: 'private'.
CXString compile: 'clang_getCString
	^ self ffiCall: #(void *clang_getCString(CXString self))' classified: 'private'.
CXString compile: 'dispose
	self clang_disposeString' classified: 'accessing'.
CXTranslationUnit compile: 'tokenSpelling: token
	| string |
	string := self clang_getTokenSpelling: token.
	string isNull ifTrue: [ ^ nil ].
	^ [ string cString ] ensure: [ string dispose ]' classified: 'accessing - tokens'.
