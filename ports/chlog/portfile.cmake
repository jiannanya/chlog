vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jiannanya/chlog
    REF 91334614beec914b24a9b7a280478716d5fdb085
    SHA512 2a3ad64296b1f9bfb6a95db804fb97e64370b4e9a3ded0a23e534a02ccdb9c2910a901f909f0221ed4ff27b5a941185edbb7d54f2fb4ebdf1b61049ca0b64102
)

vcpkg_configure_cmake(
    SOURCE_PATH "${SOURCE_PATH}"
    PREFER_NINJA
    OPTIONS
        -DCHLOG_BUILD_EXAMPLES=OFF
        -DCHLOG_BUILD_BENCHMARKS=OFF
)

vcpkg_install_cmake()

vcpkg_fixup_cmake_targets(
    CONFIG_PATH lib/cmake/chlog
)

# Header-only: remove debug tree if produced.
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
endif()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
