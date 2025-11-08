cmake_minimum_required(VERSION 3.16)
project(als-dimmer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(USE_DDCUTIL "Build with DDC/CI support via libddcutil" OFF)
option(INSTALL_SYSTEMD_SERVICE "Install systemd service file" OFF)
set(CONFIG_FILE "config_opti4001_ddcutil.json" CACHE STRING "Default config file to use")

# Core sources (always compiled)
set(CORE_SOURCES
    src/main.cpp
    src/config.cpp
    src/state_manager.cpp
    src/control_interface.cpp
    src/zone_mapper.cpp
    src/brightness_controller.cpp
    src/sensors/file_sensor.cpp
    src/sensors/opti4001_sensor.cpp
    src/sensors/fpga_opti4001_sensor.cpp
    src/outputs/file_output.cpp
    src/outputs/i2c_dimmer_output.cpp
)

# Create executable
add_executable(als-dimmer ${CORE_SOURCES})

# Include directories
target_include_directories(als-dimmer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Link libraries
target_link_libraries(als-dimmer PRIVATE pthread)

# Conditional DDC/CI support
if(USE_DDCUTIL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DDCUTIL REQUIRED ddcutil)

    target_sources(als-dimmer PRIVATE src/outputs/ddcutil_output.cpp)
    target_compile_definitions(als-dimmer PRIVATE HAVE_DDCUTIL)
    target_link_libraries(als-dimmer PRIVATE ${DDCUTIL_LIBRARIES})
    target_include_directories(als-dimmer PRIVATE ${DDCUTIL_INCLUDE_DIRS})

    message(STATUS "DDC/CI support: ENABLED")
else()
    message(STATUS "DDC/CI support: DISABLED")
endif()

# Compiler warnings
target_compile_options(als-dimmer PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

# Configure systemd service file with CMAKE_INSTALL_PREFIX
if(INSTALL_SYSTEMD_SERVICE)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/systemd/als-dimmer.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/als-dimmer.service
        @ONLY
    )
endif()

# Install rules
install(TARGETS als-dimmer DESTINATION bin)

# Install all config files to PREFIX/etc/als-dimmer/
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/configs/*.json")
install(FILES ${CONFIG_FILES}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/etc/als-dimmer)

# Create symlink for default config
install(CODE "
    file(CREATE_LINK
        ${CONFIG_FILE}
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/etc/als-dimmer/config.json\"
        SYMBOLIC
    )
    message(STATUS \"Created symlink: config.json -> ${CONFIG_FILE}\")
")

# Install systemd service if enabled
if(INSTALL_SYSTEMD_SERVICE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/als-dimmer.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    message(STATUS "Systemd service will be installed to ${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
endif()
