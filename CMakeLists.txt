# Set minimum CMake version.
cmake_minimum_required(VERSION 3.17)

# Create project.
project(ClevoFanControl C)

# Enable the language standard.
set(CMAKE_C_STANDARD 99)

# Add the main executable.
add_executable(clevo-fan-control src/main.c)

# Use the package PkgConfig to detect dependencies.
find_package(PkgConfig REQUIRED)

# Prefer the legacy AppIndicator package when available, but fall back to
# Ayatana AppIndicator. Newer Debian/Ubuntu releases ship Ayatana instead of
# appindicator3-0.1.
pkg_check_modules(APPINDICATOR appindicator3-0.1)
if(NOT APPINDICATOR_FOUND)
        pkg_check_modules(APPINDICATOR REQUIRED ayatana-appindicator3-0.1)
        target_compile_definitions(clevo-fan-control PRIVATE USE_AYATANA_APPINDICATOR)
endif()

pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# Include dependency headers for this target.
target_include_directories(clevo-fan-control PRIVATE
        ${APPINDICATOR_INCLUDE_DIRS}
        ${GTK3_INCLUDE_DIRS}
)

# Add dependency library search paths for this target.
target_link_directories(clevo-fan-control PRIVATE
        ${APPINDICATOR_LIBRARY_DIRS}
        ${GTK3_LIBRARY_DIRS}
)

# Add dependency-specific compiler flags for this target.
target_compile_options(clevo-fan-control PRIVATE
        ${APPINDICATOR_CFLAGS_OTHER}
        ${GTK3_CFLAGS_OTHER}
)

# Link the target to AppIndicator/GTK and libm. round() is provided by libm,
# so it needs to be modeled as a link dependency instead of a compiler flag.
target_link_libraries(clevo-fan-control
        ${APPINDICATOR_LIBRARIES}
        ${GTK3_LIBRARIES}
        m
)

# Specify installation rules.
install(
        TARGETS clevo-fan-control
        PERMISSIONS WORLD_EXECUTE OWNER_WRITE OWNER_READ SETUID GROUP_EXECUTE
        DESTINATION bin
)
install(
        FILES src/clevo-fan-control.desktop
        PERMISSIONS WORLD_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE
        DESTINATION ~/.config/autostart
)
install (
        CODE "execute_process(COMMAND sudo chown ${USER}:${GROUP} ${HOME}/.config/autostart/clevo-fan-control.desktop)"
)
