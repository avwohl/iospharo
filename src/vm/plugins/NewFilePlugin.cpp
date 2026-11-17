/*
 * NewFilePlugin.cpp - the "NewFilePlugin" named primitives (file and directory
 * access with explicit offsets and memory mapping) for the Pharo VM.
 *
 * The image package ronsaldo/pharo-newfile calls these through
 * <primitive: 'primitiveFileOpen' module: 'NewFilePlugin'> and so on.  The
 * behaviour follows the stock VM (pharo-vm, plugins/NewFilePlugin and
 * smalltalksrc/VMMaker/NewFilePlugin.class.st), with these differences, each
 * on purpose:
 *
 *  - The value in the ExternalAddress of a file handle or a directory handle
 *    is an id, not a C pointer.  The id comes from a 64-bit counter that
 *    starts above the user address space and is never used again in the
 *    process, so a copy of a closed handle can never match a later handle.
 *    A table maps each live id to its struct.  (The stock plugin keeps the C
 *    pointer and dereferences whatever the image passes back.)  The value
 *    that primitiveDirectoryNext answers is a real char* (the image reads the
 *    name through it), and the value that the map primitive answers is the
 *    real mapped address.
 *  - A NULL handle (a closed file or directory) answers what the stock plugin
 *    answers: close, rewind and unmap do nothing; directory next, map and a
 *    read or write give a NULL address or -1; size answers -1, tell answers
 *    0, truncate answers false.  A value that is neither NULL nor a live id
 *    fails the primitive.  Seek on a NULL handle also fails, because the stock
 *    plugin dereferences NULL there.
 *  - The buffer of a read or a write must be a data object with no pointers
 *    (byte, 16-bit, 32-bit or 64-bit format; not a compiled method), and
 *    offset + size, counted in bytes, must lie inside it.  A read into an
 *    immutable object fails.  (The stock plugin checks nothing.)
 *  - A path with an embedded NUL fails the primitive.
 *  - A bad mode, creation disposition, flag bit, seek mode or protection
 *    value fails the primitive.  A failed lseek() fails the seek primitive
 *    (for example ESPIPE for a pipe or a terminal; the stock plugin ignores
 *    the result of lseek()).
 *    (The stock plugin answers a null handle for a bad mode, treats a bad
 *    disposition like OpenExisting, and ignores the rest.)
 *  - The map primitive counts references.  A second map of a file answers the
 *    same address and counts a reference only when the file still has the
 *    mapped length and the mapping allows what the request asks for.
 *    Otherwise it answers a NULL address and counts nothing (the image treats
 *    NULL as "no mapping").  The stock plugin answers the old address.
 *  - Unmap drops one reference and unmaps at the last one.  (The stock plugin
 *    tests the count the wrong way round: it unmaps while references remain,
 *    and never at the last one.)  A view object of the image that outlives the unmap holds a
 *    dead address, as with any unmapped memory.  Closing a file that still has
 *    a mapping keeps the mapping record, and an unmap through a copy of the
 *    closed handle releases the mapping.  The image's own NewFile>>close
 *    nulls its handle, and an unmap through a NULL handle does nothing, so
 *    the mapping of that file lasts until the plugin shuts down (relaunch or
 *    exit).
 *  - newFilePluginShutdown() closes every tracked file and directory, unmaps
 *    every mapping and clears the table.  The proxy reset of a relaunch calls
 *    it.
 *  - primitiveFileClose and primitiveDirectoryClose set the handle object to
 *    NULL after the close.  (The image does that too.)
 *  - NewDirectory_open in the stock plugin passes the image's path without a
 *    terminating NUL to opendir(); this plugin uses the NUL-terminated copy.
 *  - open() adds O_CLOEXEC, and open, read and write repeat after EINTR.
 *  - A 64-bit argument or result must be a SmallInteger (61 bits).  A
 *    LargePositiveInteger offset or size fails the primitive.
 *
 * A handle is an ExternalAddress (8 bytes), as in the stock VM.  A failure of
 * the underlying system call that the stock plugin reports as a result (open
 * answers a null handle, read and write answer -1, getSize and tell answer
 * -1, create/delete/remove answer false) is reported the same way here.
 *
 * Every primitive reads its arguments from the stack, and changes the stack
 * only when it succeeds.  A primitive that allocates (open, directory next,
 * memory map) allocates first and then reads the stack again.  No primitive
 * keeps a pointer into a Smalltalk object across an allocation.  The
 * ExternalAddress objects are allocated by the interpreter proxy in old
 * space, so each open, directory next and map adds 16 bytes that only a full
 * GC frees.
 *
 * Memory mapped regions are outside the object memory, so the garbage
 * collector does not see them.
 *
 * All calls come from the interpreter thread.  The mutex guards the tables
 * only (against newFilePluginShutdown from the thread that starts a
 * relaunch), not the structs.
 *
 * Windows: the stock Win32 side is not ported.  Under _WIN32 every primitive,
 * primitiveIsAvailable included, fails, so the image falls back to its old
 * file classes (NewFile class>>primitiveIsAvailable answers false then).
 */

#include "sqVirtualMachine.h"

// InterpreterProxy.cpp: a data object with 8-, 16-, 32- or 64-bit elements.
extern "C" sqInt proxyIsRawBitsObject(sqInt oop);

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef _WIN32
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#endif

namespace {

VirtualMachine* vm = nullptr;

}  // namespace

#ifndef _WIN32

static_assert(sizeof(void*) == 8, "NewFilePlugin: an ExternalAddress holds 8 bytes");

namespace {

// Values the image sends (pharo-newfile NewFileConstants, same as the stock
// plugin header NewFile.h).
enum NewFileOpenMode {
    OpenModeReadOnly = 0,
    OpenModeWriteOnly = 1,
    OpenModeReadWrite = 2,
};
enum NewFileCreationDisposition {
    DispositionCreateNew = 1,
    DispositionCreateAlways = 2,
    DispositionOpenExisting = 3,
    DispositionOpenAlways = 4,
    DispositionTruncateExisting = 5,
};
enum NewFileOpenFlags {
    OpenFlagsNone = 0,
    OpenFlagsAppend = 1,
};
enum NewFileSeekMode {
    SeekModeSet = 0,
    SeekModeCurrent = 1,
    SeekModeEnd = 2,
};
enum NewFileMemMapProtection {
    MemMapReadOnly = 0,
    MemMapReadWrite = 1,
};

// SmallInteger range (61 bits).
const int64_t kSmallIntegerMax = (int64_t(1) << 60) - 1;
const int64_t kSmallIntegerMin = -(int64_t(1) << 60);

// An ExternalAddress holds one 64-bit value.
const size_t kHandleBytes = sizeof(uint64_t);

// Handle ids start above the user address space (and are never 0).
const uint64_t kFirstHandleId = UINT64_C(0xF000000000000001);

// ---------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------

bool smallIntegerArg(sqInt oop, int64_t& out) {
    if (!vm->isIntegerObject(oop)) return false;
    out = static_cast<int64_t>(vm->integerValueOf(oop));
    return true;
}

bool nonNegativeArg(sqInt oop, uint64_t& out) {
    int64_t v;
    if (!smallIntegerArg(oop, v) || v < 0) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

bool smallIntegerFits(int64_t v) {
    return v >= kSmallIntegerMin && v <= kSmallIntegerMax;
}

// Copy a path (a bytes object) to a NUL-terminated std::string.  Fails on a
// non-bytes object and on an embedded NUL.
bool pathArg(sqInt oop, std::string& out) {
    if (!vm->isBytes(oop)) return false;
    sqInt size = vm->byteSizeOf(oop);
    const char* bytes = static_cast<const char*>(vm->firstIndexableField(oop));
    if (size < 0 || (size > 0 && !bytes)) return false;
    if (size > 0 && std::memchr(bytes, 0, static_cast<size_t>(size)) != nullptr) return false;
    out.assign(bytes, static_cast<size_t>(size));
    return true;
}

// ---------------------------------------------------------------------
// Handle table
// ---------------------------------------------------------------------

struct Mapping {
    int count = 0;
    size_t length = 0;
    void* address = nullptr;
    bool writable = false;
};

struct NewFile {
    int fileDescriptor = -1;
    Mapping mapping;
};

struct NewDirectory {
    DIR* handle = nullptr;
};

std::mutex gHandleMutex;
uint64_t gNextHandleId = kFirstHandleId;
std::unordered_map<uint64_t, NewFile*> gFiles;
std::unordered_map<uint64_t, NewDirectory*> gDirectories;
// Mappings of files that are closed but not yet unmapped, by the id the file had.
std::unordered_map<uint64_t, Mapping> gClosedMappings;

uint64_t newHandleId() {
    // The caller holds no lock.
    std::lock_guard<std::mutex> lock(gHandleMutex);
    return gNextHandleId++;
}

enum class Lookup { Bad, Null, Unknown, Found };

// Read the id out of an ExternalAddress.  Fails (false) when the object is not
// an ExternalAddress of the handle size.
bool externalAddressValue(sqInt oop, uint64_t& out) {
    sqInt externalAddressClass = vm->classExternalAddress();
    if (externalAddressClass == vm->nilObject()) return false;
    if (!vm->isBytes(oop)) return false;
    if (vm->fetchClassOf(oop) != externalAddressClass) return false;
    if (static_cast<size_t>(vm->byteSizeOf(oop)) != kHandleBytes) return false;
    std::memcpy(&out, vm->firstIndexableField(oop), kHandleBytes);
    return true;
}

// Store a 64-bit value (an id or a real address) into an ExternalAddress.
void externalAddressStore(sqInt oop, uint64_t value) {
    std::memcpy(vm->firstIndexableField(oop), &value, kHandleBytes);
}

void externalAddressStorePointer(sqInt oop, const void* p) {
    externalAddressStore(oop, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)));
}

Lookup lookupFile(sqInt oop, NewFile*& file, uint64_t& id) {
    file = nullptr;
    id = 0;
    if (!externalAddressValue(oop, id)) return Lookup::Bad;
    if (id == 0) return Lookup::Null;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    auto it = gFiles.find(id);
    if (it == gFiles.end()) return Lookup::Unknown;
    file = it->second;
    return Lookup::Found;
}

Lookup lookupDirectory(sqInt oop, NewDirectory*& directory, uint64_t& id) {
    directory = nullptr;
    id = 0;
    if (!externalAddressValue(oop, id)) return Lookup::Bad;
    if (id == 0) return Lookup::Null;
    std::lock_guard<std::mutex> lock(gHandleMutex);
    auto it = gDirectories.find(id);
    if (it == gDirectories.end()) return Lookup::Unknown;
    directory = it->second;
    return Lookup::Found;
}

// Allocate an ExternalAddress.  Answers nil oop (and leaves the failure flag
// set) when allocation fails.  The object is not rooted: the caller must
// store into it before any other allocation.
sqInt newExternalAddress() {
    sqInt externalAddressClass = vm->classExternalAddress();
    if (externalAddressClass == vm->nilObject()) {
        vm->primitiveFail();
        return vm->nilObject();
    }
    sqInt oop = vm->instantiateClassindexableSize(externalAddressClass, kHandleBytes);
    if (vm->failed()) return vm->nilObject();
    if (!vm->isBytes(oop) || static_cast<size_t>(vm->byteSizeOf(oop)) != kHandleBytes) {
        vm->primitiveFail();
        return vm->nilObject();
    }
    return oop;
}

// Arguments: check the argument count the image used.
bool argCountIs(sqInt n) {
    return vm->methodArgumentCount() == n;
}

// Check that [offset, offset + size) lies inside the buffer object (counted in
// bytes).  The buffer is a data object with 8-, 16-, 32- or 64-bit elements.
// A read must not write into an immutable object.
bool bufferRange(sqInt bufferOop, uint64_t offset, uint64_t size, bool forRead, char*& start) {
    if (!vm->isNonImmediate(bufferOop)) return false;
    // Not a compiled method: a read into one would change its header word
    // and its literals.
    if (!proxyIsRawBitsObject(bufferOop)) return false;
    if (forRead && vm->isOopImmutable(bufferOop)) return false;
    sqInt byteSize = vm->byteSizeOf(bufferOop);
    if (byteSize < 0) return false;
    uint64_t length = static_cast<uint64_t>(byteSize);
    if (offset > length || size > length - offset) return false;
    char* base = static_cast<char*>(vm->firstIndexableField(bufferOop));
    if (!base && length > 0) return false;
    start = base + offset;
    return true;
}

// ---------------------------------------------------------------------
// The file and directory operations (after the stock UnixFile.c)
// ---------------------------------------------------------------------

NewFile* fileOpen(const std::string& path, int mode, int disposition, int flags) {
    int openFlags = 0;
    const mode_t openMode = 0644;
    switch (mode) {
    case OpenModeReadOnly:  openFlags = O_RDONLY; break;
    case OpenModeWriteOnly: openFlags = O_WRONLY; break;
    case OpenModeReadWrite: openFlags = O_RDWR; break;
    default: return nullptr;
    }
    if (flags & OpenFlagsAppend) openFlags |= O_APPEND;
    switch (disposition) {
    case DispositionCreateNew:         openFlags |= O_CREAT | O_EXCL; break;
    case DispositionCreateAlways:      openFlags |= O_CREAT | O_TRUNC; break;
    case DispositionOpenExisting:      break;
    case DispositionOpenAlways:        openFlags |= O_CREAT; break;
    case DispositionTruncateExisting:  openFlags |= O_TRUNC; break;
    default: return nullptr;
    }
    int fd;
    do {
        fd = open(path.c_str(), openFlags | O_CLOEXEC, openMode);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return nullptr;
    NewFile* file = new NewFile();
    file->fileDescriptor = fd;
    return file;
}

int64_t fileSize(NewFile* file) {
    struct stat s;
    if (fstat(file->fileDescriptor, &s) != 0) return -1;
    return static_cast<int64_t>(s.st_size);
}

// Answers the mapped address, or NULL (no reference counted) for an empty
// file, a failed mmap, a file whose size is no longer the mapped length, or a
// read/write request on a read-only mapping.
void* fileMemoryMap(NewFile* file, int protection) {
    Mapping& m = file->mapping;
    bool wantWrite = protection == MemMapReadWrite;
    int64_t size = fileSize(file);
    if (m.count > 0) {
        if (size < 0 || static_cast<size_t>(size) != m.length) return nullptr;
        if (wantWrite && !m.writable) return nullptr;
        ++m.count;
        return m.address;
    }
    if (size <= 0) return nullptr;
    int mmapProtection = wantWrite ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* address = mmap(nullptr, static_cast<size_t>(size), mmapProtection, MAP_SHARED,
                         file->fileDescriptor, 0);
    if (address == MAP_FAILED) return nullptr;
    m.address = address;
    m.length = static_cast<size_t>(size);
    m.writable = wantWrite;
    m.count = 1;
    return address;
}

void releaseMapping(Mapping& m) {
    if (m.address) munmap(m.address, m.length);
    m = Mapping();
}

// Drop one reference; the last one unmaps.
void unreferenceMapping(Mapping& m) {
    if (m.count <= 0) return;
    if (--m.count == 0) releaseMapping(m);
}

}  // namespace

// Close every tracked file and directory, unmap every mapping and clear the
// table.  Called from the proxy reset of a VM relaunch.  The id counter keeps
// counting, so an id is never used again.
void newFilePluginShutdown() {
    std::lock_guard<std::mutex> lock(gHandleMutex);
    for (auto& entry : gFiles) {
        NewFile* file = entry.second;
        releaseMapping(file->mapping);
        close(file->fileDescriptor);
        delete file;
    }
    gFiles.clear();
    for (auto& entry : gDirectories) {
        closedir(entry.second->handle);
        delete entry.second;
    }
    gDirectories.clear();
    for (auto& entry : gClosedMappings) releaseMapping(entry.second);
    gClosedMappings.clear();
}

#else  // _WIN32

void newFilePluginShutdown() {}

#endif

// =====================================================================
// Primitives
// =====================================================================

extern "C" sqInt NewFilePlugin_setInterpreter(VirtualMachine* anInterpreter) {
    vm = anInterpreter;
    return 0;
}

// primitiveIsAvailable  (0 args) -> true
extern "C" sqInt nfp_primitiveIsAvailable(void) {
#ifdef _WIN32
    return vm->primitiveFail();
#else
    if (!argCountIs(0)) return vm->primitiveFail();
    return vm->methodReturnValue(vm->trueObject());
#endif
}

#ifdef _WIN32

#define NFP_UNSUPPORTED(name) \
    extern "C" sqInt nfp_##name(void) { return vm->primitiveFail(); }
NFP_UNSUPPORTED(primitiveDeleteFile)
NFP_UNSUPPORTED(primitiveDirectoryClose)
NFP_UNSUPPORTED(primitiveDirectoryCreate)
NFP_UNSUPPORTED(primitiveDirectoryNext)
NFP_UNSUPPORTED(primitiveDirectoryOpen)
NFP_UNSUPPORTED(primitiveDirectoryRemoveEmpty)
NFP_UNSUPPORTED(primitiveDirectoryRewind)
NFP_UNSUPPORTED(primitiveFileClose)
NFP_UNSUPPORTED(primitiveFileGetSize)
NFP_UNSUPPORTED(primitiveFileOpen)
NFP_UNSUPPORTED(primitiveFileReadInto)
NFP_UNSUPPORTED(primitiveFileReadIntoAtFileOffset)
NFP_UNSUPPORTED(primitiveFileSeek)
NFP_UNSUPPORTED(primitiveFileTell)
NFP_UNSUPPORTED(primitiveFileTruncate)
NFP_UNSUPPORTED(primitiveFileWriteFrom)
NFP_UNSUPPORTED(primitiveFileWriteFromAtFileOffset)
NFP_UNSUPPORTED(primitiveMemoryMapWithProtection)
NFP_UNSUPPORTED(primitiveMemoryUnmap)
#undef NFP_UNSUPPORTED

#else  // POSIX

// primitiveDeleteFile  (1 arg: path bytes) -> Boolean (unlink succeeded)
extern "C" sqInt nfp_primitiveDeleteFile(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    std::string path;
    if (!pathArg(vm->stackValue(0), path)) return vm->primitiveFail();
    bool ok = unlink(path.c_str()) == 0;
    return vm->methodReturnBool(ok);
}

// primitiveDirectoryCreate  (1 arg: path bytes) -> Boolean (mkdir succeeded)
extern "C" sqInt nfp_primitiveDirectoryCreate(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    std::string path;
    if (!pathArg(vm->stackValue(0), path)) return vm->primitiveFail();
    bool ok = mkdir(path.c_str(), 0755) == 0;
    return vm->methodReturnBool(ok);
}

// primitiveDirectoryRemoveEmpty  (1 arg: path bytes) -> Boolean (rmdir succeeded)
extern "C" sqInt nfp_primitiveDirectoryRemoveEmpty(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    std::string path;
    if (!pathArg(vm->stackValue(0), path)) return vm->primitiveFail();
    bool ok = rmdir(path.c_str()) == 0;
    return vm->methodReturnBool(ok);
}

// primitiveDirectoryOpen  (1 arg: path bytes) -> ExternalAddress holding the
// directory handle id; holding NULL when opendir failed.
extern "C" sqInt nfp_primitiveDirectoryOpen(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    std::string path;
    if (!pathArg(vm->stackValue(0), path)) return vm->primitiveFail();

    // Allocate first (the path is already copied).
    sqInt result = newExternalAddress();
    if (vm->failed()) return vm->primitiveFail();

    uint64_t id = 0;
    DIR* dir = opendir(path.c_str());
    if (dir) {
        NewDirectory* directory = new NewDirectory();
        directory->handle = dir;
        id = newHandleId();
        std::lock_guard<std::mutex> lock(gHandleMutex);
        gDirectories[id] = directory;
    }
    externalAddressStore(result, id);
    return vm->methodReturnValue(result);
}

// primitiveDirectoryRewind  (1 arg: directory handle) -> receiver.  A NULL
// handle does nothing (as in the stock plugin).
extern "C" sqInt nfp_primitiveDirectoryRewind(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    NewDirectory* directory;
    uint64_t id;
    switch (lookupDirectory(vm->stackValue(0), directory, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found:   rewinddir(directory->handle); break;
    }
    vm->pop(1);  // leave the receiver
    return 0;
}

// primitiveDirectoryNext  (1 arg: directory handle) -> ExternalAddress holding
// a char* to the next entry name (valid until the next call or the close), or
// NULL at the end of the directory and for a NULL handle.
extern "C" sqInt nfp_primitiveDirectoryNext(void) {
    if (!argCountIs(1)) return vm->primitiveFail();

    // Allocate first; then read the handle from the stack.
    sqInt result = newExternalAddress();
    if (vm->failed()) return vm->primitiveFail();

    NewDirectory* directory;
    uint64_t id;
    const char* name = nullptr;
    switch (lookupDirectory(vm->stackValue(0), directory, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found: {
        struct dirent* entry = readdir(directory->handle);
        if (entry) name = entry->d_name;
        break;
    }
    }
    externalAddressStorePointer(result, name);
    return vm->methodReturnValue(result);
}

// primitiveDirectoryClose  (1 arg: directory handle) -> receiver.  The handle
// object is set to NULL.  A NULL handle does nothing (as in the stock plugin).
extern "C" sqInt nfp_primitiveDirectoryClose(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    sqInt handleOop = vm->stackValue(0);
    NewDirectory* directory;
    uint64_t id;
    switch (lookupDirectory(handleOop, directory, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found: {
        {
            std::lock_guard<std::mutex> lock(gHandleMutex);
            gDirectories.erase(id);
        }
        closedir(directory->handle);
        delete directory;
        externalAddressStore(handleOop, 0);
        break;
    }
    }
    vm->pop(1);  // leave the receiver
    return 0;
}

// primitiveFileOpen  (4 args: path bytes, mode, creationDisposition, flags)
// -> ExternalAddress holding the file handle id; holding NULL when open() failed.
extern "C" sqInt nfp_primitiveFileOpen(void) {
    if (!argCountIs(4)) return vm->primitiveFail();
    uint64_t flags, disposition, mode;
    std::string path;
    if (!nonNegativeArg(vm->stackValue(0), flags)) return vm->primitiveFail();
    if (!nonNegativeArg(vm->stackValue(1), disposition)) return vm->primitiveFail();
    if (!nonNegativeArg(vm->stackValue(2), mode)) return vm->primitiveFail();
    if (!pathArg(vm->stackValue(3), path)) return vm->primitiveFail();
    if (flags & ~static_cast<uint64_t>(OpenFlagsAppend)) return vm->primitiveFail();
    if (mode > OpenModeReadWrite) return vm->primitiveFail();
    if (disposition < DispositionCreateNew || disposition > DispositionTruncateExisting)
        return vm->primitiveFail();

    sqInt result = newExternalAddress();
    if (vm->failed()) return vm->primitiveFail();

    uint64_t id = 0;
    NewFile* file = fileOpen(path, static_cast<int>(mode), static_cast<int>(disposition),
                             static_cast<int>(flags));
    if (file) {
        id = newHandleId();
        std::lock_guard<std::mutex> lock(gHandleMutex);
        gFiles[id] = file;
    }
    externalAddressStore(result, id);
    return vm->methodReturnValue(result);
}

// primitiveFileClose  (1 arg: file handle) -> receiver.  The handle object is
// set to NULL.  A NULL handle does nothing (as in the stock plugin).  A live
// memory mapping stays (see the file header).
extern "C" sqInt nfp_primitiveFileClose(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    sqInt handleOop = vm->stackValue(0);
    NewFile* file;
    uint64_t id;
    switch (lookupFile(handleOop, file, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found: {
        {
            std::lock_guard<std::mutex> lock(gHandleMutex);
            gFiles.erase(id);
            if (file->mapping.count > 0) gClosedMappings[id] = file->mapping;
        }
        close(file->fileDescriptor);
        delete file;
        externalAddressStore(handleOop, 0);
        break;
    }
    }
    vm->pop(1);  // leave the receiver
    return 0;
}

// primitiveFileGetSize  (1 arg: file handle) -> Integer (-1 when fstat fails
// and for a NULL handle)
extern "C" sqInt nfp_primitiveFileGetSize(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    NewFile* file;
    uint64_t id;
    int64_t size = -1;
    switch (lookupFile(vm->stackValue(0), file, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found:   size = fileSize(file); break;
    }
    if (!smallIntegerFits(size)) return vm->primitiveFail();
    return vm->methodReturnValue(vm->integerObjectOf(static_cast<sqInt>(size)));
}

// primitiveFileTell  (1 arg: file handle) -> Integer (-1 when lseek fails, 0
// for a NULL handle)
extern "C" sqInt nfp_primitiveFileTell(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    NewFile* file;
    uint64_t id;
    int64_t position = 0;
    switch (lookupFile(vm->stackValue(0), file, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found:
        position = static_cast<int64_t>(lseek(file->fileDescriptor, 0, SEEK_CUR));
        break;
    }
    if (!smallIntegerFits(position)) return vm->primitiveFail();
    return vm->methodReturnValue(vm->integerObjectOf(static_cast<sqInt>(position)));
}

// primitiveFileSeek  (3 args: file handle, offset (signed), seekMode)
// -> receiver.  Fails when the mode is unknown, lseek() fails, or the handle
// is NULL (the stock plugin dereferences NULL there).
extern "C" sqInt nfp_primitiveFileSeek(void) {
    if (!argCountIs(3)) return vm->primitiveFail();
    uint64_t seekMode;
    int64_t offset;
    if (!nonNegativeArg(vm->stackValue(0), seekMode)) return vm->primitiveFail();
    if (!smallIntegerArg(vm->stackValue(1), offset)) return vm->primitiveFail();
    NewFile* file;
    uint64_t id;
    if (lookupFile(vm->stackValue(2), file, id) != Lookup::Found) return vm->primitiveFail();
    int whence;
    switch (seekMode) {
    case SeekModeSet:     whence = SEEK_SET; break;
    case SeekModeCurrent: whence = SEEK_CUR; break;
    case SeekModeEnd:     whence = SEEK_END; break;
    default: return vm->primitiveFail();
    }
    if (lseek(file->fileDescriptor, static_cast<off_t>(offset), whence) < 0)
        return vm->primitiveFail();
    vm->pop(3);  // leave the receiver
    return 0;
}

// primitiveFileTruncate  (2 args: file handle, new size) -> Boolean (false for
// a NULL handle)
extern "C" sqInt nfp_primitiveFileTruncate(void) {
    if (!argCountIs(2)) return vm->primitiveFail();
    uint64_t newSize;
    if (!nonNegativeArg(vm->stackValue(0), newSize)) return vm->primitiveFail();
    if (newSize > static_cast<uint64_t>(INT64_MAX)) return vm->primitiveFail();
    NewFile* file;
    uint64_t id;
    bool ok = false;
    switch (lookupFile(vm->stackValue(1), file, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found:
        ok = ftruncate(file->fileDescriptor, static_cast<off_t>(newSize)) == 0;
        break;
    }
    return vm->methodReturnBool(ok);
}

// Shared by the four read/write primitives.
//   withOffset: false -> args (file, buffer, bufferOffset, size);
//               true  -> args (file, buffer, bufferOffset, size, fileOffset).
// Answers the byte count, or -1 when the system call failed or the handle is NULL.
static sqInt readOrWrite(bool isWrite, bool withOffset) {
    sqInt argc = withOffset ? 5 : 4;
    if (!argCountIs(argc)) return vm->primitiveFail();
    int slot = 0;
    uint64_t fileOffset = 0, size, bufferOffset;
    if (withOffset && !nonNegativeArg(vm->stackValue(slot++), fileOffset))
        return vm->primitiveFail();
    if (!nonNegativeArg(vm->stackValue(slot++), size)) return vm->primitiveFail();
    if (!nonNegativeArg(vm->stackValue(slot++), bufferOffset)) return vm->primitiveFail();
    sqInt bufferOop = vm->stackValue(slot++);
    NewFile* file;
    uint64_t id;
    switch (lookupFile(vm->stackValue(slot++), file, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    return vm->methodReturnValue(vm->integerObjectOf(-1));
    case Lookup::Found:   break;
    }
    if (withOffset && fileOffset > static_cast<uint64_t>(INT64_MAX)) return vm->primitiveFail();
    char* buffer = nullptr;
    if (!bufferRange(bufferOop, bufferOffset, size, !isWrite, buffer)) return vm->primitiveFail();

    // No allocation happens between taking the buffer pointer and the call.
    ssize_t n;
    do {
        if (withOffset) {
            n = isWrite ? pwrite(file->fileDescriptor, buffer, static_cast<size_t>(size),
                                 static_cast<off_t>(fileOffset))
                        : pread(file->fileDescriptor, buffer, static_cast<size_t>(size),
                                static_cast<off_t>(fileOffset));
        } else {
            n = isWrite ? write(file->fileDescriptor, buffer, static_cast<size_t>(size))
                        : read(file->fileDescriptor, buffer, static_cast<size_t>(size));
        }
    } while (n < 0 && errno == EINTR);
    return vm->methodReturnValue(vm->integerObjectOf(static_cast<sqInt>(n)));
}

// primitiveFileReadInto  (4 args: file, buffer, bufferOffset, size) -> bytes read or -1
extern "C" sqInt nfp_primitiveFileReadInto(void) { return readOrWrite(false, false); }

// primitiveFileReadIntoAtFileOffset  (5 args: file, buffer, bufferOffset, size, fileOffset)
extern "C" sqInt nfp_primitiveFileReadIntoAtFileOffset(void) { return readOrWrite(false, true); }

// primitiveFileWriteFrom  (4 args: file, buffer, bufferOffset, size) -> bytes written or -1
extern "C" sqInt nfp_primitiveFileWriteFrom(void) { return readOrWrite(true, false); }

// primitiveFileWriteFromAtFileOffset  (5 args: file, buffer, bufferOffset, size, fileOffset)
extern "C" sqInt nfp_primitiveFileWriteFromAtFileOffset(void) { return readOrWrite(true, true); }

// primitiveMemoryMapWithProtection  (2 args: file handle, protection 0=read
// only 1=read/write) -> ExternalAddress holding the mapped address; holding
// NULL for an empty file, a failed mmap(), a NULL handle, a second map after
// the file changed size, or a read/write request on a read-only mapping (no
// reference is counted then).
extern "C" sqInt nfp_primitiveMemoryMapWithProtection(void) {
    if (!argCountIs(2)) return vm->primitiveFail();
    uint64_t protection;
    if (!nonNegativeArg(vm->stackValue(0), protection)) return vm->primitiveFail();
    if (protection > MemMapReadWrite) return vm->primitiveFail();

    sqInt result = newExternalAddress();
    if (vm->failed()) return vm->primitiveFail();

    NewFile* file;
    uint64_t id;
    void* address = nullptr;
    switch (lookupFile(vm->stackValue(1), file, id)) {
    case Lookup::Bad:
    case Lookup::Unknown: return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found:   address = fileMemoryMap(file, static_cast<int>(protection)); break;
    }
    externalAddressStorePointer(result, address);
    return vm->methodReturnValue(result);
}

// primitiveMemoryUnmap  (1 arg: file handle) -> receiver.  Drops one map
// reference; the last one unmaps.  A NULL handle, and a file without a
// mapping, do nothing (as in the stock plugin).  The id of a closed file that
// still has a mapping releases that mapping.
extern "C" sqInt nfp_primitiveMemoryUnmap(void) {
    if (!argCountIs(1)) return vm->primitiveFail();
    NewFile* file;
    uint64_t id;
    switch (lookupFile(vm->stackValue(0), file, id)) {
    case Lookup::Bad:     return vm->primitiveFail();
    case Lookup::Null:    break;
    case Lookup::Found:   unreferenceMapping(file->mapping); break;
    case Lookup::Unknown: {
        std::lock_guard<std::mutex> lock(gHandleMutex);
        auto it = gClosedMappings.find(id);
        if (it == gClosedMappings.end()) return vm->primitiveFail();
        unreferenceMapping(it->second);
        if (it->second.count <= 0) gClosedMappings.erase(it);
        break;
    }
    }
    vm->pop(1);  // leave the receiver
    return 0;
}

#endif  // POSIX
