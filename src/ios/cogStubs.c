/*
 * cogStubs.c
 *
 * Comprehensive stub implementations for Cog JIT symbols on iOS.
 * iOS doesn't support JIT due to code signing restrictions,
 * so we provide no-op stubs for all Cog-related symbols.
 */

#include <stddef.h>

/* Stack/Frame pointers */
void* CFramePointer = NULL;
void* CStackPointer = NULL;

/* Build info */
const char* __cogitBuildInfo = "iOS interpreter-only (no JIT)";

/* Trampolines and entry points - all NULL for interpreter mode */
void* ceBaseFrameReturnTrampoline = NULL;
void* ceCall0ArgsPIC = NULL;
void* ceCall1ArgsPIC = NULL;
void* ceCall2ArgsPIC = NULL;
void* ceCallCogCodePopReceiverAndClassRegs = NULL;
void* ceCallCogCodePopReceiverArg0Regs = NULL;
void* ceCallCogCodePopReceiverArg1Arg0Regs = NULL;
void* ceCallCogCodePopReceiverReg = NULL;
void* ceCannotResumeTrampoline = NULL;
void* ceEnterCogCodePopReceiverReg = NULL;
void* ceReturnToInterpreterTrampoline = NULL;
void* ceTakeProfileSample = NULL;
void* ceTraceBlockActivationTrampoline = NULL;
void* ceTraceStoreTrampoline = NULL;
void* realCEEnterCogCodePopReceiverReg = NULL;
void* realCECallCogCodePopReceiverReg = NULL;
void* ceSendMustBeBooleanTrampoline = NULL;
void* ceFlushICache = NULL;
void* cePrimReturnEnterCogCode = NULL;
void* cePrimReturnEnterCogCodeProfiling = NULL;

/* Offsets */
int cbEntryOffset = 0;
int cbNoSwitchEntryOffset = 0;
int cmEntryOffset = 0;
int cmNoCheckEntryOffset = 0;
int missOffset = 0;
int mnuOffset = 0;
int numRegArgs = 0;

/* Cog method zone */
void* cogCodeBase = NULL;
void* cogMethodZone = NULL;
int cogMethodZoneSize = 0;
void* minCogMethodAddress = NULL;
void* maxCogMethodAddress = NULL;
void* cogCodeConstituents = NULL;

/* Method cache */
void* methodCache = NULL;
int methodCacheSize = 0;
void* lastMethodCacheProbeType = NULL;
int methodCacheEntryCount = 0;

/* Open PIC */
void* openPICList = NULL;
int openPICSize = 0;
void* cPICPrototype = NULL;
int cPICPrototypeSize = 0;

/* Young referrers */
void* youngReferrers = NULL;
int youngReferrersSize = 0;

/* Profile support */
int profilingFlag = 0;
void* profileSamples = NULL;
int numProfileSamples = 0;
int traceFlags = 0;
int traceLinkedSendOffset = 0;

/* Stub functions - Cog method management */
void addAllToYoungReferrers(void) {}
void addCogMethodsToHeapMap(void) {}
int bytecodePCForstartBcpcin(void* method, int startBcpc) { return startBcpc; }
void ceCaptureCStackPointers(void* fp, void* sp) { CFramePointer = fp; CStackPointer = sp; }
void* cogMethodSurrogateAt(void* address) { return NULL; }
int cogMethodZoneFreeSpace(void) { return 0; }
void compactCogCompiledCode(void) {}
void freeOldCogMethod(void* method) {}
void freePICForMethod(void* method) {}
void* getCogCodeZoneStart(void) { return NULL; }
void* getCogCodeZoneEnd(void) { return NULL; }
int inCogCodeZone(void* address) { return 0; }
void initializeCogMethodZone(void) {}
int isCogCompiledCode(void* address) { return 0; }
int isInCogMethodZone(void* address) { return 0; }
void* lookupAddress(void* thing) { return NULL; }
void mapFor(void* address) {}
void markMethodAndReferents(void* method) {}
int mcprimaliasaliasSize(void) { return 0; }
void* methodFor(void* thing) { return NULL; }
int numLocked(void) { return 0; }
void printOnTrace(void) {}
void printOpenPICList(void) {}
void recreateMethodHeaderCache(void) {}
int selectorIndexInLinkedSend(void* method) { return -1; }
void* selectorOfLinkedSendAt(void* address) { return NULL; }
void setCogCodeZoneStart(void* start) {}
void unlinkAllCallsIn(void* method) {}
void unlockAllCogMethods(void) {}
void updateObjectReferencesIn(void* method) {}
void voidCogMethod(void* method) {}
void voidOpenPIC(void* pic) {}
void addToYoungReferrers(void* referrer) {}
void removeFromYoungReferrers(void* referrer) {}
void markAndTraceYoungReferrers(void) {}
void checkAssertsEnabledIn(void* method) {}
void cPICHasFreedTargets(void* cpic) {}
void freeCPIC(void* cpic) {}
int hasCoggableClosures(void* method) { return 0; }
int isCoggable(void* method) { return 0; }
void lockMethod(void* method) {}
void unlockMethod(void* method) {}
void startProfiling(void) {}
void stopProfiling(void) {}
void clearProfile(void) {}
void* picDataFor(void* address) { return NULL; }
int picDataSizeFor(void* address) { return 0; }
void* frameOfMarriedContext(void* context) { return NULL; }
int isMarriedContext(void* context) { return 0; }
void marriedContextPut(void* context, void* frame) {}
void addToHeapMap(void* address) {}
void removeFromHeapMap(void* address) {}
int inHeapMap(void* address) { return 0; }
void flushExternalPrimitives(void) {}
void unlinkSendsLinkedForInvalidClasses(void) {}
void unlinkSendsOfisMNUSelector(void* selector, int isMNU) {}
void unlinkSendsToandFreeIf(void* method, int freeIf) {}
void unlinkAllSends(void) {}
void safeFreeMethod(void* method) {}
void setPostCompileHook(void* hook) {}
void setSelectorOfto(void* method, void* selector) {}
void findNewMethodInClassTag(void* class) {}
void primitiveMethodXray(void) {}
void freeUntracedCogMethods(void) {}
void printCogMethodZone(void) {}
void printCogCodeZone(void) {}
void clearMethodCache(void) {}
void clearMethodCacheForMethod(void* method) {}
void clearMethodCacheForSelector(void* selector) {}
void followForwardingPointersInMethodCache(void) {}
void* machineCodeFor(void* method) { return NULL; }
int hasMachineCode(void* method) { return 0; }
void generateMachineCodeFor(void* method) {}
void markReferencesFromMachineCode(void) {}
void compactMachineCode(void) {}
void printFrameAndCallers(void* frame) {}
void* getGCAction(void) { return NULL; }
void setGCAction(void* action) {}
void* cogFullBlockMethodnumCopied(void* method, int numCopied) { return NULL; }
void cogitPostGCAction(void) {}
void* cogselector(void* method) { return NULL; }
int defaultCogCodeSize(void) { return 0; }
void followForwardedLiteralsIn(void* method) {}
void followForwardedMethods(void) {}
void freeUnmarkedMachineCode(void) {}
void* genQuickReturnConst(void) { return NULL; }
void* genQuickReturnInstVar(void) { return NULL; }
void* genQuickReturnSelf(void) { return NULL; }
int getBlockCompilationCount(void) { return 0; }
int getBlockCompilationMSecs(void) { return 0; }
int getMethodCompilationCount(void) { return 0; }
int getMethodCompilationMSecs(void) { return 0; }
/* getVMGMTOffset is in heartbeat.c */
void initializeCodeZoneFromupTo(void* from, void* to) {}
int isSendReturnPC(void* pc) { return 0; }
void mapObjectReferencesInMachineCode(void) {}
void* mapPCDataForinto(void* method, void* buffer) { return NULL; }
void markAndTraceMachineCodeOfMarkedMethods(void) {}
int mcPCForBackwardBranchstartBcpcin(void* method, int startBcpc) { return 0; }
int numMethodsOfType(int type) { return 0; }
void* profilingDataForinto(void* method, void* buffer) { return NULL; }
void rewritePrimInvocationInto(void* method, void* target) {}

/* Send/PIC stubs - called from cointerp but need Cogit implementation */
void* cogMNUPICSelectorreceivermethodOperandnumArgs(void* sel, void* rcvr, void* methodOp, int numArgs) { return NULL; }
void linkSendAtintooffsetreceiver(void* addr, void* target, int offset, void* rcvr) {}
void* patchToOpenPICFornumArgsreceiver(void* method, int numArgs, void* rcvr) { return NULL; }
void recordCallOffsetIn(void* method) {}

/* Stack page configuration */
void setDesiredStackPageBytes(long long anInteger) {
	/* iOS uses default stack page size */
}

/* Time and heartbeat functions provided by heartbeat.c */

/* Surface plugin */
int ioFindSurface(int surfaceID, void** surface, void** fn) { return 0; }
int ioRegisterSurface(void* surface, void* fn, int* surfaceID) { return 0; }
int ioUnregisterSurface(int surfaceID) { return 0; }

/* Processor/clock functions provided by heartbeat.c */

/* File attributes stubs */
int faAccessAttributes(const char* path) { return 0; }
int faCloseDirectory(void* dir) { return 0; }
int faExists(const char* path) { return 0; }
int faFileAttribute(const char* path, int attribute) { return 0; }
int faFileStatAttributes(const char* path, void* attrs) { return 0; }
void* faOpenDirectory(const char* path) { return NULL; }
int faReadDirectory(void* dir, char* name, int nameLen) { return 0; }
int faRewindDirectory(void* dir) { return 0; }
int faSetPlatPathOop(void* oop) { return 0; }
int faSetStDir(void* dir) { return 0; }
int faSetStPath(const char* path) { return 0; }
