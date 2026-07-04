; iospharo Windows VM - NSIS installer
; Modeled on z80cpmw/packaging/nsis/z80cpmw.nsi, adapted to a PER-USER
; install (no admin/UAC: %LOCALAPPDATA%\Programs, HKCU registry) so the
; build can be installed/tested unattended and users need no elevation.
;
; Built + signed by build-installer.ps1 (stages files, signs inner
; binaries with the Azure Trusted Signing kit, runs makensis, signs the
; setup exe).  File list here MUST match the staging list in
; build-installer.ps1 — if you add a DLL there, add File + Delete lines
; here too (the uninstaller enumerates explicitly; a missed file leaves
; the install dir behind rather than risking a recursive delete).

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!define APPNAME "iospharo"
!define COMPANYNAME "Aaron Wohl"
!define DESCRIPTION "Pharo Smalltalk VM (clean C++ reimplementation with x86-64 JIT)"
!ifndef VERSIONMAJOR
  !define VERSIONMAJOR 0
  !define VERSIONMINOR 1
  !define VERSIONBUILD 0
!endif
!define HELPURL "https://github.com/avwohl/iospharo"
!define UPDATEURL "https://github.com/avwohl/iospharo/releases"
!define ABOUTURL "https://github.com/avwohl/iospharo"

Name "${APPNAME}"
OutFile "..\..\dist\iospharo-${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}-setup.exe"
Unicode true
InstallDir "$LOCALAPPDATA\Programs\${APPNAME}"
InstallDirRegKey HKCU "Software\${COMPANYNAME}\${APPNAME}" "InstallDir"
RequestExecutionLevel user

VIProductVersion "${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "CompanyName" "${COMPANYNAME}"
VIAddVersionKey "LegalCopyright" "Copyright (C) 2025-2026 ${COMPANYNAME}"
VIAddVersionKey "FileDescription" "${DESCRIPTION}"
VIAddVersionKey "FileVersion" "${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}"
VIAddVersionKey "ProductVersion" "${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}"

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "iospharo VM" SecMain
    SectionIn RO

    SetOutPath "$INSTDIR"

    ; VM executable (build-win\test_load_image.exe, staged + renamed —
    ; the binary is exe-name-agnostic; primVmPath uses the real path)
    File "staging\iospharo.exe"

    ; LLVM-MinGW runtime
    File "staging\libc++.dll"
    File "staging\libdl.dll"
    File "staging\libffi-8.dll"
    File "staging\libunwind.dll"
    File "staging\libwinpthread-1.dll"

    ; FreeType / Cairo font + graphics stack (vendored from the official
    ; Pharo 13 Windows VM distribution; see THIRD-PARTY-LICENSES.txt)
    File "staging\libfreetype-6.dll"
    File "staging\libpng16.dll"
    File "staging\zlib1.dll"
    File "staging\libbz2-1.dll"
    File "staging\libharfbuzz-0.dll"
    File "staging\libcairo-2.dll"
    File "staging\libpixman-1-0.dll"
    File "staging\libfontconfig-1.dll"
    File "staging\libexpat-1.dll"

    ; TFFI test fixture (needed by the image's TFUFFI* test suites)
    File "staging\TestLibrary.dll"

    ; SDL2.dll is a 255-byte TEXT MARKER, not a real DLL: it satisfies
    ; FFIWindowsLibraryFinder's existence probe of `Smalltalk vm directory`;
    ; the VM intercepts every SDL2 call with built-in stubs (FFI.cpp).
    ; Nothing may replace it with a real SDL2.
    File "staging\SDL2.dll"

    ; Docs
    File "staging\README.txt"
    File "staging\LICENSE.txt"
    File "staging\THIRD-PARTY-LICENSES.txt"

    ; Start menu
    CreateDirectory "$SMPROGRAMS\${APPNAME}"
    CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME} README.lnk" "$INSTDIR\README.txt"
    CreateShortCut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"

    ; Registry (per-user)
    WriteRegStr HKCU "Software\${COMPANYNAME}\${APPNAME}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayName" "${APPNAME}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "QuietUninstallString" "$\"$INSTDIR\uninstall.exe$\" /S"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayIcon" "$INSTDIR\iospharo.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "Publisher" "${COMPANYNAME}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "HelpLink" "${HELPURL}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "URLUpdateInfo" "${UPDATEURL}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "URLInfoAbout" "${ABOUTURL}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayVersion" "${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "VersionMajor" ${VERSIONMAJOR}
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "VersionMinor" ${VERSIONMINOR}
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "EstimatedSize" "$0"

    WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section ".image file association" SecFileAssoc
    ; Per-user association: double-click a Pharo .image to launch it
    ; interactively.  The VM must run with the image's directory as the
    ; working dir conventionally; the VM chdir()s to the image dir itself.
    WriteRegStr HKCU "Software\Classes\.image" "" "iospharo.PharoImage"
    WriteRegStr HKCU "Software\Classes\iospharo.PharoImage" "" "Pharo Smalltalk Image"
    WriteRegStr HKCU "Software\Classes\iospharo.PharoImage\DefaultIcon" "" "$INSTDIR\iospharo.exe,0"
    WriteRegStr HKCU "Software\Classes\iospharo.PharoImage\shell\open\command" "" '"$INSTDIR\iospharo.exe" "%1"'
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\iospharo.exe"
    Delete "$INSTDIR\libc++.dll"
    Delete "$INSTDIR\libdl.dll"
    Delete "$INSTDIR\libffi-8.dll"
    Delete "$INSTDIR\libunwind.dll"
    Delete "$INSTDIR\libwinpthread-1.dll"
    Delete "$INSTDIR\libfreetype-6.dll"
    Delete "$INSTDIR\libpng16.dll"
    Delete "$INSTDIR\zlib1.dll"
    Delete "$INSTDIR\libbz2-1.dll"
    Delete "$INSTDIR\libharfbuzz-0.dll"
    Delete "$INSTDIR\libcairo-2.dll"
    Delete "$INSTDIR\libpixman-1-0.dll"
    Delete "$INSTDIR\libfontconfig-1.dll"
    Delete "$INSTDIR\libexpat-1.dll"
    Delete "$INSTDIR\TestLibrary.dll"
    Delete "$INSTDIR\SDL2.dll"
    Delete "$INSTDIR\README.txt"
    Delete "$INSTDIR\LICENSE.txt"
    Delete "$INSTDIR\THIRD-PARTY-LICENSES.txt"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${APPNAME}\${APPNAME} README.lnk"
    Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
    RMDir "$SMPROGRAMS\${APPNAME}"

    DeleteRegKey HKCU "Software\${COMPANYNAME}\${APPNAME}"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
    DeleteRegKey HKCU "Software\Classes\iospharo.PharoImage"
    ; Only remove the .image mapping if it is still ours
    ReadRegStr $0 HKCU "Software\Classes\.image" ""
    StrCmp $0 "iospharo.PharoImage" 0 +2
    DeleteRegKey HKCU "Software\Classes\.image"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The iospharo VM and its runtime libraries (required)"
    !insertmacro MUI_DESCRIPTION_TEXT ${SecFileAssoc} "Open Pharo .image files with iospharo on double-click (per-user)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "iospharo requires 64-bit Windows."
        Abort
    ${EndIf}
FunctionEnd
