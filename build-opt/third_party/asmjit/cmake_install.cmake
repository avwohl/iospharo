# Install script for directory: /Users/wohl/src/iospharo/third_party/asmjit

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "RelWithDebInfo")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/wohl/src/iospharo/build-opt/third_party/asmjit/libasmjit.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libasmjit.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libasmjit.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libasmjit.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config.cmake"
         "/Users/wohl/src/iospharo/build-opt/third_party/asmjit/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit/asmjit-config.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/build-opt/third_party/asmjit/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/build-opt/third_party/asmjit/CMakeFiles/Export/51e8fad3bd3789d5315a1fbe99c5b64d/asmjit-config-relwithdebinfo.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/asmjit.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/asmjit-scope-begin.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/asmjit-scope-end.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/host.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/api-config.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/archtraits.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/archcommons.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/assembler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/builder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/codebuffer.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/codeholder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/compiler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/compilerdefs.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/constpool.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/cpuinfo.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/emitter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/environment.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/errorhandler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/fixup.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/formatter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/func.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/globals.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/inst.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/jitallocator.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/jitruntime.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/logger.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/operand.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/osutils.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/string.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/target.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/type.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/core" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/core/virtmem.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arena.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arenahash.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arenalist.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arenapool.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arenastring.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arenatree.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/arenavector.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/span.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/support" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/support/support.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/a64.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/armglobals.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/armutils.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64assembler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64builder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64compiler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64emitter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64globals.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64instdb.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/arm" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/arm/a64operand.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86assembler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86builder.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86compiler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86emitter.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86globals.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86instdb.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/x86" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/x86/x86operand.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/ujit.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/ujit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/ujit/ujitbase.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/ujit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/ujit/unicompiler.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/ujit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/ujit/unicondition.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/ujit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/ujit/uniop.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/asmjit/ujit" TYPE FILE FILES "/Users/wohl/src/iospharo/third_party/asmjit/asmjit/ujit/vecconsttable.h")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/wohl/src/iospharo/build-opt/third_party/asmjit/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
