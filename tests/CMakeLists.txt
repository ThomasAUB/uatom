set(UATOM_UNIT_TESTS uatom_tests)

find_package(Threads REQUIRED)

add_executable(${UATOM_UNIT_TESTS} tests.cpp)

target_link_libraries(${UATOM_UNIT_TESTS} uatom Threads::Threads)

add_test(NAME ${UATOM_UNIT_TESTS} COMMAND ${UATOM_UNIT_TESTS})
