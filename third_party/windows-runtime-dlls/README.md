# Windows runtime DLLs (FreeType font stack)

Vendored from the official Pharo 13 Windows VM distribution
(get.pharo.org/64/130+vm). The image's font machinery loads FreeType via
FFI (`FT2Handle` looks for `freetype`/`libfreetype` next to the VM exe);
without it `FreeTypeFontProvider updateFromSystem` finds 0 families and
every `LogicalFont` silently falls back to the 14px bitmap StrikeFont
(discovered via `MicTextPresenterTest>>testHugeFontIsHuge`, 2026-07-03).

    libfreetype-6.dll    FreeType 2.12.1 (FTL license)
    libpng16.dll         libpng (PNG license) — freetype dep
    zlib1.dll            zlib (zlib license) — freetype dep
    libbz2-1.dll         bzip2 (BSD-like) — freetype dep
    libharfbuzz-0.dll    HarfBuzz (MIT) — freetype dep

Staged next to test_load_image.exe by a CMake POST_BUILD step
(CMakeLists.txt, "FreeType runtime" block). All licenses permit binary
redistribution.
