cmake_minimum_required(VERSION 3.20)
project(Rufux VERSION 2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wformat-security)
add_compile_definitions(RUFUX_VERSION="${PROJECT_VERSION}")
add_compile_definitions(RUFUX_LOCALEDIR="${CMAKE_INSTALL_PREFIX}/share/locale")

find_package(PkgConfig REQUIRED)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

# tauri-gui branch: the graphical interface lives in gui-tauri/ (Tauri +
# WebView replica of Rufus's dialog) and drives this backend over its CLI.
# The C binary is always command-line only here; rufux_gui_run is a stub.
message(STATUS "command-line backend only (GUI is gui-tauri/)")
set(RUFUX_GUI "none" CACHE STRING "GUI toolkit: none on this branch (see gui-tauri/)")

option(RUFUX_NLS "Native language support (gettext)" ON)
if(RUFUX_NLS)
  find_package(Intl QUIET)
endif()

add_executable(rufux
  src/linux/main.c
  src/linux/device.c
  src/linux/iso_probe.c
  src/linux/checksum.c
  src/linux/writer.c
  src/linux/exec.c
  src/linux/partition.c
  src/linux/mkfs_task.c
  src/linux/extract.c
  src/linux/boot.c
  src/linux/persist.c
  src/linux/badblocks.c
  src/linux/vhd.c
  src/linux/dosboot.c
  src/linux/wininstall.c
  src/linux/create.c
  src/linux/mount.c
  src/linux/priv.c
  src/linux/secureboot.c
  src/linux/update.c
  src/linux/smart.c
  src/linux/mjson.c
  src/linux/vdisk.c
  src/linux/bootcheck.c
  src/linux/biosboot.c
  src/linux/msdl.c
)

target_include_directories(rufux PRIVATE src/linux src)

# The stub explains that the GUI is gui-tauri/ (separate crate).
target_sources(rufux PRIVATE src/gui/gui_stub.c)

if(RUFUX_NLS AND Intl_FOUND)
  message(STATUS "NLS enabled (gettext)")
  target_compile_definitions(rufux PRIVATE ENABLE_NLS=1)
  target_include_directories(rufux PRIVATE ${Intl_INCLUDE_DIRS})
  target_link_libraries(rufux PRIVATE ${Intl_LIBRARIES})
  # compile translations
  file(GLOB PO_FILES po/*.po)
  foreach(PO ${PO_FILES})
    get_filename_component(LANG ${PO} NAME_WE)
    set(MO ${CMAKE_BINARY_DIR}/locale/${LANG}/LC_MESSAGES/rufux.mo)
    add_custom_command(OUTPUT ${MO}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/locale/${LANG}/LC_MESSAGES
      COMMAND msgfmt -o ${MO} ${PO}
      DEPENDS ${PO})
    list(APPEND MO_FILES ${MO})
    install(FILES ${MO} DESTINATION share/locale/${LANG}/LC_MESSAGES)
  endforeach()
  add_custom_target(rufux-locale ALL DEPENDS ${MO_FILES})
  find_program(MSGFMT msgfmt)
endif()

# OpenSSL for SHA-256 (checksum.c). Required, not optional: no fallback
# crypto implementations are kept in-tree.
target_include_directories(rufux PRIVATE ${CRYPTO_INCLUDE_DIRS})
target_link_directories(rufux PRIVATE ${CRYPTO_LIBRARY_DIRS})
target_link_libraries(rufux PRIVATE ${CRYPTO_LIBRARIES})

install(TARGETS rufux DESTINATION bin)
install(FILES doc/rufux.1 DESTINATION share/man/man1)
install(FILES res/linux/io.github.hultwl.rufux.desktop DESTINATION share/applications)
install(FILES res/linux/io.github.hultwl.rufux.policy DESTINATION share/polkit-1/actions)
# Boot payloads (FreeDOS set, UEFI:NTFS image) resolved at runtime.
install(DIRECTORY res/freedos DESTINATION share/rufux)
install(FILES res/uefi/uefi-ntfs.img DESTINATION share/rufux)
# Secure Boot revocation lists (UEFI DBX), read by the bootloader check
install(DIRECTORY res/dbx DESTINATION share/rufux FILES_MATCHING PATTERN "*.efiauth2")
# App icon
foreach(SZ 512 256 128 64 48 32 16)
  install(FILES res/icons/rufux-${SZ}.png
    DESTINATION share/icons/hicolor/${SZ}x${SZ}/apps
    RENAME io.github.hultwl.rufux.png)
endforeach()

enable_testing()
add_test(NAME phase1 COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_phase1.sh $<TARGET_FILE:rufux>)
add_test(NAME phase2 COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_phase2.sh $<TARGET_FILE:rufux>)
add_test(NAME phase3 COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_phase3.sh $<TARGET_FILE:rufux>)
add_test(NAME packaging COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_packaging.sh)
add_test(NAME windows_layout COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_windows_layout.sh $<TARGET_FILE:rufux>)
set_tests_properties(windows_layout PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME extract_layout COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_extract_layout.sh $<TARGET_FILE:rufux>)
set_tests_properties(extract_layout PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME wue COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_wue.sh $<TARGET_FILE:rufux>)
set_tests_properties(wue PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME zero_size COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_zero_size.sh $<TARGET_FILE:rufux>)
set_tests_properties(zero_size PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME fat_split COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_fat_split.sh $<TARGET_FILE:rufux>)
set_tests_properties(fat_split PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME relocatable COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_relocatable.sh $<TARGET_FILE:rufux>)
add_test(NAME cli COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_cli.sh $<TARGET_FILE:rufux>)
add_test(NAME fs_types COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_fs_types.sh $<TARGET_FILE:rufux>)
set_tests_properties(fs_types PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME smart COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_smart.sh $<TARGET_FILE:rufux>)
set_tests_properties(smart PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME vdisk COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_vdisk.sh $<TARGET_FILE:rufux>)
set_tests_properties(vdisk PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME bootcheck COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_bootcheck.sh $<TARGET_FILE:rufux>)
set_tests_properties(bootcheck PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME argv0 COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_argv0.sh $<TARGET_FILE:rufux>)
set_tests_properties(argv0 PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME msdl COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_msdl.sh $<TARGET_FILE:rufux>)
set_tests_properties(msdl PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME bios_boot COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_bios_boot.sh $<TARGET_FILE:rufux>)
set_tests_properties(bios_boot PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(cli PROPERTIES SKIP_RETURN_CODE 77)
