# Preparing a Pharo Image for iOS

This directory contains the OSWindow driver for iOS. Follow these steps to prepare a Pharo image for use with the iOS VM.

## Quick Setup

1. **Download Pharo** (10 or later)
   ```bash
   curl https://get.pharo.org/100+vm | bash
   ./pharo-ui Pharo.image
   ```

2. **File in the iOS driver**
   - Open a File Browser (Tools > File Browser)
   - Navigate to this directory
   - Select `OSiOSDriver.st` and click "FileIn"

3. **Run the installation script**
   - Open a Playground (Browse > Playground)
   - File in `install-ios-driver.st` or run:
   ```smalltalk
   OSiOSDriver install.
   ```

4. **Save the image**
   ```smalltalk
   Smalltalk saveSession.
   ```

5. **Copy to iOS app**
   - Copy `Pharo.image` and `Pharo.changes` to the iOS app bundle
   - Or place them where the iOS app expects to find them

## How It Works

The `OSiOSDriver` integrates with Pharo's OSWindow framework:

- **OSiOSDriver** - Registers as an OSWindow driver
- **OSiOSBackendWindow** - Represents the iOS display surface
- **OSiOSFormRenderer** - Handles Form-to-display rendering

It uses these VM primitives:
- Primitive 102: `beDisplay` - Register a Form as the display
- Primitive 103: `forceDisplayUpdate` - Flush Form bits to screen
- Primitive 106: `screenSize` - Get display dimensions

## Testing on Desktop

The iOS driver will only activate when running on the iOS VM (where primitive 106 returns valid dimensions). On desktop Pharo, the normal SDL2 driver will be used.

## Troubleshooting

**Black screen on iOS:**
- Ensure `OSiOSDriver.st` was filed in correctly
- Check that `OSiOSDriver isSupported` returns true
- Try: `OSWindowDriver current: OSiOSDriver new`

**Driver not activating:**
- Run: `OSiOSDriver install`
- Verify: `OSWindowDriver current class` should be `OSiOSDriver`

## Files

- `OSiOSDriver.st` - The iOS OSWindow driver classes
- `install-ios-driver.st` - Installation helper script
- `iOSWorldRenderer.st` - Legacy renderer (deprecated, use OSiOSDriver instead)
