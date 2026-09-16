cmake_minimum_required(VERSION 3.16)
project(cjong4_discard_risk VERSION 0.3.0 LANGUAGES C)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(CJ4DR_BUILD_TESTS "Build discard-risk tests" OFF)
option(CJ4DR_BUILD_TOOLS "Build the model evaluation CLI" OFF)
set(CJONG4_SOURCE_DIR "" CACHE PATH "Path to a cjong4 source tree")

if(NOT TARGET cjong4::cj4)
    if(CJONG4_SOURCE_DIR)
        set(BUILD_TESTS OFF)
        add_subdirectory("${CJONG4_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/cjong4")
    else()
        find_package(cjong4 4.0.0 CONFIG REQUIRED)
    endif()
endif()

add_library(cj4dr STATIC src/input.c src/model.c src/evaluator.c)
add_library(cjong4_discard_risk::cj4dr ALIAS cj4dr)
target_compile_features(cj4dr PUBLIC c_std_11)
set_target_properties(cj4dr PROPERTIES C_EXTENSIONS OFF)
target_include_directories(cj4dr PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(cj4dr PUBLIC cjong4::cj4)

function(cj4dr_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
cj4dr_enable_warnings(cj4dr)

if(CJ4DR_BUILD_TOOLS)
    add_executable(cj4dr_evaluate tools/evaluate.c)
    target_link_libraries(cj4dr_evaluate PRIVATE cj4dr)
    cj4dr_enable_warnings(cj4dr_evaluate)
endif()

if(CJ4DR_BUILD_TESTS)
    enable_testing()
    add_executable(cj4dr_input_tests tests/test_input.c tests/test_model.c)
    target_link_libraries(cj4dr_input_tests PRIVATE cj4dr)
    cj4dr_enable_warnings(cj4dr_input_tests)
    if(MSVC)
        target_compile_options(cj4dr_input_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(cj4dr_input_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME cj4dr_input_tests COMMAND cj4dr_input_tests)
    if(CJ4DR_BUILD_TOOLS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME cj4dr_cli_tests
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py"
                        $<TARGET_FILE:cj4dr_evaluate>)
        endif()
    endif()
endif()

install(TARGETS cj4dr EXPORT cjong4DiscardRiskTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(CJ4DR_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/cjong4_discard_risk")
install(EXPORT cjong4DiscardRiskTargets
    NAMESPACE cjong4_discard_risk::
    DESTINATION ${CJ4DR_CMAKE_INSTALL_DIR})
configure_package_config_file(cmake/cjong4_discard_riskConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cjong4_discard_riskConfig.cmake"
    INSTALL_DESTINATION ${CJ4DR_CMAKE_INSTALL_DIR})
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cjong4_discard_riskConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cjong4_discard_riskConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cjong4_discard_riskConfigVersion.cmake"
    DESTINATION ${CJ4DR_CMAKE_INSTALL_DIR})
