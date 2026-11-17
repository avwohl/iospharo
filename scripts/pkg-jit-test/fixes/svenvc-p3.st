"svenvc/P3: P3Client>>retryDelay: ignored its argument.  The method stored the
constant 5 (settings at: #retryDelay put: 5), so a caller that sets another
delay still waits 5 seconds between reconnection tries.  P3ClientTest>>testTimeout
calls retryDelay: 0 to have no wait, and a pool query on an unreachable host
(P3ConnectionPoolTest, 10 tries of 5 seconds) cannot be shortened either.  The
repair stores the argument."

fix value: #P3Client value: #retryDelay:
	value: [ :source | source includesSubstring: 'put: 5' ]
	value:
'retryDelay: seconds
	"Set the delay after which a reconnection will be tried on a failed connection, the default being 5 seconds.
	See also #retryCount:"

	settings at: #retryDelay put: seconds'.
