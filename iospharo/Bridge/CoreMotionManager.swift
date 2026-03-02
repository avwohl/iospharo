/*
 * CoreMotionManager.swift — Polls CMMotionManager and writes samples
 * into the shared MotionData C struct for the VM to read.
 *
 * CMMotionManager must be created on the main thread and kept alive
 * for the lifetime of the updates. PharoBridge owns this instance.
 */

import CoreMotion
import Foundation

class CoreMotionManager: @unchecked Sendable {

    private let manager = CMMotionManager()
    private let queue = OperationQueue()
    private var polling = false

    init() {
        queue.name = "com.awohl.pharo.motion"
        queue.maxConcurrentOperationCount = 1

        // Report hardware availability immediately
        var initial = MotionData()
        if manager.isAccelerometerAvailable   { initial.available |= MOTION_HAS_ACCELEROMETER }
        if manager.isGyroAvailable            { initial.available |= MOTION_HAS_GYROSCOPE }
        if manager.isMagnetometerAvailable    { initial.available |= MOTION_HAS_MAGNETOMETER }
        if manager.isDeviceMotionAvailable    { initial.available |= MOTION_HAS_DEVICE_MOTION }
        motion_update(&initial)
    }

    /// Start device-motion updates at 60 Hz (includes fused accel+gyro+mag+attitude).
    func start() {
        guard !polling else { return }
        guard manager.isDeviceMotionAvailable else { return }

        polling = true
        manager.deviceMotionUpdateInterval = 1.0 / 60.0
        manager.showsDeviceMovementDisplay = false

        // Use xMagneticNorthZVertical if magnetometer available, otherwise xArbitraryZVertical
        let frame: CMAttitudeReferenceFrame = manager.isMagnetometerAvailable
            ? .xMagneticNorthZVertical
            : .xArbitraryZVertical

        manager.startDeviceMotionUpdates(using: frame, to: queue) { [weak self] motion, _ in
            guard let self, let motion else { return }
            self.publish(motion)
        }
    }

    /// Stop updates and zero out the active flag.
    func stop() {
        guard polling else { return }
        polling = false
        manager.stopDeviceMotionUpdates()

        // Mark inactive but keep the availability bits
        var data = MotionData()
        motion_getData(&data)
        data.active = 0
        motion_update(&data)
    }

    /// Check if the VM has requested start/stop via the C flags.
    /// Called periodically (e.g. from a Timer on the main run loop).
    func pollRequests() {
        if motion_startRequested() != 0 { start() }
        if motion_stopRequested() != 0  { stop()  }
    }

    // MARK: - Private

    private func publish(_ motion: CMDeviceMotion) {
        var data = MotionData()

        // User acceleration + gravity = total acceleration in G
        data.accelerometerX = motion.userAcceleration.x + motion.gravity.x
        data.accelerometerY = motion.userAcceleration.y + motion.gravity.y
        data.accelerometerZ = motion.userAcceleration.z + motion.gravity.z

        data.gyroX = motion.rotationRate.x
        data.gyroY = motion.rotationRate.y
        data.gyroZ = motion.rotationRate.z

        data.magnetometerX = motion.magneticField.field.x
        data.magnetometerY = motion.magneticField.field.y
        data.magnetometerZ = motion.magneticField.field.z

        data.roll  = motion.attitude.roll
        data.pitch = motion.attitude.pitch
        data.yaw   = motion.attitude.yaw

        data.timestamp = motion.timestamp

        // Preserve availability bits
        data.available = manager.isAccelerometerAvailable ? MOTION_HAS_ACCELEROMETER : 0
        if manager.isGyroAvailable         { data.available |= MOTION_HAS_GYROSCOPE }
        if manager.isMagnetometerAvailable { data.available |= MOTION_HAS_MAGNETOMETER }
        if manager.isDeviceMotionAvailable { data.available |= MOTION_HAS_DEVICE_MOTION }
        data.active = 1

        motion_update(&data)
    }
}
