# pharo-version.sh -- shared Pharo release selection for the scripts.
# Source this file; do not run it.
#
#   PHARO_VERSION=140 scripts/bench-suite.sh      (default: 140, Pharo 14)
#   PHARO_VERSION=130 scripts/bench-suite.sh      (Pharo 13.1)
#   PHARO_VERSION=150 scripts/bench-suite.sh      (Pharo 15 alpha)
#
# Variables set (an existing value in the environment wins):
#   PHARO_VERSION      three digits: 130, 140, 150
#   PHARO_DOTTED       13.1, 14.0, 15.0 (used in labels and --pharo-version)
#   PHARO_SUFFIX       "" for 130 (keeps the old /tmp/harness name), 14 for 140, 15 for 150
#   PHARO_IMAGE_URL    get.pharo.org URL of the image download script
#   PHARO_VM_URL       get.pharo.org URL of the image plus stock VM download script
#   PHARO_VMONLY_URL   get.pharo.org URL of the stock VM only download script
#   PHARO_HARNESS      default harness directory, /tmp/harness<suffix>
#   PHARO_HARNESS_X86  default x86_64 (Rosetta) harness directory, /tmp/harness<suffix>-x86
#   PHARO_IMAGE_SIG    default image signature label for perfdb

PHARO_VERSION="${PHARO_VERSION:-140}"
case "$PHARO_VERSION" in
  130) PHARO_DOTTED=13.1; PHARO_SUFFIX="";  PHARO_IMAGE_SIG_DEFAULT="pharo13.1-45e803d" ;;
  140) PHARO_DOTTED=14.0; PHARO_SUFFIX=14;  PHARO_IMAGE_SIG_DEFAULT="pharo14.0" ;;
  150) PHARO_DOTTED=15.0; PHARO_SUFFIX=15;  PHARO_IMAGE_SIG_DEFAULT="pharo15.0" ;;
  *) echo "pharo-version.sh: unsupported PHARO_VERSION=$PHARO_VERSION (use 130, 140 or 150)" >&2
     return 2 2>/dev/null || exit 2 ;;
esac
PHARO_IMAGE_URL="https://get.pharo.org/64/${PHARO_VERSION}"
PHARO_VM_URL="https://get.pharo.org/64/${PHARO_VERSION}+vm"
PHARO_VMONLY_URL="https://get.pharo.org/64/vm${PHARO_VERSION}"
PHARO_HARNESS="${PHARO_HARNESS:-/tmp/harness${PHARO_SUFFIX}}"
PHARO_HARNESS_X86="${PHARO_HARNESS_X86:-/tmp/harness${PHARO_SUFFIX}-x86}"
PHARO_IMAGE_SIG="${PHARO_IMAGE_SIG:-$PHARO_IMAGE_SIG_DEFAULT}"
export PHARO_VERSION PHARO_DOTTED PHARO_SUFFIX PHARO_IMAGE_URL PHARO_VM_URL PHARO_VMONLY_URL PHARO_HARNESS PHARO_HARNESS_X86 PHARO_IMAGE_SIG
