# Primitive Implementation Gaps

This document lists known gaps in primitive implementations identified during the audit.
Last updated: 2026-01-22

## ~~Critical (Affects Core Functionality)~~ ✅ RESOLVED

### ~~Critical Section Primitives (185-187)~~ ✅ IMPLEMENTED
| Primitive | Name | Description |
|-----------|------|-------------|
| 185 | primitiveExitCriticalSection | Exit critical section, resume blocked processes |
| 186 | primitiveEnterCriticalSection | Enter critical section, may block if owned |
| 187 | primitiveTestAndSetOwnershipOfCriticalSection | Atomic test-and-set for locking |

- **Status**: IMPLEMENTED (2026-01-22)
- Supports reentrant locking (same process can enter multiple times)
- Blocking enter suspends process and switches to highest priority runnable
- Non-blocking test-and-set returns nil if owned by another process

## High Priority (May Affect Some Features)

### ~~SmallFloat Primitives (541-559)~~ ✅ IMPLEMENTED
- **Status**: IMPLEMENTED (2026-01-22)
- Delegates to existing Float primitives (which already handle SmallFloat via extractFloat)
- Covers: add, subtract, comparisons, multiply, divide, truncated, fractionalPart,
  exponent, timesTwoPower, sqrt, sin, arctan, logN, exp

### ~~Old Space / Pinned Allocation (596-599)~~ ✅ IMPLEMENTED
| Primitive | Name | Description |
|-----------|------|-------------|
| 596 | primitiveNewOldSpace | Create object directly in old space |
| 597 | primitiveNewWithArgOldSpace | Create sized object in old space |
| 598 | primitiveNewPinned | Create pinned (non-moving) object |
| 599 | primitiveNewWithArgPinned | Create sized pinned object |

- **Status**: IMPLEMENTED (2026-01-22)
- OldSpace variants allocate normally (GC promotes to old space naturally)
- Pinned variants allocate and set pinned flag via pinObject()

### ~~Context/VM Introspection (213-218)~~ ✅ IMPLEMENTED
| Primitive | Name | Description |
|-----------|------|-------------|
| 213 | primitiveContextXray | Return context state flags |
| 214 | primitiveVoidVMState | Clear all VM state |
| 215 | primitiveVoidVMStateForMethod | Clear VM state for specific method |
| 216 | primitiveMethodXray | Return method metadata |
| 217 | primitiveMethodProfilingData | Get method profiling info |
| 218 | primitiveDoNamedPrimitiveWithArgs | Call named primitive with args array |

- **Status**: IMPLEMENTED (2026-01-22)
- primitiveContextXray (213): Returns 0 for heap contexts (no JIT marriage tracking)
- primitiveVoidVMState (214): Flushes method cache
- primitiveVoidVMStateForMethod (215): Succeeds (cache naturally replaced)
- primitiveMethodXray (216): Fails (Cog JIT-specific)
- primitiveMethodProfilingData (217): Fails (Cog JIT-specific)
- primitiveDoNamedPrimitiveWithArgs (218): Fails (let Smalltalk handle)

## Medium Priority (Optional Features)

### External Plugin Management (571-573)
| Primitive | Name | Description |
|-----------|------|-------------|
| 571 | primitiveUnloadModule | Unload external plugin module |
| 572 | primitiveListBuiltinModule | List built-in modules |
| 573 | primitiveListExternalModule | List external modules |

- **Impact**: Low - We don't support external plugins currently
- **Note**: Primitive 570 (primitiveFlushExternalPrimitives) IS implemented

### ~~Time/Timezone Variants (242-246)~~ ✅ IMPLEMENTED
| Primitive | Name | Description |
|-----------|------|-------------|
| 242 | primitiveSignalAtUTCMicroseconds | Timer semaphore at UTC time |
| 243 | primitiveUpdateTimezone | Update timezone info |
| 244 | primitiveUtcAndTimezoneOffset | Get UTC and timezone offset together |
| 245 | primitiveCoarseUTCMicrosecondClock | Fast (less precise) UTC clock |
| 246 | primitiveCoarseLocalMicrosecondClock | Fast local clock |

- **Status**: IMPLEMENTED (2026-01-22)
- Coarse clocks return current time (no heartbeat caching optimization)
- primitiveUpdateTimezone calls tzset()

### ~~VM Profiling (250-251, 253)~~ ✅ IMPLEMENTED
| Primitive | Name | Description |
|-----------|------|-------------|
| 250 | primitiveClearVMProfile | Clear profiling data |
| 251 | primitiveControlVMProfiling | Start/stop profiling |
| 253 | primitiveCollectCogCodeConstituents | Collect JIT code info |

- **Status**: IMPLEMENTED (2026-01-22)
- primitiveClearVMProfile (250): Succeeds (no-op in interpreter)
- primitiveControlVMProfiling (251): Succeeds (no-op in interpreter)
- primitiveCollectCogCodeConstituents (253): Fails (Cog JIT-specific)

### ~~Become Variant (248)~~ ✅ IMPLEMENTED
| Primitive | Name | Description |
|-----------|------|-------------|
| 248 | primitiveArrayBecomeOneWayNoCopyHash | One-way become without hash copy |

- **Status**: IMPLEMENTED (2026-01-22)
- Like primitive 249 but never copies identity hash from source to target

## Low Priority (Rarely Used)

### SIMD Operations (574)
| Primitive | Name | Description |
|-----------|------|-------------|
| 574 | primitiveFloat64ArrayAdd | SIMD float64 array addition |

- **Impact**: Very low - Performance optimization only

### ~~FFI Byte Access (600-659)~~ ✅ IMPLEMENTED
60 primitives for low-level FFI memory access:
- Load operations: boolean, int8/16/32/64, uint8/16/32/64, float32/64, char8/16/32, pointer
- Store operations: corresponding types
- **Status**: IMPLEMENTED (2026-01-22)

## Design Issues (Not Bugs)

### Quick Primitive Slots (264-267)
Our table has iOS-specific functions at slots 264-267:
- 264: primitiveGetNextEvent
- 265: primitiveInputSemaphore2
- 267: primitiveSampledSound

Official VM uses these as quick primitives (instance variable accessors).

- **Impact**: None currently - interpreter handles quick primitives separately
- **Risk**: Could cause issues if code inspects primitive table directly

## iOS-Specific Primitives (Unregistered)

Many iOS-specific primitives are implemented but not registered in the table.
These need slot assignments before they can be used:

### Sensors
- Accelerometer: AccelerometerRead, AccelerometerStart, AccelerometerStop
- Gyroscope: GyroscopeRead, GyroscopeStart, GyroscopeStop
- Magnetometer: MagnetometerRead, MagnetometerStart, MagnetometerStop
- Location: LocationRead, LocationStart, LocationStop
- Device Motion: DeviceMotionRead

### Camera
- CameraCapture, CameraClose, CameraCount, CameraGetFrame, CameraOpen
- CameraSetExposure, CameraSetFlash, CameraSetFocus
- CameraStartPreview, CameraStopPreview

### App Integration
- Biometric: BiometricAuthenticate, BiometricAvailable
- IAP: IAPPurchase, IAPGetProducts, IAPFinishTransaction, IAPGetReceipt
- Notifications: NotificationSchedule, NotificationCancel, NotificationGetPending
- Social: ShareFile, ShareImage, ShareText, ShareURL, SocialPost
- URL: OpenURL, CanOpenURL

These need to be assigned to primitive slots >= 600 (external primitive range)
or use the named primitive mechanism.
