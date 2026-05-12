# Pharo VM — Code Review (memory-corruption focus)

Repo:   /Users/wohl/esrc/pharo-vm
Branch: pharo-10
Date:   2026-05-11

Method: seven parallel review passes covering core VM, FFI, SqueakSSL/Socket,
File/FileAttr/Misc/LargeInt/DSA, BitBlt/B2D/JPEG/Surface, build/supply chain,
plus targeted re-verification of the prior audit's findings.  Every CRITICAL
and HIGH finding below was spot-checked by hand against the source file at the
cited line; agent claims that did not survive verification have been demoted
or omitted.  Severity is in the "harm if reached / how reachable" sense.

Key conclusions

  * The previously-found bugs are essentially all still present.  One was a
    false positive (missing-`;`-after-`checkFailed()`); one is now PARTIAL
    rather than as-described (aioWin alias is heap-interior, not stack).
    Everything else is unfixed.
  * The heap-damage attack surface is dominated by primitives that take
    image-supplied sizes/offsets and feed them straight into memcpy / array
    indexing without bound-checking.  This is structural: 30+ instances.
  * SqueakSSL is the most dangerous individual subsystem: TLS without cert
    validation on Linux, hostname-check forgery on Windows/macOS, plus six
    independent heap-damage holes.
  * The build chain bootstraps the entire VM by piping HTTP-fetched bash into
    a privileged shell, then signs and uploads the result with
    StrictHostKeyChecking disabled.  A single compromise of get.pharo.org or
    files.pharo.org silently substitutes every shipped binary.

Conventions

  Severity   CRITICAL = remote attacker, no auth, easy / image-controlled
             HIGH     = local attacker w/ image control, or remote w/ peer
             MEDIUM   = local + non-trivial conditions, or DoS
             LOW      = latent / defense in depth

  All file paths are relative to /Users/wohl/esrc/pharo-vm.

================================================================
1. VERIFICATION OF PRIOR AUDIT FINDINGS
================================================================

Status    File:line                                         Item
-------   ----------------------------------------------    ----------------
PRESENT   extracted/vm/src/common/sqNamedPrims.c:56-57      ModuleEntry +0/+1 off-by-one
PARTIAL   extracted/vm/src/win/aioWin.c:455-467             alias is to heap-interior, not stack; window still exists
PRESENT   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c  no SSL_CTX_set_verify (line 89-143)
PRESENT   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c  SSLv23_method, only SSLv2/3 disabled (102-107)
PRESENT   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c  cipher list "!ADH:HIGH:MEDIUM:@STRENGTH" (115)
PRESENT   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c  X509_NAME_get_text_by_NID retval ignored (326-330,413-417)
PRESENT   ffi/src/primitiveUtils.c:22-38                    image-controlled memcpy size
PRESENT   ffi/src/callbacks/callbackPrimitives.c:160-161    unchecked malloc + strcpy in userData
PRESENT   ffi/src/callbacks/callbacks.c:14-32               stack CallbackInvocation in global queue
N/A       (various)                                         "missing `;` after checkFailed()" — false positive; macro body terminates
PRESENT   Jenkinsfile:84,249                                wget|bash, plain HTTP
PRESENT   scripts/runTests.sh:31                            wget -O - https://get.pharo.org/64/80 | bash
PRESENT   Jenkinsfile:97-403                                scp -o StrictHostKeyChecking=no
PRESENT   cmake/importLibFFI.cmake:17-18 et al              mutable git tags
PRESENT   docker/{ubuntu-arm64,debian10-armv7}/Dockerfile   unpinned base images
PRESENT   Jenkinsfile:138 + cmake/sign.cmake:8-11           SIGN_CERT_PASSWORD via env
PRESENT   CMakeLists.txt + cmake/Linux.cmake et al          no -D_FORTIFY_SOURCE / stack-protector / PIE / format-security


================================================================
2. CRITICAL — HEAP DAMAGE / MEMORY CORRUPTION
================================================================

----------------------------------------------------------------
2.1  DSAPrims primitiveBigMultiply: missing `return` after primitiveFailFor → arbitrary heap write
----------------------------------------------------------------
File:   extracted/plugins/DSAPrims/src/common/DSAPrims.c:296-326
Why:    Both class-mismatch (line 300-304) and length-mismatch (line 308-310)
        call primitiveFailFor() WITHOUT returning.  Execution continues to
        line 311 onward, which writes prodPtr[k] in a loop bounded by
        f1Len and f2Len.  prodPtr is sized by prodLen.

        prod = stackValue(0);
        ...
        if (!(prodLen == (f1Len + f2Len))) {
            primitiveFailFor(PrimErrBadArgument);   // <-- no return
        }
        prodPtr = firstIndexableField(prod);
        ...
        for (i = 0; i < f1Len; i += 1) {
          ... for (j = 0; j < f2Len; j += 1) {
                prodPtr[k] = (sum & 0xFF);   k += 1;     // OOB write
              }
              prodPtr[k] = carry;                         // OOB write
        }

Reach:  Any image-side caller can pass a `prod` shorter than f1Len+f2Len.
        Linear, attacker-chosen heap overwrite in the LargePositiveInteger
        bytes object's tail — corrupts whatever follows it in object memory
        (typically other oop slots).  This is a clean heap-write primitive.
Fix:    Add `return 0;` after every primitiveFailFor in this file.

----------------------------------------------------------------
2.2  DSAPrims primitiveBigDivide: missing `return` + index underflow
----------------------------------------------------------------
File:   extracted/plugins/DSAPrims/src/common/DSAPrims.c:170-189 (and
        adjacent code at ~189-260)
Why:    Same missing-return pattern after primitiveFailFor on class check.
        Then `dsaDivisor -= 1` (negative-base trick to allow 1-based
        indexing) followed by reads at `dsaDivisor[divisorDigitCount-1]` and
        `dsaDivisor[divisorDigitCount]`.  When divisorDigitCount is 0 or 1,
        the indexed reads underflow to dsaDivisor[-1] or dsaDivisor[-2].
        Combined with the missing return, an arbitrary oop can be cast to
        a byte array and indexed.
Fix:    Add `return 0;` after every primitiveFailFor; reject
        divisorDigitCount < 2.

----------------------------------------------------------------
2.3  JPEGReadWriter2Plugin: scanline loop bounded by JPEG header, not Form
----------------------------------------------------------------
File:   extracted/plugins/JPEGReadWriter2Plugin/src/common/sqJPEGReadWriter2Plugin.c:202-280
        caller in JPEGReadWriter2Plugin.c:340-381

Why:    The decompress loop runs while `pcinfo->output_scanline <
        pcinfo->output_height` (JPEG header) but writes
        `bitmap[((pcinfo->output_scanline - 1) * wordsPerRow) + j]` where
        `wordsPerRow` is computed from the FORM's width (line 367) and the
        size check at JPEGReadWriter2Plugin.c:370-371 uses
        `formPitch * formHeight` (formHeight, not output_height) — and
        that multiply is itself a wrap-prone signed multiply.  A JPEG with
        output_height > formHeight (or output_width > formWidth so j
        overruns at end of row) writes far past the bitmap allocation.

Reach:  Process any malicious JPEG.  This is a remote attacker primitive.
        Linear heap overwrite with attacker-chosen bytes (the decoded pixel
        values).
Fix:    Reject the JPEG when output_width != formWidth or output_height !=
        formHeight before entering the loop.  Use safe-multiply when
        validating destination size.

----------------------------------------------------------------
2.4  JPEGReaderPlugin: blockIndex-driven OOB pointer fetch + deref
----------------------------------------------------------------
File:   extracted/plugins/JPEGReaderPlugin/src/common/JPEGReaderPlugin.c:438
        (also ~551, 573, 596 in primitiveColorConvertMCU)

        blockIndex = ((((usqInt) dy >> 3)) * (yComponent[BlockWidthIndex]))
                   + (((usqInt) dx >> 3));
        sampleIndex = (((usqInt) (dy & 7) << 3)) + (dx & 7);
        sample = (yBlocks[blockIndex])[sampleIndex];

Why:    `yBlocks` is a fixed array of 128 pointers (MaxMCUBlocks).
        `BlockWidthIndex` and the dx/dy loop bounds derive from
        image-supplied JPEGColorComponent fields with no upper bound
        check.  blockIndex can exceed 128 → reads a wild pointer from
        adjacent stack/heap, then dereferences it for sampleIndex bytes.
Reach:  Image-side caller controls the JPEG state object directly.
        OOB read/write or crash; potential type-confusion read primitive.
Fix:    Bound blockIndex < MaxMCUBlocks; bound dx/dy by per-component
        block extents.

----------------------------------------------------------------
2.5  FFI typesPrimitives: type-confusion via getHandler on arbitrary oop
----------------------------------------------------------------
File:   ffi/src/typesPrimitives.c:170-172 (and reached through
        ffi_prep_cif/ffi_prep_closure_loc downstream)

        for(int i=0; i < membersSize; i++){
            memberTypes[i] = getHandler(stObjectat(arrayOfMembers, i + 1));
        }

Why:    `getHandler` returns the first slot of any oop (it's a generic
        handler-extractor).  No check that the result is an actual
        ffi_type*.  An attacker-crafted "type" object whose first slot
        points to chosen memory becomes an ffi_type whose `size`,
        `alignment`, `type`, `elements` are read by libffi when computing
        struct layout — controlled-dispatch / arbitrary-read primitive.
Reach:  Any image that registers an FFI struct can do this; in a Pharo
        environment loading an untrusted package this is a confused-deputy
        escape.
Fix:    Tag-check the oop's class against an FFIType class before
        calling getHandler, or store a self-describing magic in the
        ffi_type allocation and verify before use.

----------------------------------------------------------------
2.6  FFI typesPrimitives: use-after-free / dangling structType on libffi error
----------------------------------------------------------------
File:   ffi/src/typesPrimitives.c:174-188

        setHandler(receiver, structType);              // image now holds ptr
        if(failed()){ free(memberTypes); free(structType); free(offsets); return; }
        if(ffi_get_struct_offsets(...) != FFI_OK){
            free(memberTypes); free(structType); free(offsets);  // <-- frees but
            primitiveFail(); return;                              //     receiver
        }                                                          //     still holds it

Why:    `setHandler` stores structType into the receiver Smalltalk object.
        If the subsequent failed() OR ffi_get_struct_offsets() fails, the
        memory is freed but the receiver's handler slot still holds the
        dangling pointer.  Next primitive that calls getHandler(receiver)
        gets a freed-then-reallocated arena, leading to use-after-free
        (read primitive, type confusion in libffi, or double-free if the
        receiver later finalizes).
Fix:    Either don't call setHandler until after both checks pass, or
        clear the handler slot before freeing.

----------------------------------------------------------------
2.7  FFI primitiveUtils: image-controlled memcpy size
----------------------------------------------------------------
File:   ffi/src/primitiveUtils.c:22-38  [PREVIOUSLY FOUND, STILL PRESENT]

        size = stackIntegerValue(0);
        ...
        memcpy(toAddress, fromAddress, size);

Why:    `size` is an arbitrary image-supplied integer cast to size_t.  No
        bound against either buffer's capacity.  A negative sqInt becomes
        SIZE_MAX-ish — instant heap obliteration.  Image-controllable
        arbitrary-size copy.

----------------------------------------------------------------
2.8  FFI callbacks: stack-allocated CallbackInvocation in global queue
----------------------------------------------------------------
File:   ffi/src/callbacks/callbacks.c:14-32  [PREVIOUSLY FOUND, STILL PRESENT]

        CallbackInvocation invocation;            // STACK
        ...
        callback->runner->callbackStack = &invocation;
        queue_add_pending_callback(&invocation);
        callback->runner->callbackEnterFunction(callback->runner, &invocation);

Why:    In same-thread mode, `callbackExitFunction` does sig_longjmp and
        never returns through callbackFrontend — so the post-enter pop
        (primitiveCallbackReturn ~line 222) is not reached.  Once the
        stack unwinds, runner->callbackStack and the queue entry both
        point to invalidated stack memory.  Next callback dereferences a
        dangling pointer in `invocation.previous` chain.

----------------------------------------------------------------
2.9  SqueakSSL Unix: TLS without certificate validation
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c:89-143, 326-338
        [PRIOR FINDING + the bypass mechanism is now mapped]

Why:    sqSetupSSL never calls SSL_CTX_set_verify, so verify_mode is
        SSL_VERIFY_NONE.  SSL_get_verify_result() (line 335) returns
        X509_V_OK by default in that mode regardless of cert validity.
        Then ssl->certFlags is set to SQSSL_OK (line 338).  The image's
        "is this connection verified?" check therefore returns yes for any
        cert.  When no serverName was supplied (NO_MATCH_DONE_YET path,
        line 324-331), the code falls back to copying the cert's CN into
        peerName and still returns OK.

        Combine with: weak cipher list ("!ADH:HIGH:MEDIUM:@STRENGTH" on
        line 115), TLS 1.0/1.1 still enabled (only SSLv2/v3 disabled at
        line 107), no SSL_OP_NO_COMPRESSION, no SSL_OP_NO_RENEGOTIATION.

Reach:  Network MITM against any HTTPS use of SqueakSSL on Linux/BSD.
Fix:    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        Tighter cipher list; SSL_OP_NO_COMPRESSION;
        Treat NO_MATCH_DONE_YET as failure when peerName is required.


================================================================
3. HIGH — HEAP DAMAGE / MEMORY CORRUPTION
================================================================

----------------------------------------------------------------
3.1  UnixOSProcessPlugin realpathAsType: 1024-byte buffer for realpath()
----------------------------------------------------------------
File:   extracted/plugins/UnixOSProcessPlugin/src/common/UnixOSProcessPlugin.c:4185-4214

        bufferSize = 1024;
        newPathString = instantiateClassindexableSize(classString(), bufferSize);
        ...
        buffer = arrayValueOf(newPathString);
        realpathResult = realpath(pathString, buffer);          // writes ≤ PATH_MAX
        if (realpathResult == 0) { return primitiveFail(); }
        else {
            if ((strlen(realpathResult)) >= 1024) {
                logErrorFromErrno("warning: ... object memory may have been corrupted");

Why:    Linux PATH_MAX is 4096; realpath(3) writes up to PATH_MAX bytes.
        The post-call `>= 1024` check happens AFTER the OOB write into
        Pharo object memory.  The error string itself admits it.
Reach:  Any image, any path with a long resolved form (deeply nested
        symlinks, network mounts).  Linear heap overwrite into the
        instantiated Smalltalk String oop's neighbours.
Fix:    Use a stack buffer of PATH_MAX, then instantiate the string
        sized to strlen(result).

----------------------------------------------------------------
3.2  FileAttributesPlugin: readlink off-by-one stack overflow
----------------------------------------------------------------
File:   extracted/plugins/FileAttributesPlugin/src/unix/faSupport.c:470-483

        char targetFile[FA_PATH_MAX];
        ...
        status = readlink(faGetPlatPath(aFaPath), targetFile, FA_PATH_MAX);
        if (status >= 0) {
            targetFile[status] = 0;            // status==FA_PATH_MAX → +1 OOB

Why:    readlink does not NUL-terminate.  When the symlink target is
        exactly FA_PATH_MAX bytes, the trailing-NUL write smashes the
        adjacent stack slot (frame pointer / canary / return address).
Reach:  Attacker who can plant a long symlink on the local filesystem +
        any image that queries link targets.
Fix:    `status = readlink(..., FA_PATH_MAX-1); if (status < 0) ...; targetFile[status] = 0;`

----------------------------------------------------------------
3.3  MiscPrimitivePlugin primitiveDecompressFromByteArray: index validation + bm-type confusion
----------------------------------------------------------------
File:   extracted/plugins/MiscPrimitivePlugin/src/common/MiscPrimitivePlugin.c:431-465

        bm = arrayValueOf(stackValue(2));         // <-- no isWords/isBitmap check
        if (isOopImmutable(stackValue(2))) ...
        if (!(isBytes(stackValue(1)))) ...
        ba = firstIndexableField(stackValue(1));
        index = stackIntegerValue(0);             // <-- no >= 1 check
        ...
        i = index - 1;                            // can be -1, INT_MIN, etc.
        ...
        while (i < end) {
            anInt = ba[i];                        // OOB read at ba[-1]+

Why:    `index` is image-controllable; the only check is failed() after
        the conversion.  No `index >= 1` test before `i = index - 1`.
        Negative i is signed, so `while (i < end)` enters the loop and
        reads ba[i] far before the array.  Separately, bm has no
        isWords/isBitmap check; if a ByteArray is passed where the loop
        treats `bm[k]` as int, writes write 4× the assumed offset.
Reach:  Image primitive; arbitrary heap read with attacker-chosen
        negative offsets.
Fix:    Validate `index >= 1`; check `isWords(bm) || isBitmap(bm)`.

----------------------------------------------------------------
3.4  MiscPrimitivePlugin primitiveCompressToByteArray: signed-overflow size check
----------------------------------------------------------------
File:   extracted/plugins/MiscPrimitivePlugin/src/common/MiscPrimitivePlugin.c:200-215

        size = sizeOfSTArrayFromCPrimitive(bm);
        destSize = sizeOfSTArrayFromCPrimitive(ba);
        if (destSize < (((size * 4) + 7) + ((size / 0x7C0) * 3))) {
            return primitiveFailFor(PrimErrUnsupported);
        }

Why:    `size * 4` is signed sqInt; on 32-bit builds it wraps for
        size > 2³⁹.  Wrapped expression < destSize — check passes.
        Subsequent compression writes far beyond ba.  Same `bm` lacks
        isWords/isBitmap check.
Fix:    Use uint64 / overflow-checked multiply; check bm format.

----------------------------------------------------------------
3.5  BitBltPlugin: depth==0 → divide-by-zero, then int-overflow on size check
----------------------------------------------------------------
File:   extracted/plugins/BitBltPlugin/src/common/BitBltPlugin.c:3083-3107
        and 3233-3260 (warping path), and 5526-5537 (primitivePixelValueAt)

        destDepth = fetchIntegerofObject(FormDepthIndex, destForm);
        if (!((destMSB = destDepth > 0))) { destDepth = 0 - destDepth; }
        ...
        destPPW = 32 / destDepth;                                  // /0 if depth==0
        destPitch = ((destWidth + (destPPW - 1)) / destPPW) * 4;
        destBitsSize = byteSizeOf(destBits);
        if (!(destBitsSize >= (destPitch * destHeight))) ...       // signed overflow

Why:    No upper or non-zero bound on depth.  destPitch * destHeight is
        signed int; for destWidth = 0x10001, destHeight = 0x10001 at
        depth = 32 the product wraps to a small value smaller than
        destBitsSize → check passes despite write region being multi-GB.
Reach:  Form construction is image-side.  DoS at minimum, OOB write at
        worst (BitBlt then writes through destBits with the wrapped
        pitch).
Fix:    Reject destDepth not in {1,2,4,8,16,32}; use checked multiply
        for the size validation.

----------------------------------------------------------------
3.6  SurfacePlugin manualSurface: width*depth signed-overflow + raw pointer write
----------------------------------------------------------------
File:   extracted/plugins/SurfacePlugin/src/common/sqManualSurface.c:126,152-167

        if (rowPitch < (width*depth)/8) return -1;         // signed mul overflows
        ...
        int setManualSurfacePointer(int surfaceID, void* ptr) {
            ...  surface->ptr = ptr;
        }

Why:    Two issues compounding: (a) width*depth wraps negative for large
        width, defeating the rowPitch lower bound, and (b)
        setManualSurfacePointer accepts an arbitrary void* with no length
        association — BitBlt then uses the pointer for writes through
        manualSurfaceLock.  Combined, an image can register a tiny
        surface with a wild pointer and a wraparound rowPitch.
Fix:    Use unsigned64 arithmetic on width*depth; require an explicit
        length parameter to setManualSurfacePointer; verify the buffer
        belongs to the caller (FFI Pinned region).

----------------------------------------------------------------
3.7  B2DPlugin primitiveCopyBuffer: GWBufferTop unchecked
----------------------------------------------------------------
File:   extracted/plugins/B2DPlugin/src/common/B2DPlugin.c:10770-10781

        diff = (slotSizeOf(buf2)) - (slotSizeOf(buf1));
        if (diff < 0) return primitiveFailFor(GEFSizeMismatch);
        ...
        for (i = 0; i < (workBuffer[GWBufferTop]); i += 1) { dst[i] = src[i]; }

Why:    loadWorkBufferFrom validates GWSize against the slot size, but
        not GWBufferTop.  A WorkBuffer where GWBufferTop > GWSize copies
        far past dst's allocation.
Fix:    Validate GWBufferTop ≤ slotSizeOf(buf1) ≤ slotSizeOf(buf2).

----------------------------------------------------------------
3.8  B2DPlugin: nSegments * 3 / 6 signed-overflow defeats length check
----------------------------------------------------------------
File:   extracted/plugins/B2DPlugin/src/common/B2DPlugin.c:9499-9522
        also 9777, 9782 (primitiveAddCompressedShape), 9672-9674

        length = slotSizeOf(points);
        if (!((length == (nSegments * 3)) || (length == (nSegments * 6)))) ...

Why:    nSegments * 3 wraps; an attacker picks nSegments = 0x55555556 so
        the product equals 1, then supplies a length-1 points array.
        Loader iterates nSegments items into the work buffer.

----------------------------------------------------------------
3.9  src/externalPrimitives.c: strcpy of caller-controlled lookupName
----------------------------------------------------------------
File:   src/externalPrimitives.c:126-138

        char buf[256];
        ...
    #ifdef _WIN32
        strcpy_s(buf, 256, lookupName);          // truncates on Windows
    #else
        strcpy(buf, lookupName);                 // unbounded on Unix/macOS
    #endif
        snprintf(buf+strlen(buf), sizeof(buf) - strlen(buf), "AccessorDepth");

Why:    `lookupName` is the primitive name from the image symbol table.
        An image with primitive names > 256 bytes overflows `buf` on
        every plugin load on Linux/macOS.  Stack overflow.
Fix:    Use snprintf(buf, sizeof(buf), "%sAccessorDepth", lookupName).

----------------------------------------------------------------
3.10  src/utils.c: setVMName/setImageName/setVMPath strcpy into PATH_MAX globals
----------------------------------------------------------------
File:   src/utils.c:182,203,220 (Unix path)

        strcpy(vmName, name);
        strcpy(imageName, name);
        strcpy(vmFullPath, name);

Why:    All three set* functions strcpy a caller-supplied path into a
        fixed-size global buffer with no length check.  Reachable from
        client.c:225 → setImageName(getFullPath(...)) where getFullPath
        can produce a path longer than PATH_MAX through symlink
        resolution on Linux.  Adjacent globals corrupted.
Fix:    Bounded copy + truncate-with-error.

----------------------------------------------------------------
3.11  src/utilsMac.mm: strcpy of NSBundle path into PATH_MAX vmPath
----------------------------------------------------------------
File:   src/utilsMac.mm:10
Why:    `[appFolder fileSystemRepresentation]` length is unbounded.  An
        app installed in a deep directory path can corrupt the
        adjacent imageName/vmName globals via the unconditional strcpy.

----------------------------------------------------------------
3.12  src/win/winDebugWindow.c: newline-doubling overflow
----------------------------------------------------------------
File:   src/win/winDebugWindow.c:127,182-193

        char logBuffer2[LOGBUFFER_SIZE * 2 + 1];
        while(logIndex <= logLimit){             // <= writes the NUL too
            if(logBuffer[logIndex] == '\n'){ logBuffer2[logIndex2++] = '\r'; }
            logBuffer2[logIndex2] = logBuffer[logIndex];
            logIndex++; logIndex2++;
        }

Why:    For an input of L `\n` plus terminator, output is 2*(L+1) bytes
        but the buffer is 2*LOGBUFFER_SIZE+1 — short by 1 when L equals
        LOGBUFFER_SIZE.  Stack overflow into the debug log window code
        path.

----------------------------------------------------------------
3.13  src/fileUtilsWin.c: FILE_NAME_INFO trailing-NUL OOB write
----------------------------------------------------------------
File:   src/fileUtilsWin.c:64

        int size = sizeof(FILE_NAME_INFO) + sizeof(WCHAR) * MAX_PATH;
        nameinfo = malloc(size);
        ... pGetFileInformationByHandleEx(fdHandle, FileNameInfo, nameinfo, size) ...
        nameinfo->FileName[nameinfo->FileNameLength / sizeof(WCHAR)] = L'\0';

Why:    The Windows API returns the actual stored FileNameLength (which
        reflects the underlying object name, not the buffer size).  When
        the name fills the buffer exactly, the divide-by-2 NUL write
        lands one WCHAR past the last writable slot — heap corruption
        in the malloc'd buffer's neighbour.

----------------------------------------------------------------
3.14  SqueakSSL Unix sqCreateSSL: unchecked calloc + realloc + BIO_new
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c:152-176

        ssl = calloc(1, sizeof(sqSSL));
        ssl->bioRead = BIO_new(BIO_s_mem());      // ssl may be NULL → NULL deref
        ...
        handleBuf = realloc(handleBuf, ...);      // realloc → NULL leaks old buf,
        for(i = handleMax; ...) handleBuf[i] = NULL;  // then NULL deref

Why:    Multiple unchecked allocation results.  realloc-fail also leaks
        the previous handleBuf.

----------------------------------------------------------------
3.15  SqueakSSL Win32 sqExtractPeerName: alloca() with attacker-controllable size
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/win/sqWin32SSL.c:284-295

        cchTmpBuf = CertGetNameString(certHandle, ..., NULL, 0);
        tmpBuf = (LPTSTR)alloca(cchTmpBuf * sizeof(TCHAR));   // peer-controlled size
        CertGetNameString(certHandle, ..., tmpBuf, cchTmpBuf);
        ...
        ssl->peerName = calloc(1, cbPeerName);      // not checked
        WideCharToMultiByte(CP_UTF8, 0, tmpBuf, -1, ssl->peerName, cbPeerName, ...);

Why:    A peer cert with an enormous CN drives an unchecked alloca() →
        stack overflow.  The follow-on calloc is unchecked.
Fix:    Cap cchTmpBuf at a sane upper bound; use heap, not alloca; check
        calloc.

----------------------------------------------------------------
3.16  SqueakSSL Win32 sqEncryptSSL: int-overflow → memcpy past dstBuf
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/win/sqWin32SSL.c:746-769

        ssl->inbuf[2].pvBuffer = dstBuf + ssl->inbuf[0].cbBuffer + ssl->inbuf[1].cbBuffer;
        ...
        total = ssl->inbuf[0].cbBuffer + ssl->inbuf[1].cbBuffer + ssl->inbuf[2].cbBuffer;
        if(dstLen < total) return SQSSL_BUFFER_TOO_SMALL;
        memcpy(ssl->inbuf[1].pvBuffer, srcBuf, srcLen);

Why:    `total` is signed int sum of three SChannel-supplied DWORDs;
        sum can wrap negative or to a small positive — bypassing the
        buffer-too-small check.  Heap overflow in the wrapped case.

----------------------------------------------------------------
3.17  SqueakSSL: handle table negative-index OOB
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c:74-76
        also win/sqWin32SSL.c:96-98 and osx/sqMacSSL.c:116-119

        static sqSSL *sslFromHandle(sqInt handle) {
            return handle < handleMax ? handleBuf[handle] : NULL;
        }

Why:    `handle` comes from stackIntegerValue and is signed.  No lower-
        bound check.  An image-supplied -1 / INT_MIN reads handleBuf at a
        large negative offset → OOB pointer read, then deref of whatever
        is there as an sqSSL*.
Fix:    `if (handle < 1 || handle >= handleMax) return NULL;`

----------------------------------------------------------------
3.18  SqueakSSL Unix sqConnectSSL: peerName from uninitialized stack
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c:319-330,
        and 405-417 in sqAcceptSSL

        char peerName[254];               // STACK, uninitialized
        X509_NAME_get_text_by_NID(..., peerName, sizeof(peerName));   // ret ignored
        logTrace("sqConnectSSL: peerName = %s\n", peerName);
        ssl->peerName = strndup(peerName, sizeof(peerName) - 1);

Why:    When the cert lacks the requested NID, X509_NAME_get_text_by_NID
        returns -1 and does NOT touch peerName.  logTrace then prints up
        to NUL of stack content (info leak), and strndup copies stack
        garbage into the image as ssl->peerName.  Subsequent
        primitiveGetStringProperty (SqueakSSL.c:436-441) reads strlen on
        that — possible OOB read into the heap if a strndup'd buffer is
        not NUL-terminated within bounds.

----------------------------------------------------------------
3.19  SqueakSSL Win32: peerName forged from serverName (cert-bypass)
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/win/sqWin32SSL.c:269-275 and 215,349-353

Why:    When certFlags == SQSSL_OK and serverName provided,
        sqExtractPeerName UNCONDITIONALLY copies serverName into
        peerName.  No real subject extraction.  Combined with
        SCH_CRED_MANUAL_CRED_VALIDATION + epp.pwszServerName = NULL
        (no Windows-side hostname check), the image-side peerName ==
        serverName check is meaningless.
Fix:    Either let SChannel verify the hostname (set pwszServerName) or
        extract the actual subject CN/SAN and compare.

----------------------------------------------------------------
3.20  SqueakSSL macOS: BreakOnServerAuth + manual verify without hostname
----------------------------------------------------------------
File:   extracted/plugins/SqueakSSL/src/osx/sqMacSSL.c:154-201, 262-272, 363-383

Why:    SecureTransport's auto cert verification is disabled
        (kSSLSessionOptionBreakOnServerAuth).  The manual sqVerifyCert
        calls SecTrustEvaluate but uses no SSL policy carrying the
        hostname, so trust evaluation does not check the cert against
        serverName.  As on Windows, peerName is then forged from
        serverName, defeating image-side checks.

----------------------------------------------------------------
3.21  src/threadSafeQueue: read of `queue->first` outside the mutex
----------------------------------------------------------------
File:   src/threadSafeQueue/threadSafeQueue.c:113-137

        if (queue->semaphore->wait(queue->semaphore) != 0) ...
        TSQueueNode *node = queue->first;          // OUTSIDE mutex
        if(node == NULL) return NULL;
        void *element = node->element;             // still outside
        platform_semaphore_wait(queue->mutex);

Why:    Two consumers passed the semaphore can both read `queue->first`
        and `node->element` concurrently.  Then the lock-holder unlinks
        and `free(node)`.  The other thread then walks freed memory —
        use-after-free.  Used by the FFI worker queue and callback
        queue, so the bug is reachable via any FFI workload.

----------------------------------------------------------------
3.22  SocketPlugin: AIO close-handler use-after-free in sqSocketDestroy
----------------------------------------------------------------
File:   extracted/plugins/SocketPlugin/src/common/SocketPluginImpl.c:1109-1123

        if (SOCKET(s))
            sqSocketAbortConnection(s);     // may queue closeHandler(...,pss,...)
        if (PSP(s))
            free(PSP(s));                   // closeHandler may still fire on freed pss

Why:    Classic AIO-registration UAF.  Async epoll/select dispatch can
        fire on the closed/freed fd, dereferencing pss.

----------------------------------------------------------------
3.23  SocketPlugin: stat() on non-NUL-terminated image buffer
----------------------------------------------------------------
File:   extracted/plugins/SocketPlugin/src/common/SocketPluginImpl.c:1898-1923

        if (servSize && (family == SQ_SOCKET_FAMILY_LOCAL) && ...) {
            struct stat st;
            if ((0 == stat(servName, &st)) && (st.st_mode & S_IFSOCK))

Why:    `servName` is `firstIndexableField(stackValue(4))` — image bytes
        with no NUL guarantee.  stat() reads until it finds NUL byte
        (OOB read of the heap).

----------------------------------------------------------------
3.24  SocketPlugin: strncpy without NUL-term on DNS PTR record
----------------------------------------------------------------
File:   extracted/plugins/SocketPlugin/src/common/SocketPluginImpl.c:1698

        strncpy(lastName, res, MAXHOSTNAMELEN);   // res from gethostbyaddr h_name

Why:    A poisoned PTR record (a hostile DNS server) returns a name
        ≥ MAXHOSTNAMELEN bytes.  lastName is not NUL-terminated;
        sqResolverAddrLookupResultSize then runs strlen off the end.

----------------------------------------------------------------
3.25  LargeIntegers digitLshift: shiftCount overflow → undersized alloc
----------------------------------------------------------------
File:   extracted/plugins/LargeIntegers/src/common/LargeIntegers.c:642-645

        newByteLen = ((highBit + shiftCount) + 7) / 8;
        ...
        newOop = instantiateClassindexableSize(fetchClassOf(anOop), newByteLen);

Why:    shiftCount is image-supplied (primDigitBitShiftMagnitude line
        1307).  On 32-bit sqInt, large shiftCount overflows the sum,
        producing a small newByteLen.  Subsequent cDigitLshift writes
        far past the new oop's allocation.

----------------------------------------------------------------
3.26  Module-load off-by-one (was previously found)
----------------------------------------------------------------
File:   extracted/vm/src/common/sqNamedPrims.c:56-57

        module = (ModuleEntry*) calloc(1, sizeof(ModuleEntry) + strlen(pluginName));
        strcpy(module->name, pluginName);

Why:    `name[1]` flexible member — allocates strlen, but strcpy writes
        strlen+1 bytes (incl. NUL).  1-byte heap overflow on every
        plugin load.  calloc zero-fills, but strcpy still writes the
        NUL one byte past the allocation (typically into the next
        chunk's metadata or padding).

================================================================
4. CRITICAL/HIGH — CRASH BUGS (NULL deref, divide-by-zero)
================================================================

----------------------------------------------------------------
4.1  src/client.c → setImageName(NULL) crash on realpath failure
----------------------------------------------------------------
File:   src/client.c:222-225 → src/utils.c:203 (strcpy(imageName, NULL))

        char* fullImageName = alloca(FILENAME_MAX);
        fullImageName = getFullPath(fileName, fullImageName, FILENAME_MAX);
        setImageName(fullImageName);

Why:    realpath() returns NULL on permission/missing-component errors.
        setImageName then strcpy's from NULL → SEGV during VM init.
        Identical pattern at parameters/parameters.c:751-755 for
        setVMPath.

----------------------------------------------------------------
4.2  src/pathUtilities.c: strrchr + null-check off-by-one
----------------------------------------------------------------
File:   src/pathUtilities.c:233-237

        char *fileExtension = strrchr(name, '.');
        if(!extension) continue;            // <-- WRONG: checks `extension`, not `fileExtension`
        if(strcmp(fileExtension, extension) != 0) continue;

Why:    The null check tests the WRONG variable.  If `name` has no `.`,
        fileExtension == NULL and strcmp(NULL, ...) crashes.  Trivially
        triggered by any directory entry without a dot (e.g. "Makefile").

----------------------------------------------------------------
4.3  src/pathUtilities.c: first[strlen(first)-1] when first is empty
----------------------------------------------------------------
File:   src/pathUtilities.c:163

        if(first[strlen(first)-1] != SEPARATOR_CHAR) {

Why:    When `first` is "", strlen returns 0 and `first[-1]` reads one
        byte before the buffer.  Reachable via parameters.c:210-212
        fallback.

----------------------------------------------------------------
4.4  BitBlt/B2D divide-by-zero on Form depth
----------------------------------------------------------------
File:   extracted/plugins/BitBltPlugin/src/common/BitBltPlugin.c:3083, 3260, 5526
        and B2DPlugin.c:9672

        ppw = 32 / bmDepth;        // crash if bmDepth == 0

Why:    Depth is fetched from the Form with no zero/range check.

----------------------------------------------------------------
4.5  JPEGReader divide-by-zero on partial-zero scale
----------------------------------------------------------------
File:   extracted/plugins/JPEGReaderPlugin/src/common/JPEGReaderPlugin.c:431-434

        if (!((sx == 0) && (sy == 0))) {
            dx = dx / sx;          // crash if only sx==0 (or only sy==0)
            dy = dy / sy;
        }

----------------------------------------------------------------
4.6  src/win/winDebug.c: fopen() return unchecked in crash handler
----------------------------------------------------------------
File:   src/win/winDebug.c:138-167, 430-438

        crashDumpFile = fopen(crashdumpFileName, "a+");
        vm_setVMOutputStream(crashDumpFile);
        reportStackState(exp, date, crashDumpFile);
        ...
        fclose(crashDumpFile);

Why:    The crash handler itself crashes when stdout/PWD isn't writable
        — silencing the original crash.  The Unix counterpart in
        debugUnix.c:60-69 does null-check, so this is platform-asymmetric.

----------------------------------------------------------------
4.7  Multiple unchecked malloc in extracted/vm/src/win/aioWin.c
----------------------------------------------------------------
File:   extracted/vm/src/win/aioWin.c:38, 455, 461, 498, 522
Why:    Network-heavy load → OOM → instant NULL deref instead of clean
        error.  aioWin.c:457/465 also has the prior alias finding
        (heap-interior pointer transiently shared with later malloc).

----------------------------------------------------------------
4.8  FFI worker/sameThread: many unchecked mallocs
----------------------------------------------------------------
Files:  ffi/src/sameThread/sameThread.c:35-39 (vmcc → sigsetjmp deref)
        ffi/src/worker/workerTask.c:14, 28, 36
        ffi/src/functionDefinitionPrimitives.c:11-13, 29-31 (cif)
        ffi/src/callbacks/callbackPrimitives.c:35 (stringForCString on
            possibly-NULL userData)

Why:    Every malloc-then-deref in this layer is a clean NULL deref on
        OOM.  Several propagate NULL into libffi which itself does not
        check.

----------------------------------------------------------------
4.9  extracted/vm/src/common/sqExternalSemaphores.c: realloc-NULL deref + leak + int overflow
----------------------------------------------------------------
File:   extracted/vm/src/common/sqExternalSemaphores.c:106-109

        signalRequests = realloc(signalRequests, sz * sizeof(SignalRequest));
        memset(signalRequests + numSignalRequests, 0, ...);

Why:    realloc-fail aliases the failure into the original pointer (leak)
        AND immediately memsets through NULL+offset (SEGV).  Also `sz *
        sizeof(SignalRequest)` is `int * size_t` and can overflow when
        the image header reports a large external-semaphore count.

----------------------------------------------------------------
4.10  Format-string footgun in error()
----------------------------------------------------------------
File:   src/debug.c:45

        void error(char *errorMessage){ logError(errorMessage); ... }

Why:    `logError(...)` macros into `logMessage(..., __VA_ARGS__)` which
        treats the first va_arg as the format.  Today every caller passes
        a literal, but the API contract is that any caller-controlled
        text would give an attacker `%n`/`%s` primitives.  The function
        is publicly exported.

----------------------------------------------------------------
4.11  extracted/vm/src/unix/aio.c: epoll_wait with -1 fd
----------------------------------------------------------------
File:   extracted/vm/src/unix/aio.c:290-292

        epollDescriptor = fillEPollDescriptor();
        epollReturn = epoll_wait(epollDescriptor, ...);

Why:    fillEPollDescriptor returns -1 on epoll_create1/epoll_ctl
        failure; -1 isn't checked before epoll_wait.  Tight error-log
        loop, errno pollution.

================================================================
5. MEDIUM — RACE / SIGNAL-UNSAFETY / OTHER
================================================================

5.1  signal handlers async-signal-unsafe + SA_NODEFER
     File: src/debugUnix.c:88-95, 122-162
     The SIGSEGV/SIGBUS/SIGFPE handler runs fopen/vfprintf/
     backtrace_symbols_fd/platform_semaphore_wait/ctime_r — none of
     which are async-signal-safe.  SA_NODEFER allows recursive entry
     into the same handler.  Combined with #5.2 below, signal handling
     under crash is undefined.

5.2  sa_mask never initialized via sigemptyset
     File: src/debugUnix.c:123, 144, 154
     term_handler_action and sigpipe_handler_action's sa_mask is
     uninitialized stack garbage — kernel reads it to decide what to
     mask during the handler.  Behavior is build-dependent.

5.3  externalPrimitives.c global moduleNameBuffer race
     File: src/externalPrimitives.c:57, 66
     Two concurrent loads scribble each other's path; downstream
     LoadLibrary/dlopen receives a torn string.

5.4  FFI runner->callbackStack updated unlocked from arbitrary thread
     File: ffi/src/callbacks/callbacks.c:24-29
     Reentrant callbacks from multiple threads on the same Runner
     corrupt the chain.

5.5  SqueakSSL Unix sqWin32SSL.c sqAddPfxCertToStore
     File: extracted/plugins/SqueakSSL/src/win/sqWin32SSL.c:903-933
     Unchecked CertOpenSystemStore + WCHAR password buffer arithmetic
     using DWORD return value as int.  Crash on restricted systems.

5.6  SqueakSSL macOS sqGetPeerCertificates leaks `trust` on alloc fail
     File: extracted/plugins/SqueakSSL/src/osx/sqMacSSL.c:298-323

5.7  glibc strerror_r return value ignored
     File: src/debug.c:57-66
     The GNU variant returns a pointer that may not write to the
     buffer; subsequent printing of the buffer leaks uninitialized
     stack.

5.8  imageAccess.c: sz * count overflow before fread
     File: src/imageAccess.c:78, 136

5.9  win32Main.c: totalSize int overflow before malloc
     File: src/win32Main.c:33-37

5.10 sqUnixCharConv.c: toupper(signed char) is UB for non-ASCII
     File: extracted/vm/src/unix/sqUnixCharConv.c:210
     Same line: malloc unchecked.

5.11 LargeIntegers primMontgomeryTimesModulo: empty operands → OOB read
     File: extracted/plugins/LargeIntegers/src/common/LargeIntegers.c:2278-2317
     pSecond[0]/pThird[0] read without checking secondLen >= 1.

5.12 UnixOSProcessPlugin cStringFromString: unchecked calloc + len+1 overflow
     File: extracted/plugins/UnixOSProcessPlugin/src/common/UnixOSProcessPlugin.c:411-422

5.13 BitBlt cmShiftTable / cmMaskTable: shift amounts ≥ 32 are UB
     File: extracted/plugins/BitBltPlugin/src/common/BitBltPlugin.c:987-990, 2404-2407
     ColorMap entries are user-controlled.

5.14 JPEG huffman tables: 1U << byte where byte ≥ 32 is UB
     File: extracted/plugins/JPEGReaderPlugin/src/common/JPEGReaderPlugin.c:738, 798

5.15 SocketPlugin: (startIndex + count) - 1 ≤ slotSizeOf(array) wraps
     File: extracted/plugins/SocketPlugin/src/common/SocketPlugin.c:1795,1858,2060

5.16 SocketPlugin findOption: strncpy non-NUL-term footgun
     File: extracted/plugins/SocketPlugin/src/common/SocketPluginImpl.c:1534-1547

5.17 SocketPlugin: file-static lastError pollution
     File: extracted/plugins/SocketPlugin/src/common/SocketPluginImpl.c:2494-2496

5.18 FilePlugin macOS: strncpy(lastPath, unixPath, MAXPATHLEN) no NUL
     File: extracted/plugins/FilePlugin/src/osx/sqUnixFile.c:143

5.19 FileAttributesPlugin: access()/lstat() vs open() TOCTOU
     File: extracted/plugins/FileAttributesPlugin/src/unix/faSupport.c:435-448

5.20 macAlias.c: FSRefMakePath hard-codes PATH_MAX, ignoring caller bound
     File: src/macAlias.c:41

5.21 LocalePlugin currency-symbol two-call TOCTOU
     File: extracted/plugins/LocalePlugin/src/common/LocalePlugin.c:160-162

5.22 LocalePlugin / SqueakSSL OSX vsprintf into fixed 1024-byte stack buffer
     File: extracted/plugins/SqueakSSL/src/osx/sqMacSSL.c:80-112

5.23 SocketPlugin nameToAddr drops AF_INET6 silently
     File: extracted/plugins/SocketPlugin/src/common/SocketPluginImpl.c:368-402

5.24 W^X violated permanently for JIT pages on Linux/FreeBSD
     File: src/memoryUnix.c:66-89, 109-111
     mmap is PROT_READ|PROT_WRITE|PROT_EXEC; the
     sqMakeMemoryExecutableFromTo / NotExecutable hooks are commented
     out.  Defeats W^X kernel protection — a single OOB write into the
     JIT region becomes arbitrary RCE.

5.25 LOW: parameters.c chdir(originalArgument) without sanitization
     File: src/parameters/parameters.c:574

5.26 FFI executeWorkerTask leaks WorkerTask
     File: ffi/src/worker/worker.c:210-219

5.27 FFI readString returns un-pinned image-memory pointer
     File: ffi/src/utils.c:43-50
     If GC moves the string between strlen() and strcpy() in
     callbackPrimitives, the lengths disagree.

================================================================
6. TLS CONFIGURATION (recap, beyond #2.9 above)
================================================================

6.1  Win SChannel: SP_PROT_TLS1_0/1_1/1_2 enabled for both client/server
     File: extracted/plugins/SqueakSSL/src/win/sqWin32SSL.c:216-218

6.2  macOS: SSLSetProtocolVersionMin(ctx, kTLSProtocol1)  (TLS 1.0 min)
     File: extracted/plugins/SqueakSSL/src/osx/sqMacSSL.c:154-164

6.3  Unix: no SSL_OP_NO_COMPRESSION (CRIME), no
     SSL_OP_CIPHER_SERVER_PREFERENCE, no SSL_OP_NO_RENEGOTIATION.
     File: extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c:107

6.4  Unix: serverName length is silently truncated by strnlen-based
     length passed to X509_check_host, so attacker-influenced long
     serverName causes "checked-vs-actual" mismatch.
     File: extracted/plugins/SqueakSSL/src/unix/sqUnixSSL.c:309-323

================================================================
7. BUILD / SUPPLY CHAIN / HARDENING
================================================================

CRITICAL  Jenkinsfile:84,249  — wget … | bash, plain HTTP
CRITICAL  scripts/runTests.sh:31  — wget -O - https://get.pharo.org/64/80 | bash,
                                     run from .github/workflows on PR
CRITICAL  scripts/installCygwin.ps1:7-9  — installer + mirror over plain HTTP
HIGH      cmake/import{LibFFI,LibGit2,SDL2}.cmake  — mutable git tags,
            no commit-SHA pin
HIGH      macros.cmake:69-103 + every cmake/import*.cmake using
            files.pharo.org  — DownloadProject without URL_HASH for
            libgit2, libssh2, openssl, zlib, SDL2, cairo, pixman,
            libpng, freetype, fontconfig, harfbuzz, gcc-runtime
HIGH      cmake/importFreetype2.cmake:47-49 — direct savannah download
            without URL_HASH
HIGH      docker/{ubuntu-arm64,debian10-armv7}/Dockerfile  — unpinned
            base image
HIGH      Jenkinsfile:97-403  — scp -o StrictHostKeyChecking=no
            (authenticated SSH to files.pharo.org, all upload paths)
MEDIUM    Jenkinsfile:138 + cmake/sign.cmake:11  — SIGN_CERT_PASSWORD
            via env, broad withCredentials() block
MEDIUM    .github/workflows/continuous-integration-workflow.yaml:2
            — `on: [push, pull_request]` with no `permissions:` block;
              PR from a fork can edit runTests.sh and run anything on
              the runner with the workflow's GITHUB_TOKEN scope
LOW       .github/workflows/...:11,14,68 — EOL runners (ubuntu-18.04,
            windows-2016) and EOL action versions (checkout@v1,
            upload-artifact@v1)
LOW       cmake/packaging.cmake:92  — CPACK_PACKAGE_CHECKSUM "SHA1"
LOW       scripts/installCygwin.ps1:35-48  — cygwin -q (suppresses
            signature warnings)

Hardening flags absent in default build:
  HIGH    No -D_FORTIFY_SOURCE=2, -fstack-protector-strong,
          -fPIE/-pie, -Wformat -Wformat-security, -Wl,-z,relro,
          -Wl,-z,now, -Wl,-z,noexecstack on UNIX/Linux releases.
          Many warnings actively silenced (-Wno-int-conversion,
          -Wno-pointer-sign) — these silence bug classes.
          File: CMakeLists.txt:206, 266-296; cmake/Linux.cmake:1
  MEDIUM  Linux rpath set to bare "." instead of "$ORIGIN" — relative
          to CWD, not the binary.  File: cmake/Linux.cmake:1
  MEDIUM  Windows/Cygwin: no /GS, no /guard:cf, no /DYNAMICBASE, no
          /NXCOMPAT.  File: CMakeLists.txt:206

================================================================
8. SUGGESTED PATCHING ORDER
================================================================

  1. Add `return 0;` after every primitiveFailFor in DSAPrims (#2.1, #2.2).
     One-line fixes that close two heap-write primitives.
  2. JPEG primJPEGReadImage* (§2.3): require formWidth/Height to match
     output_width/output_height.  Closes a remote-attacker primitive.
  3. SqueakSSL Unix: add SSL_CTX_set_verify + min protocol TLS1_2 +
     stricter cipher list (§2.9).  Closes the most-impactful network bug.
  4. UnixOSProcessPlugin realpathAsType (§3.1): use stack PATH_MAX buffer.
  5. FileAttributesPlugin readlink (§3.2): readlink(...,FA_PATH_MAX-1).
  6. sqNamedPrims `+ 1` (§3.26): one-character fix.
  7. MiscPrim primitiveDecompressFromByteArray (§3.3): bound `index >= 1`,
     check `isWords/isBitmap(bm)`.
  8. FFI typesPrimitives (§2.5, §2.6): tag-check incoming type oops; clear
     handler before free.
  9. threadSafeQueue (§3.21): take the mutex before reading queue->first.
 10. Build/CI: pin docker images and cmake URL_HASH for downloaded
     dependencies; remove curl|bash from runTests.sh; add `permissions:
     contents: read` to the workflow.

================================================================
9. CHANGES VS PRIOR AUDIT
================================================================

Confirmed still present and unfixed: SSL no-verify, SSL weak TLS/cipher,
SSL X509_NAME_get_text_by_NID retval-ignored, FFI primitiveUtils memcpy,
FFI callbackPrimitives malloc+strcpy, FFI stack-CallbackInvocation,
sqNamedPrims +1 off-by-one, supply-chain (wget|bash, scp -o
StrictHostKeyChecking=no, plain-HTTP GTK3, mutable tags, unpinned Docker,
SIGN_CERT_PASSWORD env, no hardening flags).

Demoted: "missing semicolons after checkFailed() macro" — the macro body
itself terminates with `;`, so the absence of an explicit trailing `;`
at call sites does not mask an early return.

Reclassified: aioWin.c:457/465 — the alias is between two heap regions
(an interior pointer into allHandles vs. a fresh malloc), not between
stack and heap.  No present free-of-stack-pointer hazard, but the
transient alias remains and would become exploitable if any code path
between the two assignments returns early.

Net delta: ~50 new findings ordered above; 0 prior findings are fixed.
