# Plan: Pharo Smalltalk on Bare Metal / Containers

Smalltalk is the environment -- windows, mouse, menus, overlapping windows --
that Steve Jobs saw at Xerox PARC and brought to the Macintosh. Smalltalk is
mostly written in Smalltalk: the compiler, debugger, UI framework, process
scheduler, and even device drivers (in some implementations) are all Smalltalk
objects. The VM is just a thin execution engine.

This document explores what it would take to run Pharo Smalltalk directly on
bare hardware (no operating system), in a container/unikernel, or on a
lightweight RTOS.


## What Does the VM Actually Need from the OS?

Our iospharo C++ VM uses these OS services:

    Service             Where in our code                OS API used
    ------------------- -------------------------------- -------------------------
    Heap allocation     ObjectMemory.cpp:54              mmap(MAP_ANONYMOUS)
    Heap deallocation   ObjectMemory.cpp:31              munmap()
    Small allocations   ObjectMemory.cpp:46              aligned_alloc / malloc
    Image load/save     ImageLoader.cpp, ImageWriter.cpp std::ifstream/ofstream
    File I/O prims      Primitives.cpp:10859+            fopen/fread/fwrite/fclose
    Directory listing   Primitives.cpp:11164+            opendir/readdir/stat
    Timers              Interpreter.cpp:866              std::thread (heartbeat)
    Wall clock          InterpreterProxy.cpp:998         gettimeofday
    Calendar time       Primitives.cpp:14550+            time/localtime/gmtime
    Sleep               Primitives.cpp:12009             usleep
    Sockets             plugins/SocketPlugin.cpp         socket/connect/send/recv
                                                         select/fcntl/pipe
    DNS                 Primitives.cpp:24960             getaddrinfo (in thread)
    Dynamic loading     FFI.cpp:270+                     dlopen/dlsym
    Threading           Interpreter.cpp, SocketPlugin    std::thread/mutex/atomic
    Display             DisplaySurface.hpp               abstracted (Metal on iOS)
    Events              EventQueue.hpp                   abstracted (UIKit on iOS)
    Sound               plugins/SoundPlugin.cpp          AudioToolbox (Apple)
    MIDI                plugins/MIDIPlugin.cpp           CoreMIDI (Apple)
    Text rendering      WorldRenderer.cpp                CoreText/CoreGraphics
    Run loop            Interpreter.cpp:732              CFRunLoopRunInMode

The display and event layers are already cleanly abstracted -- DisplaySurface
and EventQueue are interfaces that any platform can implement. The Apple-specific
code (Metal, CoreText, AudioToolbox, CoreMIDI) is isolated in platform files.


## Existing Bare Metal Smalltalk Projects

### SqueakNOS (2001-2006)
The original: Squeak running directly on x86 hardware. Had 1024x768x32
graphics, keyboard, mouse. Abandoned twice -- driver development in Smalltalk
was slow and the VM lacked JIT, making performance poor.

### CogNOS / Nopsys (2011-present, active)
The successor to SqueakNOS. The key insight was separating concerns:
  - nopsys (~3,000 lines C/ASM): boot stub, interrupts, paging, minimal libc
  - CogNOS: the Cog VM adapted for bare metal

Runs the full Cog VM with JIT on bare x86 hardware. Compatible with Pharo,
Squeak, and Cuis images. Has FAT32 filesystem, networking (ICMP demonstrated).
Runs on QEMU, VirtualBox, VMware. Latest release: v0.2 (October 2025).

Published paper: "Self-Contained Development Environments" (DLS '18)
GitHub: https://github.com/nopsys/nopsys and https://github.com/nopsys/CogNOS

### Crosstalk (2020)
Bare-metal Smalltalk-80 on Raspberry Pi by Michael Engel. Uses the Circle
bare-metal library for RPi (USB, framebuffer, interrupts). HDMI at 1920x1080,
USB keyboard/mouse. No persistent storage.
GitHub: https://github.com/michaelengel/crosstalk

### Key Lessons
1. The C/ASM substrate is small: 2,000-5,000 lines gets you booting
2. Drivers are the hard part: USB HID, disk, networking add thousands of lines
3. Virtual machines (QEMU) simplify everything: virtio devices are much simpler
4. The Smalltalk image handles most complexity internally


## How Much C/ASM for Bare Metal?

Component estimates for x86-64 (based on nopsys and OSDev patterns):

    Component                                     Lines of C/ASM
    --------------------------------------------- ---------------
    Boot + CPU init (GRUB multiboot, long mode)   ~400
    Memory manager (page allocator, paging, mmap)  ~1,000
    Interrupt handling (IDT, PIC, ISR stubs)       ~630
    PS/2 keyboard driver                           ~300
    PS/2 mouse driver                              ~200
    Framebuffer (VESA via GRUB, blitting)          ~450
    Minimal libc (memcpy, sprintf, malloc, stdio)  ~1,500
    Timer / clock (PIT, TSC, RTC)                  ~350
    --------------------------------------------- ---------------
    TOTAL (graphical, RAM-disk image)              ~4,800

    Optional additions:
    Disk + FAT32 filesystem                        ~2,500
    Networking (NIC driver + lwIP integration)     ~2,000 + ~40K lwIP
    USB HID (keyboard/mouse)                       ~3,500

For context, nopsys itself is ~3,000 lines and provides everything needed to
boot and run the Cog VM.


## Is That Too Much C/ASM? The RTOS Question

If we need thousands of lines of C/ASM anyway, should we steal from or use an
existing lightweight RTOS?

### NuttX -- Best Fit (recommended)
Apache licensed, the most POSIX-compliant RTOS. Has stdio, pthreads, sockets,
mmap, FAT filesystem, TCP/IP networking, and a simulator mode. Most of our VM
code would compile unchanged.

    Custom code needed:  ~500-1,000 lines (board support + VM integration)
    Why:                 POSIX layer means our code compiles as-is

### Zephyr RTOS -- Viable
Linux Foundation project. Growing POSIX layer, recently added mmap/mprotect
support. Virtual memory on MMU-equipped platforms. Broad hardware support.

    Custom code needed:  ~1,000-2,000 lines
    Why:                 POSIX layer is optional and still incomplete

### seL4 -- Overkill
Formally verified microkernel. Provides only IPC, scheduling, and capability
management. You'd build a full OS on top. Only justified if security
certification is the primary goal.

    Custom code needed:  ~5,000-10,000 lines
    Why:                 Essentially building your own OS

### FreeRTOS -- Too Minimal
Designed for microcontrollers without MMU. No mmap, no virtual memory, partial
POSIX (34 of 99 pthread functions). The Pharo VM needs more than this provides.

    Custom code needed:  ~5,000-8,000 lines
    Why:                 Wrong target class

### Verdict
NuttX lets us compile the VM nearly unchanged. If we want bare metal with
minimal work, NuttX is the answer. If we want the bare-metal-Smalltalk
experience (no OS at all), fork CogNOS/nopsys -- it's proven for exactly this.


## The JIT Question

Our iospharo VM is interpreter-only because Apple forbids JIT on iPad. On bare
metal, there is no such restriction. This matters a lot for performance:

    Execution mode              Relative speed
    --------------------------- ----------------
    Interpreter (our VM)        1x (baseline)
    Cog JIT                     3-15x faster
    Sista (speculative inline)  10-45x faster

The Cog JIT's OS requirements are minimal:
  - mmap(MAP_ANONYMOUS) to allocate the code zone (~1-2 MB)
  - mprotect() to toggle pages between RW and RX
  - Optionally, dual mapping (two virtual addresses for same physical pages)
    to satisfy W^X policies

On bare metal, ALL of these are trivial: you control the page tables directly.
No W^X enforcement to work around. You can map a region as RWX, or set up dual
mappings by pointing two page table entries at the same physical frame. This is
actually SIMPLER than the OS-hosted case.

### Options for JIT on Bare Metal

Option A: Use the official Cog VM (recommended for bare metal)
  - CogNOS already proves this works
  - Get the full JIT, mature GC, all the Pharo infrastructure
  - Lose our iOS-specific low-bit oop encoding
  - The Cog VM supports x86-64 and ARM64 JIT backends

Option B: Add JIT to our C++ VM
  - Multi-person-year project (Cog JIT is ~30-50K lines of Slang/C)
  - Would preserve our oop encoding
  - Not practical unless we have a compelling reason

Option C: Keep interpreting on bare metal
  - Works today (just port our VM)
  - 3-15x slower than it could be
  - Fine for demos, not for real development work


## The Unikernel / Container Path (Fastest to Production)

Instead of true bare metal, run the VM on a unikernel inside a hypervisor
(QEMU/KVM, Firecracker, cloud VMs). This gives bare-metal-like performance
with zero custom code.

### OSv -- Best Unikernel Fit
BSD licensed. Single address space, no syscall overhead (read() is just a
function call). Full POSIX compatibility -- runs unmodified Linux binaries.
Boots in ~3-5ms on Firecracker. 6-7 MB OS overhead. Built-in network stack
and filesystem.

    Custom code: NONE. Compile the VM, package into OSv image, run.
    JIT support: Yes (mmap/mprotect work)

### Nanos (NanoVMs) -- Production Quality
Apache licensed. Runs unmodified Linux ELF binaries. Deployable to AWS, GCP,
Azure, or local QEMU/KVM. The "ops" tool handles image building:

    ops run ./pharo-vm --args /path/to/Pharo.image

    Custom code: NONE.
    JIT support: Yes (mmap/mprotect confirmed in test suite)

### Unikraft -- Most Configurable
BSD licensed. Modular construction kit with 160+ system calls. Binary
compatible with Linux ELF. Docker integration via runu runtime.

    Custom code: Low (testing needed to verify syscall coverage)
    JIT support: Likely (Redis/NGINX compatibility suggests mmap works)

### Docker Alpine (Already Exists)
The Pharo community already has Docker-Alpine images for headless Pharo.
~12.5 MB Docker image, production-ready for server-side Pharo today.

    Custom code: NONE.
    JIT support: Yes (full Linux, Cog VM works)

### Comparison

    Approach        Boot time   OS size   Custom code   JIT    Networking
    --------------- ----------- --------- ------------- ------ ----------
    Docker+Linux    ~1-2s       ~50-100MB None          Yes    Yes
    OSv             ~3-5ms      ~7MB      None          Yes    Yes
    Nanos           ~5-10ms     ~15MB     None          Yes    Yes
    Unikraft        ~1-5ms      ~5MB      Low           Likely Yes
    NuttX RTOS      instant     ~500KB    ~1K lines     Maybe  Yes
    nopsys (bare)   instant     ~50KB     ~3K lines     Yes    Partial
    From scratch    instant     ~20KB     ~5-12K lines  Yes    Hard


## Recommendations

For IMMEDIATE use (server/cloud Pharo):
  Use Docker Alpine or package the Pharo VM into an OSv/Nanos unikernel.
  Zero custom code. Works today.

For a RESEARCH project (bare-metal Smalltalk):
  Fork CogNOS/nopsys. The ~3,000-line substrate is proven. Adapt it for
  Pharo 13 images and the official Cog VM for full JIT performance.

For MAXIMUM performance on bare metal:
  Use the official Cog VM (not our interpreter) with CogNOS. The 3-15x JIT
  speedup is substantial, and bare metal eliminates W^X complexity.

For a PRACTICAL middle ground:
  NuttX RTOS. Compile our VM with minimal changes, get a real-time OS with
  POSIX compatibility, networking, filesystem, and a tiny footprint.

What I would NOT recommend:
  Building a bare-metal substrate from scratch. Nopsys exists, Circle exists
  for ARM, and OSv/Nanos exist for cloud deployment. The ~5-12K lines of x86
  boot code, interrupt handlers, and drivers is not justified when proven
  solutions exist.


## Architecture Diagram

    True Bare Metal (nopsys)            Unikernel (OSv/Nanos)
    +-----------------------+           +-----------------------+
    | Pharo Image           |           | Pharo Image           |
    | (compiler, debugger,  |           | (compiler, debugger,  |
    |  UI, scheduler...)    |           |  UI, scheduler...)    |
    +-----------------------+           +-----------------------+
    | Cog VM (JIT + interp) |           | Pharo VM (any)        |
    +-----------------------+           +-----------------------+
    | nopsys (~3K lines)    |           | OSv/Nanos (~7MB)      |
    | boot, IRQ, paging,    |           | POSIX, virtio drivers |
    | PS/2, framebuffer     |           | network stack, FS     |
    +-----------------------+           +-----------------------+
    | x86 hardware / QEMU   |           | KVM/QEMU hypervisor   |
    +-----------------------+           +-----------------------+

    RTOS (NuttX)                        Docker
    +-----------------------+           +-----------------------+
    | Pharo Image           |           | Pharo Image           |
    +-----------------------+           +-----------------------+
    | Our C++ VM            |           | Cog VM (official)     |
    +-----------------------+           +-----------------------+
    | NuttX RTOS            |           | Alpine Linux (~5MB)   |
    | POSIX, TCP/IP, FAT    |           | full POSIX, glibc     |
    +-----------------------+           +-----------------------+
    | Hardware / QEMU       |           | Docker / containerd   |
    +-----------------------+           +-----------------------+
