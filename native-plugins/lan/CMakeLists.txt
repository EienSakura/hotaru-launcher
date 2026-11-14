# 可独立编译的局域网实现；不链接宿主的 OpenSSL 或兼容插件 ABI。
set(HOTARU_BORINGSSL_SOURCE "" CACHE PATH "固定版本 BoringSSL 源码目录")
set(HOTARU_BORINGSSL_CRYPTO "" CACHE FILEPATH "固定版本 BoringSSL 的 x64 Release crypto.lib")
if(NOT EXISTS "${HOTARU_BORINGSSL_SOURCE}/include/openssl/curve25519.h" OR
   NOT EXISTS "${HOTARU_BORINGSSL_SOURCE}/LICENSE" OR NOT EXISTS "${HOTARU_BORINGSSL_CRYPTO}")
    message(FATAL_ERROR "请先在项目内编译 BoringSSL 并提供 HOTARU_BORINGSSL_SOURCE 和 HOTARU_BORINGSSL_CRYPTO。")
endif()

add_library(hotaru-lan-crypto STATIC IMPORTED)
set_target_properties(hotaru-lan-crypto PROPERTIES
    IMPORTED_LOCATION "${HOTARU_BORINGSSL_CRYPTO}"
    INTERFACE_INCLUDE_DIRECTORIES "${HOTARU_BORINGSSL_SOURCE}/include")

add_library(hotaru-lan-runtime STATIC
    LanCrypto.cpp LanFiles.cpp LanPeer.cpp LanPeer.h LanTransfer.cpp LanTransfer.h
    LanNetwork.cpp LanNetwork.h LanService.cpp LanService.h)
set_target_properties(hotaru-lan-runtime PROPERTIES AUTOMOC ON)
target_include_directories(hotaru-lan-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(hotaru-lan-runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(hotaru-lan-runtime PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_link_libraries(hotaru-lan-runtime PUBLIC Qt6::Widgets Qt6::Network
    PRIVATE hotaru-lan-crypto crypt32 iphlpapi ole32 ws2_32 bcrypt dnsapi)

add_library(hotaru-lan-module MODULE LanPlugin.cpp plugin.json)
set_target_properties(hotaru-lan-module PROPERTIES AUTOMOC ON OUTPUT_NAME HotaruLan
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/native-preview"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/native-preview")
target_include_directories(hotaru-lan-module PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(hotaru-lan-module PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_link_libraries(hotaru-lan-module PRIVATE hotaru-lan-runtime)
add_custom_command(TARGET hotaru-lan-module POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${HOTARU_BORINGSSL_SOURCE}/LICENSE" "$<TARGET_FILE_DIR:hotaru-lan-module>/BoringSSL-LICENSE.txt")
add_dependencies(ltool hotaru-lan-module)

add_executable(hotaru-lan-selftest LanSelftest.cpp)
target_include_directories(hotaru-lan-selftest PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(hotaru-lan-selftest PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_link_libraries(hotaru-lan-selftest PRIVATE hotaru-lan-runtime)
set_target_properties(hotaru-lan-selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
