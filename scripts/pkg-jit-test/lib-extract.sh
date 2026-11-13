# lib-extract.sh -- sourced by run-pkg-jit-test.sh and run-manifest.sh.
#
# Read the lines that run_pkg_tests.st prints (CLASSES, RUN, FAIL, ERR, TIMEOUT,
# RESULT) out of a VM log.  The logs are not clean text:
#   - tests write to stdout without a newline, so a runner line can be glued to
#     the end of other output ("...23 msRESULT pass=2356 ...");
#   - some output has CR line endings;
#   - our VM echoes the image's stdout again with an "[IMAGE] " prefix, so the
#     same line can appear twice, the second time glued to the first.
# So every extractor turns CR into LF and matches the line text anywhere in the
# line with grep -o, instead of anchoring on the start of a line.  Results are
# deduplicated, and sorted with LC_ALL=C so that comm gets one sort order.

# pkg_result LOG: the RESULT line, or nothing
pkg_result() {
    tr '\r' '\n' < "$1" 2>/dev/null \
      | grep -aoE 'RESULT pass=[0-9]+ fail=[0-9]+ err=[0-9]+ timeout=[0-9]+ classes=[0-9]+' \
      | tail -1
}

# pkg_classes LOG: the CLASSES line, or nothing
pkg_classes() {
    tr '\r' '\n' < "$1" 2>/dev/null | grep -aoE 'CLASSES [0-9]+' | tail -1
}

# pkg_fails LOG: sorted unique "KIND Class>>selector" lines (KIND is FAIL, ERR or TIMEOUT)
pkg_fails() {
    # Stock Cog writes a WideString (a test message with a character above 255)
    # to stdout as 4 bytes per character, so the whole FAIL line is NUL-padded
    # and the grep below misses it.  Delete the NULs first.
    tr -d '\000' < "$1" 2>/dev/null | tr '\r' '\n' \
      | grep -aoE '(FAIL|ERR|TIMEOUT) +[A-Za-z0-9_]+>>[A-Za-z0-9_:]+' \
      | sed -E 's/ +/ /' \
      | LC_ALL=C sort -u
}

# pkg_last_run LOG: "Class>>selector" of the last RUN line, or "none"
pkg_last_run() {
    local r
    r="$(tr '\r' '\n' < "$1" 2>/dev/null \
      | grep -aoE 'RUN [A-Za-z0-9_]+>>[A-Za-z0-9_:]+' | tail -1 | sed 's/^RUN //')"
    printf '%s' "${r:-none}"
}

# pkg_result_cell LOG EXITFILE: the RESULT line, or
# "NO-RESULT last-RUN=<Class>>><selector> exit=<status>" when the VM gave none
pkg_result_cell() {
    local r ex
    r="$(pkg_result "$1")"
    if [ -n "$r" ]; then printf '%s' "$r"; return; fi
    ex="$(cat "$2" 2>/dev/null || true)"
    printf 'NO-RESULT last-RUN=%s exit=%s' "$(pkg_last_run "$1")" "${ex:-unknown}"
}
