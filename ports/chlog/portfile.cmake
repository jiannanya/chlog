vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jiannanya/chlog
    REF v1.0.0
    SHA512 c36fe021833bbc7de430a9cf0e7205cbc32f7f52b9d85100b204032ae9d9f4130e79cd9e53e13c67dcae0f0341da1153284e451db78df1f1a044b755c5e5562a
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

# Header-only: remove empty lib directories created by CMake install/export.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/lib")

# Header-only: remove debug tree if produced.
if(EXISTS "${CURRENT_PACKAGES_DIR}/debug")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
endif()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
