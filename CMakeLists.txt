# SPDX-License-Identifier: GPL-2.0
#
# NTFS for macOS: user-space port of the Linux 7.1 fs/ntfs driver.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#   UBSan is on by default; -DNTFS_ASAN=ON adds ASan, which hangs on macOS 26.
#
# Targets:
#   ntfsplatform   platform/src/*.c + platform/pagecache/pagecache.c  (static)
#   ntfscore       core/ntfs/*.c [+ core/vfs/*.c + core/logfile/*.c] (static)
#   build/libntfscore.a   the two merged, what fskit/ and tools/ link
#   tests          platform/tests, platform/pagecache/tests (ctest)
cmake_minimum_required(VERSION 3.20)
# Universal binary on macOS: the Xcode app and extension build for arm64
# and x86_64 (fskit/Config/Shared.xcconfig ARCHS), and they link
# build/libntfscore.a directly, so the core library must contain both
# slices. Default to both when the user has not set
# CMAKE_OSX_ARCHITECTURES explicitly (an explicit -D still wins). The old
# default was arm64 only, which is why Intel Macs could never run the app
# (GAPS.md §4.2). A plain cmake under Rosetta would otherwise default to
# x86_64 only, which is the same trap in the other direction.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures" FORCE)
endif()
project(ntfsport C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)	# -std=gnu11

# Sanitizers, split because they are not equally usable here.
#
# UBSan works and is cheap, so it is ON by default for the tests: this port does
# a lot of unaligned reads out of packed on-disk structures, and UBSan is what
# catches that class. Turning it off is the unusual choice, not turning it on.
#
# ASan HANGS on macOS 26 / Apple silicon. It deadlocks inside
# __asan::AsanInitInternal() before main() ever runs, so the process produces no
# output and never exits -- confirmed 2026-09-14 on every test in this project,
# not just one. One runaway sat burning a core for 23 hours before anyone
# noticed, and because the logfile suite ran ASan by default its 243 checks had
# silently not executed for a day. So ASan is opt-in, and anyone who turns it on
# should expect that and check the toolchain first. It is worth retrying after a
# Xcode update: ASan is what originally found the work-queue use-after-free and
# the lookup-versus-invalidate race (docs/progress/platform-review.md).
#
# ASan DOES run on Linux, and since 2026-11-26 it runs in CI on macos-26:
# the dedicated `asan` job in .github/workflows/ci.yml runs `tools/ci.sh
# --asan` (stage_asan: own build-asan dir, asserts the suite executed), and
# tools/pre-push.sh stage 9 runs the same stage locally. First run found
# three leaks (extent-inode $MFT folio, two pagecache test double-gets,
# eight logfile test buffers) -- see docs/GAPS.md §5.5. The macos-26 job
# is also the re-test of the 2026-09-14 macOS hang (below): the user asked
# 2026-11-26 for the hang to be fixed, not skipped. If the job goes green,
# the hang is gone on current Xcode and the stage can join the default
# ci.sh sequence.
#
# Both default OFF, and that is a constraint rather than a preference: this
# build tree produces libntfscore.a, which the Xcode app links directly
# (NTFS_CORE_MODE=real). An instrumented static library needs the sanitizer
# runtime on the final link line, which the app does not provide and must not
# ship, so turning a sanitizer on here breaks the app build. Use a second build
# directory instead, which is what tools/ci.sh does:
#
#   cmake -S . -B build-ubsan -DNTFS_UBSAN=ON && ctest --test-dir build-ubsan
#
#   -DNTFS_UBSAN=ON      UBSan (works; on in CI)
#   -DNTFS_ASAN=ON       add ASan (runs in CI on macos-26 via tools/ci.sh
#                         --asan; the 2026-09-14 macOS hang is under
#                         re-test there -- see above and GAPS.md §5.5)
#   -DNTFS_SANITIZE=ON   both, the old spelling, kept working
option(NTFS_SANITIZE "Deprecated: same as -DNTFS_ASAN=ON -DNTFS_UBSAN=ON" OFF)
option(NTFS_UBSAN "Build with UndefinedBehaviorSanitizer (use a separate build dir)" OFF)
option(NTFS_ASAN "Build the tests with AddressSanitizer (Linux-only: hangs on macOS 26)" OFF)
option(NTFS_BUILD_TESTS "Build the unit tests" ON)
option(NTFS_FUZZ "Build the libFuzzer target (Clang only; use a separate build dir)" OFF)
if(NTFS_FUZZ AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "NTFS_FUZZ needs Clang (libFuzzer is Clang-only); configure with CC=clang")
endif()

set(NTFS_C_FLAGS -Wall -Wextra -Wno-unused-parameter -Werror=implicit-function-declaration
    -fno-strict-aliasing -fwrapv)
add_compile_options(${NTFS_C_FLAGS})
if(NTFS_SANITIZE)
  set(NTFS_ASAN ON)
  set(NTFS_UBSAN ON)
endif()
set(NTFS_SAN_LIST "")
if(NTFS_ASAN)
  list(APPEND NTFS_SAN_LIST address)
endif()
if(NTFS_UBSAN)
  list(APPEND NTFS_SAN_LIST undefined)
endif()
if(NTFS_SAN_LIST)
  list(JOIN NTFS_SAN_LIST "," NTFS_SAN_ARG)
  message(STATUS "sanitizers: ${NTFS_SAN_ARG}")
  add_compile_options(-fsanitize=${NTFS_SAN_ARG} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${NTFS_SAN_ARG})
  # Report rather than continue: an unaligned load that "works" today is still a
  # bug, and a test that prints a warning and passes is a test nobody reads.
  add_compile_options(-fno-sanitize-recover=undefined)
else()
  message(STATUS "sanitizers: none")
endif()

if(NTFS_FUZZ)
  # libFuzzer coverage instrumentation on everything, driver libraries
  # included: without coverage signal from driver code the fuzzer is
  # blind mutation and catches nothing inside the parser. address+undefined
  # is what catches the crafted-image heap overflows this harness exists
  # for. The link line gets address+undefined only: -fsanitize=fuzzer on
  # the link is what pulls in libFuzzer's main, and that belongs on the
  # ntfs_fuzz target alone (see the tests section below), or every
  # executable in this build would gain a second main.
  add_compile_options(-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# ---- platform: the linux-compat layer --------------------------------------
file(GLOB NTFS_PLATFORM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/src/*.c)
add_library(ntfsplatform STATIC
  ${NTFS_PLATFORM_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/pagecache/pagecache.c)
target_include_directories(ntfsplatform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
target_link_libraries(ntfsplatform PUBLIC Threads::Threads)

# ---- core: the ported driver -----------------------------------------------
add_subdirectory(core)

# ---- merged archive: build/libntfscore.a -----------------------------------
# Apple libtool is macOS-only and does not exist on Linux (GNU libtool is a
# different tool with a different CLI). On non-Apple, merge with GNU ar's
# MRI script via tools/merge-static-libs.sh.
set(NTFS_MERGED_LIB ${CMAKE_BINARY_DIR}/libntfscore.a)
if(APPLE)
  add_custom_command(
    OUTPUT ${NTFS_MERGED_LIB}
    COMMAND libtool -static -o ${NTFS_MERGED_LIB} $<TARGET_FILE:ntfscore> $<TARGET_FILE:ntfsplatform>
    DEPENDS ntfscore ntfsplatform $<TARGET_FILE:ntfscore> $<TARGET_FILE:ntfsplatform>
    COMMENT "Merging libntfscore.a"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT ${NTFS_MERGED_LIB}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/merge-static-libs.sh ${NTFS_MERGED_LIB} $<TARGET_FILE:ntfscore> $<TARGET_FILE:ntfsplatform>
    DEPENDS ntfscore ntfsplatform $<TARGET_FILE:ntfscore> $<TARGET_FILE:ntfsplatform> ${CMAKE_CURRENT_SOURCE_DIR}/tools/merge-static-libs.sh
    COMMENT "Merging libntfscore.a"
    VERBATIM)
endif()
add_custom_target(ntfscore_merged ALL DEPENDS ${NTFS_MERGED_LIB})

# ---- tests -----------------------------------------------------------------
if(NTFS_BUILD_TESTS)
  enable_testing()

  # Mount-time decisions: the journal clean rule, the read-only reason, the
  # probe fast path, and the device-flush count. Every case is a bug that
  # reached a real disk before it had a test.
  add_executable(test_mount core/tests/test_mount.c)
  target_link_libraries(test_mount ntfscore ntfsplatform)
  target_include_directories(test_mount PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logfile
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME mount COMMAND test_mount)
  set_tests_properties(mount PROPERTIES
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images;NTFS_PATCH_ATTRDEF_SIZE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_attrdef_size.py")

  # Volume geometry: 4Kn sectors, cluster sizes on either side of the sector
  # size, and a 16 TiB sparse volume whose metadata sits past the 32-bit byte
  # mark. Builds its own images with mkntfs and checks them with ntfsck -n.
  # This is what found finding 15 (any sector size above 512 destroyed the
  # volume on the first write) and what pins it fixed. See
  # docs/UPSTREAM-BUGS.md.
  add_executable(test_geometry core/tests/test_geometry.c)
  target_link_libraries(test_geometry ntfscore ntfsplatform)
  target_include_directories(test_geometry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME geometry COMMAND test_geometry)
  set_tests_properties(geometry PROPERTIES
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images;NTFS_TOOLS=${CMAKE_CURRENT_SOURCE_DIR}/tools"
    TIMEOUT 600)

  # I/O error paths: reads and writes that fail, a device flush that fails, and
  # a volume that fills up. Wraps a real image's ntfs_bdev in a vtable that
  # refuses on demand, and runs ntfsck -n after every injected fault -- an error
  # the driver swallows becomes silent corruption on someone's disk.
  add_executable(test_faults core/tests/test_faults.c)
  target_link_libraries(test_faults ntfscore ntfsplatform)
  target_include_directories(test_faults PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME faults COMMAND test_faults)
  set_tests_properties(faults PROPERTIES
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images;NTFS_MKNTFS=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/sbin/mkntfs;NTFS_NTFSCK=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local-plus/sbin/ntfsck;NTFS_NTFSINFO=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/bin/ntfsinfo;NTFS_PATCH_REPARSE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_reparse.py")

  # Crash consistency: the write ordering in platform/src/inode.c is the only
  # thing between an interrupted operation and a volume whose next allocation
  # overwrites live data. Records a sync's device writes and asserts the order,
  # then cuts real operations short at sampled write counts and asks ntfsck.
  add_executable(test_crash core/tests/test_crash.c)
  target_link_libraries(test_crash ntfscore ntfsplatform)
  target_include_directories(test_crash PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logfile
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME crash COMMAND test_crash)
  set_tests_properties(crash PROPERTIES
    ENVIRONMENT "NTFS_MKNTFS=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/sbin/mkntfs;NTFS_NTFSCK=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local-plus/sbin/ntfsck;NTFS_NTFSINFO=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/bin/ntfsinfo;NTFS_PATCH_REPARSE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_reparse.py")

  # Large directories: the index B-tree. Nothing exercised a directory past a
  # handful of entries, so ntfs_ib_split(), ntfs_ir_reparent(),
  # ntfs_index_rm_node/_rm_leaf() and ntfs_ih_takeout() had never run under a
  # test. Builds its own images with mkntfs and checks each phase with
  # `ntfsck -n`, because a mis-split index loses names, not files, and a
  # read-back comparison agrees with the driver's own broken tree.
  add_executable(test_bigdir core/tests/test_bigdir.c)
  target_link_libraries(test_bigdir ntfscore ntfsplatform)
  target_include_directories(test_bigdir PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logfile
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME bigdir COMMAND test_bigdir)
  set_tests_properties(bigdir PROPERTIES
    ENVIRONMENT "NTFS_MKNTFS=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/sbin/mkntfs;NTFS_NTFSCK=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local-plus/sbin/ntfsck;NTFS_NTFSINFO=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/bin/ntfsinfo;NTFS_PATCH_REPARSE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_reparse.py;NTFS_PATCH_IB_SIZE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_ib_size.py")

  # Compressed writes and sparse files. core/ntfs/compress.c's write half had
  # never been executed by anything, and the compressed-sparse fixture was only
  # ever read. Several assertions here pin BUGS rather than correct behaviour;
  # they are marked BUG: in the source and listed in docs/UPSTREAM-BUGS.md.
  add_executable(test_compress core/tests/test_compress.c)
  target_link_libraries(test_compress ntfscore ntfsplatform)
  target_include_directories(test_compress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logfile
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME compress COMMAND test_compress)
  set_tests_properties(compress PROPERTIES
    TIMEOUT 600
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images;NTFSCK=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local-plus/sbin/ntfsck;NTFS_PATCH_OOB_RUN=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_oob_run.py;NTFS_PATCH_EMPTY_MAPPING_PAIRS=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_empty_mapping_pairs.py")

  # Hard links, symlinks, timestamps and permissions: all four shipped with no
  # test. Every mutating case ends in `ntfsck -n`, because a wrong link count or
  # a stale index entry is invisible to a read-back and only costs data later.
  add_executable(test_links core/tests/test_links.c)
  target_link_libraries(test_links ntfscore ntfsplatform)
  target_include_directories(test_links PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/logfile
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME links COMMAND test_links)
  set_tests_properties(links PROPERTIES
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images;NTFS_NTFSCK=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local-plus/sbin/ntfsck;NTFS_NTFSINFO=${CMAKE_CURRENT_SOURCE_DIR}/tools/.local/bin/ntfsinfo;NTFS_PATCH_REPARSE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_reparse.py;NTFS_PATCH_INTERIX=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_interix.py;NTFS_PATCH_EA=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_ea.py;NTFS_PATCH_STALE_REPARSE=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_stale_reparse.py;NTFS_PATCH_ERR_MREF=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_err_mref.py;NTFS_PATCH_NONRES_ATTR=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_nonres_attr.py;NTFS_PATCH_IB_USA=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_ib_usa.py;NTFS_PATCH_OOB_EA=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/patch_oob_ea.py;NTFS_INDEX_UNION=${CMAKE_CURRENT_SOURCE_DIR}/tools/mkfixtures/index_union.py")

  # The three name and xattr decisions in PORTING.md section 6, which were
  # recorded and then enforced by nothing: Windows-illegal names rejected,
  # case-insensitive and case-preserving lookup, and xattrs stored as alternate
  # data streams rather than ._ files.
  add_executable(test_names core/tests/test_names.c)
  target_link_libraries(test_names ntfscore ntfsplatform)
  target_include_directories(test_names PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  add_test(NAME names COMMAND test_names)
  set_tests_properties(names PROPERTIES
    ENVIRONMENT "NTFS_IMAGES=${CMAKE_CURRENT_SOURCE_DIR}/tools/images;NTFS_TOOLS=${CMAKE_CURRENT_SOURCE_DIR}/tools")

  # enable-module.sh: two silent-failure bugs cost a mount each. Static checks
  # plus the real prune loop under stubbed tools; touches no system state.
  add_test(NAME enable_module_script
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fskit/scripts/tests/test-enable-module.sh)

  # The $LogFile module keeps its own CMake project so it can be built alone.
  # Pull its tests in here as well: for a day they existed but were never run by
  # `ctest --test-dir build`, which is how a stale header that aborted ntfslog,
  # and an ASan hang that stopped logfile_unit's 243 checks from executing, both
  # went unnoticed. A test nobody runs is not coverage.
  set(NTFS_LOGFILE_TOOLS ON CACHE BOOL "" FORCE)
  set(NTFS_IMAGES "${CMAKE_CURRENT_SOURCE_DIR}/tools/images" CACHE PATH "" FORCE)
  add_subdirectory(core/logfile logfile-tools)

  add_executable(test_inode platform/tests/test_inode.c)
  target_link_libraries(test_inode ntfsplatform)
  add_test(NAME platform_inode COMMAND test_inode)

  add_executable(test_bdev platform/tests/test_bdev.c)
  target_link_libraries(test_bdev ntfsplatform)
  add_test(NAME platform_bdev COMMAND test_bdev)

  # The page cache's own tests, linked against the full platform library
  # (its standalone_shims.c is only for the standalone build).
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/pagecache/tests/test_pagecache.c)
    add_executable(test_pagecache platform/pagecache/tests/test_pagecache.c)
    target_link_libraries(test_pagecache ntfsplatform)
    add_test(NAME pagecache COMMAND test_pagecache)
  endif()
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/platform/pagecache/tests/stress_pagecache.c)
    add_executable(stress_pagecache platform/pagecache/tests/stress_pagecache.c)
    target_link_libraries(stress_pagecache ntfsplatform)
    add_test(NAME pagecache_stress COMMAND stress_pagecache 3)
  endif()

  # Fuzz harness (GAPS.md §5.2): the same source as the libFuzzer target,
  # with a plain main() that mounts and walks each image once. That is the
  # deterministic corpus check tools/fuzz.sh runs (and pre-push runs);
  # sanitizer aborts still fire on a crash. It builds in both modes: in a
  # fuzz build its link line carries address+undefined (global, above)
  # but not -fsanitize=fuzzer, so it keeps its own main.
  add_executable(ntfs_fuzz_standalone core/fuzz/fuzz_mount.c)
  target_compile_definitions(ntfs_fuzz_standalone PRIVATE NTFS_FUZZ_STANDALONE)
  target_link_libraries(ntfs_fuzz_standalone ntfscore ntfsplatform)
  target_include_directories(ntfs_fuzz_standalone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)

  # The libFuzzer binary itself (Clang only, -DNTFS_FUZZ=ON, own build
  # dir so the instrumentation does not leak into the library the app
  # links):
  #   CC=clang cmake -S . -B build-fuzz -DNTFS_FUZZ=ON
  #   cmake --build build-fuzz --target ntfs_fuzz
  #   tools/fuzz.sh run 300
  if(NTFS_FUZZ)
    add_executable(ntfs_fuzz core/fuzz/fuzz_mount.c)
    target_link_options(ntfs_fuzz PRIVATE
      -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_libraries(ntfs_fuzz ntfscore ntfsplatform)
    target_include_directories(ntfs_fuzz PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/core/include
      ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
  endif()
endif()
