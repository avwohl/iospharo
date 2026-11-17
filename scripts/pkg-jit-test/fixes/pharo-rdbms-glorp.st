"Glorp (pharo-rdbms/glorp, master 520f5db): the baseline loads the package
Glorp-Pharo10 for pharo10.x to pharo13.x only.  The package has one method,
String>>convertToEncoding:, that Dialect class>>convert:toEncoding: calls.  On
Pharo 14 the method is absent, and GlorpDatabasePlatformTest>>testTrimString1
and testTrimString2 stop with MessageNotUnderstood on stock Cog and on our VM.

The repair in the package is one more platform name in BaselineOfGlorp
(#'pharo14.x').  Here the method of the package is added."

add value: #String value: #convertToEncoding: value: '*Glorp-Pharo10'
	value:
'convertToEncoding: encodingName

	^ self encodeWith: encodingName'.
