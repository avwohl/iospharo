#!/usr/bin/env python3
"""Report whether a Spur image is multi-segment, without booting a VM.

Our ImageLoader walks the image data as ONE contiguous object sequence and
relocates every pointer by a single (newBase - oldBase) delta, so a
multi-segment image mis-parses at the first bridge and leaves everything past
it holding saved-image addresses (docs/vm-compat-bugs.md #4).  loadHeapData
refuses such images; this tells you up front which ones those are.

The test is `firstSegmentBytes < imageBytes`.  It is NOT "are there bytes past
the declared data" — segments live INSIDE imageBytes, separated by bridge
objects, which is why that earlier test wrongly cleared the Ume image.

    scripts/image-segments.py <image> [<image> ...]

exit 0 = all single-segment, 1 = at least one multi-segment.
"""
import os
import struct
import sys

# SpurImageHeader (src/vm/ImageLoader.hpp): imageFormat u32, headerSize u32,
# imageBytes u64, startOfMemory u64, specialObjectsOop u64, lastHash u64,
# screenSize u64, imageHeaderFlags u64, extraVMMemory u32, numStackPages u16,
# cogCodeSize u16, edenBytes u32, maxExtSemTabSize u16, imageVersion u16,
# firstSegmentBytes u64.
OFF_IMAGE_BYTES = 8
OFF_FIRST_SEGMENT = 4 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 4 + 2 + 2 + 4 + 2 + 2


def check(path):
    with open(path, 'rb') as f:
        head = f.read(128)
    if len(head) < 128:
        print(f'{path}: too small to be a Spur image')
        return None
    image_bytes = struct.unpack_from('<Q', head, OFF_IMAGE_BYTES)[0]
    first_segment = struct.unpack_from('<Q', head, OFF_FIRST_SEGMENT)[0]
    multi = 0 < first_segment < image_bytes
    print('%-40s %8.1f MB  imageBytes=%-12d firstSegment=%-12d %s'
          % (os.path.basename(path), os.path.getsize(path) / 1048576.0,
             image_bytes, first_segment,
             'MULTI-SEGMENT (this VM refuses it)' if multi else 'single'))
    return multi


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    any_multi = False
    for path in argv:
        result = check(path)
        any_multi = any_multi or bool(result)
    return 1 if any_multi else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
