"ba-st-sagan (ba-st/Sagan, Sagan-RDBMS-Tests): three kinds of entries.

1. The RDBMS tests use PostgreSQL.  The test class
   SingleSessionRDMBSRepositoryTest chooses its database login when the class
   initializes: the environment variable RDBMS names the database, and without
   the variable the login is PostgreSQL on 127.0.0.1:5432 (user postgres,
   password secret, database test, SSL).  The machine has no PostgreSQL, so the
   login of each test fails with ConnectionClosed.  The package has a login for
   SQLite3 in the same class (sqlite3Login), and the CI of the package runs the
   same tests with RDBMS=SQLite3 (.github/workflows/SQLite3-Integration-Tests.yml).
   The entries make SQLite3 the default login, which is what the CI does with
   the variable.  A variable RDBMS that is set still wins.  The login is built
   at each call, because the SQLite login names the directory of the running
   image, and the image that runs the tests is a copy of the image that was
   prepared.  The last statement runs the class initialization again, so that
   the Glorp default driver of the saved image is SQLite3Driver (the test
   RDBMSSessionProviderTest>>expectedFailingLoginMessage reads that driver
   before it builds any login).

   The long waits of the failed logins are not a defect.  RDBMSSessionProvider
   retries a failed login 3 times with an exponential backoff with a random
   factor (Retry>>backoffExponentiallyWithTimeSlot:), a time slot of 3 seconds,
   and that gives 0 to 33 seconds of wait.  The time limit of a test is 10
   seconds.  So a test ends as ERROR when the random wait is short and as
   TIMEOUT when the wait is long, and the draw differs on each VM and run.
   With a database that accepts the login there is no wait.

2. The text binding of Pharo-SQLite3 (the dependency that the SQLite login
   uses).  SQLite3Library>>with:at:putText: passes the UTF-8 ByteArray to
   sqlite3_bind_text with the destructor argument 0 (SQLITE_STATIC).  SQLite
   then keeps the pointer and reads the bytes at sqlite3_step, but nothing
   keeps the ByteArray alive after the bind call, and a collection can move or
   reuse the memory before the step.  The repair passes SQLITE_TRANSIENT (-1),
   so SQLite copies the bytes in the call; the destructor argument is a 64-bit
   integer, so that -1 fills the register.  The same entries are in the file
   of the row rko281-restoreforpharo (the same defect in another fork).  The
   coordinator can move these two entries to scripts/image-fixes/."

fix value: #'SingleSessionRDMBSRepositoryTest class' value: #defaultLogin
	value: [ :source | source includesSubstring: 'postgreSQLLogin' ]
	value:
'defaultLogin

	"SQLite3 in a file of the directory of the image, as the CI of the package does with RDBMS=SQLite3.  No PostgreSQL server runs on this machine."

	<ignoreForCoverage>
	^ self sqlite3Login'.

fix value: #'SingleSessionRDMBSRepositoryTest class' value: #login
	value: [ :source | source includesSubstring: '^ DefaultLogin' ]
	value:
'login

	"Built at each call: the SQLite login names the directory of the running image, and the running image is not the prepared image."

	^ OSEnvironment current
		at: ''RDBMS''
		ifPresent: [ :variable | self loginFor: variable ]
		ifAbsent: [ self defaultLogin ]'.

(Smalltalk globals includesKey: #SingleSessionRDMBSRepositoryTest)
	ifFalse: [ Stdio stdout nextPutAll: 'SKIP SingleSessionRDMBSRepositoryTest class>>initialize (class absent)'; lf; flush ]
	ifTrue: [
		(Smalltalk globals includesKey: #SQLite3Driver)
			ifFalse: [ Stdio stdout nextPutAll: 'SKIP SingleSessionRDMBSRepositoryTest class>>initialize (SQLite3Driver absent)'; lf; flush ]
			ifTrue: [
				(Smalltalk globals at: #SingleSessionRDMBSRepositoryTest) initialize.
				Stdio stdout nextPutAll: 'RAN SingleSessionRDMBSRepositoryTest class>>initialize (SQLite3 login)'; lf; flush ] ].

fix value: #SQLite3Library value: #with:at:putText:
	value: [ :source | source includesSubstring: 'with: 0' ]
	value:
'with: aStatement at: aColumn putText: aString

	| bytes |

	bytes := self pharoStringToUTF8: aString.

	"SQLITE_TRANSIENT (-1): SQLite copies the bytes in this call.  With 0
	(SQLITE_STATIC) SQLite reads the bytes at sqlite3_step, when the garbage
	collector can have moved or reused the memory of the temporary ByteArray."
	^ self
		  apiBindText: aStatement
		  atColumn: aColumn
		  with: bytes
		  with: bytes size
		  with: -1'.

fix value: #SQLite3Library value: #apiBindText:atColumn:with:with:with:
	value: [ :source | source includesSubstring: 'int anotherInteger' ]
	value:
'apiBindText: aStatement atColumn: aColumn with: aString with: anInteger with: anotherInteger
	"int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*))
	The last argument is the destructor.  It is a pointer, so it is a 64-bit integer here (SQLITE_TRANSIENT is -1)."

	^ self ffiCall: #(int sqlite3_bind_text (sqlite3_stmt* aStatement, int aColumn, String aString, int anInteger, sqlite3_int64 anotherInteger))'.
