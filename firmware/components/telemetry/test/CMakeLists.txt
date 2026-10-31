# Host build of the telemetry tests: the component is plain C, so it runs on the PC without ESP-IDF.
#   cmake -S firmware/components/telemetry/test -B build/telemetry-test && cmake --build build/telemetry-test
#   ./build/telemetry-test/telemetry_test
cmake_minimum_required(VERSION 3.16)
project(telemetry_test C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror)
# AddressSanitizer and UBSan catch out-of-bounds access and overflow that a plain run would miss
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

add_executable(telemetry_test
    test_main.c
    ../src/outbox.c
    ../src/payload.c
    ../src/backoff.c
    ../src/device_config.c)
target_include_directories(telemetry_test PRIVATE ../include)
target_link_libraries(telemetry_test m)
