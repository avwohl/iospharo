# Fresh Pharo 12 Reference Image

Use this image for testing to ensure no saved state persists.

## Image Details
- **Source**: https://files.pharo.org/image/120/latest-64.zip
- **File**: Pharo12.0-SNAPSHOT-64bit-2047df47d7.image
- **SHA256**: `33c86ce42b7786db464a80ad1de60a0542479c9ef2de59d5df4d998cbdcba7fc`
- **Location**: `/tmp/Pharo12-fresh/`

## Expected Submorphs (fresh image)
1. MenubarMorph
2. TaskbarMorph
3. SpWindow (Welcome window)
4. ImageMorph (Pharo logo)

## Usage
Copy to app bundle before testing:
```bash
APP_BUNDLE="/Users/wohl/Library/Developer/Xcode/DerivedData/iospharo-buxermqmwwmdxjbbgetrozfkwrod/Build/Products/Debug-maccatalyst/iospharo.app/Contents/Resources"
cp /tmp/Pharo12-fresh/Pharo12.0-SNAPSHOT-64bit-2047df47d7.image "$APP_BUNDLE/Pharo.image"
cp /tmp/Pharo12-fresh/Pharo12.0-SNAPSHOT-64bit-2047df47d7.changes "$APP_BUNDLE/Pharo.changes"
cp /tmp/Pharo12-fresh/Pharo12.0-64bit-2047df4.sources "$APP_BUNDLE/"
```

Verify SHA256 after copy:
```bash
shasum -a 256 "$APP_BUNDLE/Pharo.image"
# Should be: 33c86ce42b7786db464a80ad1de60a0542479c9ef2de59d5df4d998cbdcba7fc
```
