cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS deployment version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target macOS architecture")
project(Rufux VERSION 2.3.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wformat-security)
add_compile_definitions(
  _DARWIN_C_SOURCE
  RUFUX_VERSION="${PROJECT_VERSION}"
  RUFUX_LOCALEDIR="${CMAKE_INSTALL_PREFIX}/share/locale"
)

set(OPENSSL_ROOT_PREFIX "/opt/homebrew/opt/openssl@3" CACHE PATH "Homebrew OpenSSL 3 prefix")
set(E2FSPROGS_PREFIX "/opt/homebrew/opt/e2fsprogs" CACHE PATH "Homebrew e2fsprogs prefix")
set(NTFS3G_PREFIX "${CMAKE_SOURCE_DIR}/vendor/ntfs-3g" CACHE PATH "Vendored ntfs-3g prefix")

set(ENV{PKG_CONFIG_PATH}
  "${OPENSSL_ROOT_PREFIX}/lib/pkgconfig:${E2FSPROGS_PREFIX}/lib/pkgconfig:${NTFS3G_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}"
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

find_path(E2FSPROGS_INCLUDE_DIR ext2fs/ext2fs.h
  PATHS "${E2FSPROGS_PREFIX}/include"
  NO_DEFAULT_PATH
  REQUIRED
)
find_library(EXT2FS_LIBRARY
  NAMES libext2fs.2.1.dylib libext2fs.2.dylib ext2fs
  PATHS "${E2FSPROGS_PREFIX}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)
find_library(COM_ERR_LIBRARY
  NAMES libcom_err.1.1.dylib libcom_err.1.dylib com_err
  PATHS "${E2FSPROGS_PREFIX}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)

find_path(NTFS3G_INCLUDE_DIR ntfs-3g/volume.h
  PATHS "${NTFS3G_PREFIX}/include"
  NO_DEFAULT_PATH
  REQUIRED
)
find_library(NTFS3G_LIBRARY ntfs-3g
  PATHS "${NTFS3G_PREFIX}/lib"
  NO_DEFAULT_PATH
  REQUIRED
)

find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(DISKARBITRATION_FRAMEWORK DiskArbitration REQUIRED)
find_library(CURL_LIBRARY curl REQUIRED)

set(CMAKE_BUILD_RPATH
  "${NTFS3G_PREFIX}/lib"
  "${E2FSPROGS_PREFIX}/lib"
  "${OPENSSL_ROOT_PREFIX}/lib"
)

add_library(rufux_core STATIC
  src/platform_macos.c
  src/exec.c
  src/device.c
  src/writer.c
  src/badblocks.c
  src/vdisk.c
  src/partition.c
  src/boot.c
  src/dosboot.c
  src/mkfs_task.c
  src/persist.c
  src/ntfs_populate.c
  src/mount.c
  src/checksum.c
  src/iso_probe.c
  src/vhd.c
  src/mjson.c
  src/bootcheck.c
  src/secureboot.c
  src/smart.c
  src/msdl.c
  src/update.c
  src/priv.c
  src/i18n.c
  src/gui_stub.c
  src/extract.c
  src/wininstall.c
  src/biosboot.c
  src/create.c
)

target_include_directories(rufux_core PUBLIC
  "${CMAKE_SOURCE_DIR}/src"
  ${CRYPTO_INCLUDE_DIRS}
  "${E2FSPROGS_INCLUDE_DIR}"
  "${NTFS3G_INCLUDE_DIR}"
)

target_link_directories(rufux_core PUBLIC
  ${CRYPTO_LIBRARY_DIRS}
  "${E2FSPROGS_PREFIX}/lib"
  "${NTFS3G_PREFIX}/lib"
)

target_link_libraries(rufux_core PUBLIC
  ${CRYPTO_LIBRARIES}
  "${NTFS3G_LIBRARY}"
  "${EXT2FS_LIBRARY}"
  "${COM_ERR_LIBRARY}"
  "${IOKIT_FRAMEWORK}"
  "${COREFOUNDATION_FRAMEWORK}"
  "${DISKARBITRATION_FRAMEWORK}"
  "${CURL_LIBRARY}"
)

add_executable(ctest_core tests/ctest_core.c)
target_link_libraries(ctest_core PRIVATE rufux_core)

add_executable(ctest_msdl_cli tests/ctest_msdl_cli.c)
target_link_libraries(ctest_msdl_cli PRIVATE rufux_core)

add_executable(ctest_device_writer tests/ctest_device_writer.c)
target_link_libraries(ctest_device_writer PRIVATE rufux_core)

add_executable(ctest_partition_boot tests/ctest_partition_boot.c)
target_link_libraries(ctest_partition_boot PRIVATE rufux_core)

add_executable(rufux-mountd src/mountd_nfs.c)
target_include_directories(rufux-mountd PRIVATE
  "${E2FSPROGS_INCLUDE_DIR}"
  "${NTFS3G_INCLUDE_DIR}"
)
target_link_directories(rufux-mountd PRIVATE
  "${E2FSPROGS_PREFIX}/lib"
  "${NTFS3G_PREFIX}/lib"
)
target_link_libraries(rufux-mountd PRIVATE
  "${NTFS3G_LIBRARY}"
  "${EXT2FS_LIBRARY}"
  "${COM_ERR_LIBRARY}"
)

add_executable(ctest_filesystems tests/ctest_filesystems.c)
target_link_libraries(ctest_filesystems PRIVATE rufux_core)

add_executable(rufux-mount-test-cli tests/ctest_mount_cli.c)
target_link_libraries(rufux-mount-test-cli PRIVATE rufux_core)
add_dependencies(rufux-mount-test-cli rufux-mountd)

add_executable(rufux src/main.c)
target_link_libraries(rufux PRIVATE rufux_core)
add_dependencies(rufux rufux-mountd)

enable_testing()
add_test(NAME ctest_core COMMAND ctest_core)
add_test(NAME test_core COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_core.sh" $<TARGET_FILE:ctest_msdl_cli>)
add_test(NAME ctest_device_writer COMMAND ctest_device_writer)
add_test(NAME ctest_partition_boot COMMAND ctest_partition_boot)
add_test(NAME ctest_filesystems COMMAND ctest_filesystems)
add_test(NAME test_mount_rw COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_mount_rw.sh" $<TARGET_FILE:rufux-mount-test-cli>)
add_test(NAME test_create_e2e COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_create_e2e.sh" $<TARGET_FILE:rufux>)
add_test(NAME test_bundle_dmg COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_bundle_dmg.sh")
