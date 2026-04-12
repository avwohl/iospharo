| result |
result := OpalCompiler new evaluate: '3 + 4'.
Stdio stdout nextPutAll: result printString; lf; flush.
Smalltalk exitSuccess.
