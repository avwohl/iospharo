# Primitive Implementation Gaps

This document lists known gaps in primitive implementations identified during the audit.
Last updated: 2026-01-22

## Critical (Affects Core Functionality)

### Critical Section Primitives (185-187)
| Primitive | Name | Description |
|-----------|------|-------------|
| 185 | primitiveExitCriticalSection | Exit critical section, resume blocked processes |
| 186 | primitiveEnterCriticalSection | Enter critical section, may block if owned |
| 187 | primitiveTestAndSetOwnershipOfCriticalSection | Atomic test-and-set for locking |

- **Status**: Stubbed out (always return Failure)
- **Impact**: HIGH - Process synchronization using Mutex/Semaphore may not work correctly
- **Workaround**: Many uses of critical sections work via Smalltalk fallback code
- **Note**: Full implementation requires process scheduling integration

## High Priority (May Affect Some Features)

### SmallFloat Primitives (541-559)
- **Status**: Not implemented (nullptr in table)
- **Impact**: Low - Regular Float primitives handle SmallFloat operands via `extractFloat()` helper
- **Official VM**: Optimized paths when both operands are SmallFloat immediates
- **Our behavior**: Falls back to regular Float primitives, slightly slower but functionally correct

### Old Space / Pinned Allocation (596-599)
| Primitive | Name | Description |
|-----------|------|-------------|
| 596 | primitiveNewOldSpace | Create object directly in old space |
| 597 | primitiveNewWithArgOldSpace | Create sized object in old space |
| 598 | primitiveNewPinned | Create pinned (non-moving) object |
| 599 | primitiveNewWithArgPinned | Create sized pinned object |

- **Impact**: Medium - All objects go to default space
- **Workaround**: GC promotes objects to old space naturally

### Context/VM Introspection (213-218)
| Primitive | Name | Description |
|-----------|------|-------------|
| 213 | primitiveContextXray | Return context state flags |
| 214 | primitiveVoidVMState | Clear all VM state |
| 215 | primitiveVoidVMStateForMethod | Clear VM state for specific method |
| 216 | primitiveMethodXray | Return method metadata |
| 217 | primitiveMethodProfilingData | Get method profiling info |
| 218 | primitiveDoNamedPrimitiveWithArgs | Call named primitive with args array |

- **Impact**: Medium - Used by debuggers and development tools
- **Workaround**: Smalltalk fallback code handles most cases

## Medium Priority (Optional Features)

### External Plugin Management (571-573)
| Primitive | Name | Description |
|-----------|------|-------------|
| 571 | primitiveUnloadModule | Unload external plugin module |
| 572 | primitiveListBuiltinModule | List built-in modules |
| 573 | primitiveListExternalModule | List external modules |

- **Impact**: Low - We don't support external plugins currently
- **Note**: Primitive 570 (primitiveFlushExternalPrimitives) IS implemented

### Time/Timezone Variants (242-246)
| Primitive | Name | Description |
|-----------|------|-------------|
| 242 | primitiveSignalAtUTCMicroseconds | Timer semaphore at UTC time |
| 243 | primitiveUpdateTimezone | Update timezone info |
| 244 | primitiveUtcAndTimezoneOffset | Get UTC and timezone offset together |
| 245 | primitiveCoarseUTCMicrosecondClock | Fast (less precise) UTC clock |
| 246 | primitiveCoarseLocalMicrosecondClock | Fast local clock |

- **Impact**: Low - Primitives 240-241 (UTC/Local microsecond clocks) work
- **Workaround**: Use existing clock primitives

### VM Profiling (250-251, 253)
| Primitive | Name | Description |
|-----------|------|-------------|
| 250 | primitiveClearVMProfile | Clear profiling data |
| 251 | primitiveControlVMProfiling | Start/stop profiling |
| 253 | primitiveCollectCogCodeConstituents | Collect JIT code info |

- **Impact**: Low - Profiling is a development feature
- **Note**: Primitive 252 (primitiveVMProfileSamplesInto) IS implemented

### Become Variant (248)
| Primitive | Name | Description |
|-----------|------|-------------|
| 248 | primitiveArrayBecomeOneWayNoCopyHash | One-way become without hash copy |

- **Impact**: Low - Primitive 249 (with hash copy) is implemented
- **Workaround**: Use primitive 249 or primitive 72

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
