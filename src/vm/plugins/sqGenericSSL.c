/* sqGenericSSL.c — portable no-op SSL platform backend for builds without a
 * native TLS implementation (i.e. non-Apple; Apple uses sqMacSSL.c via
 * Security.framework).  Its purpose is to let SqueakSSL.c LINK when crypto is
 * enabled (PHARO_WITH_CRYPTO=ON) so the DSAPlugin — which provides the native
 * SHA1/MD5/DSA primitives (primitiveHashBlock / primitiveExpandBlock) — is
 * available.  The TLS primitives themselves gracefully fail with
 * SQSSL_GENERIC_ERROR; no socket/TLS test depends on them in these builds, and
 * a real OpenSSL backend can later replace this file.  See
 * docs/sunit-3way-comparison.md (the SHA1 crypto-config finding). */
#include "sq.h"
#include "SqueakSSL.h"

sqInt sqCreateSSL(void) { return SQSSL_GENERIC_ERROR; }
sqInt sqDestroySSL(sqInt handle) { (void)handle; return 0; }
sqInt sqAcceptSSL(sqInt handle, char* srcBuf, sqInt srcLen, char* dstBuf, sqInt dstLen)
{ (void)handle; (void)srcBuf; (void)srcLen; (void)dstBuf; (void)dstLen; return SQSSL_GENERIC_ERROR; }
sqInt sqConnectSSL(sqInt handle, char* srcBuf, sqInt srcLen, char* dstBuf, sqInt dstLen)
{ (void)handle; (void)srcBuf; (void)srcLen; (void)dstBuf; (void)dstLen; return SQSSL_GENERIC_ERROR; }
sqInt sqEncryptSSL(sqInt handle, char* srcBuf, sqInt srcLen, char* dstBuf, sqInt dstLen)
{ (void)handle; (void)srcBuf; (void)srcLen; (void)dstBuf; (void)dstLen; return SQSSL_GENERIC_ERROR; }
sqInt sqDecryptSSL(sqInt handle, char* srcBuf, sqInt srcLen, char* dstBuf, sqInt dstLen)
{ (void)handle; (void)srcBuf; (void)srcLen; (void)dstBuf; (void)dstLen; return SQSSL_GENERIC_ERROR; }
char* sqGetStringPropertySSL(sqInt handle, int propID) { (void)handle; (void)propID; return 0; }
sqInt sqSetStringPropertySSL(sqInt handle, int propID, char* propName, sqInt propLen)
{ (void)handle; (void)propID; (void)propName; (void)propLen; return 0; }
sqInt sqGetIntPropertySSL(sqInt handle, sqInt propID) { (void)handle; (void)propID; return 0; }
sqInt sqSetIntPropertySSL(sqInt handle, sqInt propID, sqInt propValue)
{ (void)handle; (void)propID; (void)propValue; return 0; }
