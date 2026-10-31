# Host build of the 1-Wire and DS18B20 tests: the protocol code is plain C on top of three bit operations, so it runs
# on the PC against a simulated bus.
#   cmake -S firmware/components/onewire/test -B build/onewire-test && cmake --build build/onewire-test
#   ./build/onewire-test/onewire_test
cmake_minimum_required(VERSION 3.16)
project(onewire_test C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror)
# AddressSanitizer and UBSan catch out-of-bounds access and overflow that a plain run would miss
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

add_executable(onewire_test
    test_main.c
    ../onewire.c
    ../../ds18b20/ds18b20.c
    ../../onewire_sim/onewire_sim.c)
target_include_directories(onewire_test PRIVATE
    ../include
    ../../ds18b20/include
    ../../onewire_sim/include)
