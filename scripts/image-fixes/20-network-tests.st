"network-tests cluster.  Entries only; see tools/fix_header.st."

"N1. NetNameResolver class>>localHostAddress: the primitive answers 0.0.0.0 on
macOS under stock Cog (the plugin looks only at the interfaces eth0 and wlan0)
and always 127.0.0.1 under our VM (the primitive is a stub).  The method then
answers the loopback address on a connected machine, which
NetNameResolverTest>>testLocalHostAddress denies.  When the primitive gives no
usable address, use the address that the name of this host resolves to."
fix value: #'NetNameResolver class' value: #localHostAddress
	value: [ :source | (source includesSubstring: 'primAddress = #[0 0 0 0] ifTrue:') ]
	value:
'localHostAddress
	"Return the local address of this host."
	"NetNameResolver localHostAddress"
	"The primitive can answer an empty address (stock Cog looks only at the interfaces eth0 and wlan0, so it answers 0.0.0.0 on macOS) or the loopback address (a VM that stubs the primitive).  In both cases use the address to which the name of this host resolves.  When that fails too, or is a loopback address, use the loopback address."

	| primAddress hostAddress |
	self initializeNetwork.
	[ primAddress := self primLocalAddress ] on: PrimitiveFailed do: [ :err | primAddress := #[0 0 0 0] ].
	(primAddress = #[0 0 0 0] or: [ primAddress first = 127 ]) ifTrue: [
		hostAddress := [ self addressForName: self localHostName ]
			on: NameLookupFailure
			do: [ :err | nil ].
		primAddress := (hostAddress notNil and: [ hostAddress size = 4 and: [ hostAddress first ~= 127 ] ])
			ifTrue: [ hostAddress asByteArray ]
			ifFalse: [ #[127 0 0 1] ] ].
	^ SocketAddress fromOldByteAddress: primAddress'.

"N2. NetNameResolverTest>>testLocalHostAddress: skipped as broken; the cause is
N1, so the skip goes."
fix value: #NetNameResolverTest value: #testLocalHostAddress
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testLocalHostAddress
	NetNameResolver isConnected
		ifTrue: [self deny: self localHostAddressIsLoopBackAddress]
		ifFalse: [self assert: self localHostAddressIsLoopBackAddress]'.

"N3. ZnHTTPSTest>>testSNIPage: the commented-out body works today (the page
redirects from www.mnot.net to mnot.net and ZnClient follows the redirect);
restore the body, with the guards of the other HTTPS tests."
fix value: #ZnHTTPSTest value: #testSNIPage
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testSNIPage
	| client |
	self ensureSocketStreamFactory.
	self isNativeSSLPluginPresent ifFalse: [ ^ self ].
	(client := ZnClient new)
		get: ''https://www.mnot.net/blog/2014/05/09/if_you_can_read_this_youre_sniing''.
	self assert: client isSuccess.
	client close'.

"N4. ZnHTTPSTest>>testGetPharoVersion: an empty skip.  Fetch the zero-conf
script of the Pharo download server over HTTPS (served as bytes, so
decode with asString)."
fix value: #ZnHTTPSTest value: #testGetPharoVersion
	value: [ :source | source includesSubstring: 'self skip' ]
	value:
'testGetPharoVersion
	| client |
	self ensureSocketStreamFactory.
	self isNativeSSLPluginPresent ifFalse: [ ^ self ].
	(client := ZnClient new) get: ''https://get.pharo.org/64/stable''.
	self assert: client isSuccess.
	self assert: (client contents asString includesSubstring: ''Pharo'').
	client close'.

"N5. ZnClientTest>>testQueryGoogle: www.google.com answers a client without
JavaScript with a page that asks for JavaScript, with no search result, so the
assertion on 'pharo.org' fails on every VM.  The test checks that a query is
sent and an HTML answer comes back; use a search engine that answers without
JavaScript."
fix value: #ZnClientTest value: #testQueryGoogle
	value: [ :source | source includesSubstring: 'www.google.com' ]
	value:
'testQueryGoogle
	| client |
	(client := ZnClient new)
		https;
		host: ''www.bing.com'';
		addPath: ''search'';
		queryAt: ''q'' put: ''Pharo Smalltalk'';
		get.
	self assert: client isSuccess.
	self assert: (client response contentType matches: ZnMimeType textHtml).
	self assert: (client contents includesSubstring: ''pharo.org'').
	client close'.
