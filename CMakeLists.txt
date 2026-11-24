cmake_minimum_required(VERSION 3.0.0)
project(astro VERSION 1.1.0 LANGUAGES C CXX)

# Automatically create moc files
set(CMAKE_AUTOMOC ON)

execute_process(
    COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
    OUTPUT_VARIABLE ARCH_TRIPLET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(QT_IMPORTS_DIR "lib/${ARCH_TRIPLET}")

set(PROJECT_NAME "astro")
set(FULL_PROJECT_NAME "astro.madabu")
set(DATA_DIR /)
set(DESKTOP_FILE_NAME ${PROJECT_NAME}.desktop)



configure_file(manifest.json.in ${CMAKE_CURRENT_BINARY_DIR}/manifest.json)

# Generate QML-readable application version information.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated-qml")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/BuildInfo.js.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated-qml/BuildInfo.js"
    @ONLY
)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/generated-qml/BuildInfo.js"
    DESTINATION "${DATA_DIR}qml"
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/manifest.json DESTINATION ${CMAKE_INSTALL_PREFIX})
install(FILES ${PROJECT_NAME}.apparmor DESTINATION ${DATA_DIR})

# Ubuntu Touch push notification helper
install(PROGRAMS backend/notifications/pushexec DESTINATION ${DATA_DIR})
install(FILES backend/notifications/push.json DESTINATION ${DATA_DIR})
install(FILES backend/notifications/push-apparmor.json DESTINATION ${DATA_DIR})

install(DIRECTORY assets DESTINATION ${DATA_DIR})

install(DIRECTORY qml DESTINATION ${DATA_DIR})
install(FILES README.md LICENSE DESTINATION ${DATA_DIR})

# Translations
file(GLOB_RECURSE I18N_SRC_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/po qml/*.qml qml/*.js)
list(APPEND I18N_SRC_FILES ${DESKTOP_FILE_NAME}.in.h)

find_program(INTLTOOL_MERGE intltool-merge)
if(NOT INTLTOOL_MERGE)
    message(FATAL_ERROR "Could not find intltool-merge, please install the intltool package")
endif()
find_program(INTLTOOL_EXTRACT intltool-extract)
if(NOT INTLTOOL_EXTRACT)
    message(FATAL_ERROR "Could not find intltool-extract, please install the intltool package")
endif()

add_custom_target(${DESKTOP_FILE_NAME} ALL
    COMMENT "Merging translations into ${DESKTOP_FILE_NAME}..."
    COMMAND LC_ALL=C ${INTLTOOL_MERGE} -d -u ${CMAKE_SOURCE_DIR}/po ${CMAKE_SOURCE_DIR}/${DESKTOP_FILE_NAME}.in ${DESKTOP_FILE_NAME}
    COMMAND sed -i 's/${PROJECT_NAME}-//g' ${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_FILE_NAME}
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${DESKTOP_FILE_NAME} DESTINATION ${DATA_DIR})

add_subdirectory(po)

# Make source files visible in qtcreator
file(GLOB_RECURSE PROJECT_SRC_FILES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    qml/*.qml
    qml/*.js
    src/*
    *.json
    *.json.in
    *.apparmor
    *.desktop.in
)

add_custom_target(${PROJECT_NAME}_FILES ALL SOURCES ${PROJECT_SRC_FILES})


# Astro C++ QML backend
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Qml DBus)
add_library(AstroBackend MODULE
    backend/QMLBackend/Backend.cpp
    backend/QMLBackend/BackendConnections.cpp
    backend/QMLBackend/BackendSecretService.cpp
    backend/QMLBackend/BackendStorage.cpp
    backend/QMLBackend/BackendMatrix.cpp
    backend/QMLBackend/BackendMedia.cpp
    backend/QMLBackend/Backend.h
    backend/notifications/NotificationManager.cpp
    backend/notifications/NotificationManager.h
    backend/StorageWorker/StorageWorker.cpp
    backend/StorageWorker/StorageWorker.h
    backend/StorageWorker/StorageWorkerNotifications.cpp
    backend/StorageWorker/StorageWorkerMatrix.cpp
    backend/StorageWorker/StorageWorkerDevices.cpp
    backend/StorageWorker/StorageWorkerMedia.cpp
    backend/astro_core_ffi.h
    backend/plugin.cpp
    backend/plugin.h
)

target_include_directories(AstroBackend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/backend/QMLBackend
)

target_link_libraries(AstroBackend
    PRIVATE
        Qt5::Core
        Qt5::Qml
        Qt5::DBus
        Qt5::Gui
)

install(
    TARGETS AstroBackend
    LIBRARY DESTINATION ${QT_IMPORTS_DIR}/Astro/Backend
)

install(
    FILES backend/qmldir
    DESTINATION ${QT_IMPORTS_DIR}/Astro/Backend
)
# Build the Astro Rust core as an ARM64 static library
set(RUST_TARGET "$ENV{ARCH_RUST}")

if(NOT RUST_TARGET)
    message(FATAL_ERROR "ARCH_RUST is not set by the Clickable build environment")
endif()

find_program(CARGO_EXECUTABLE cargo)

if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR "Cargo was not found in the Clickable build environment")
endif()

set(RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(RUST_STATIC_LIBRARY
    "${RUST_TARGET_DIR}/${RUST_TARGET}/release/libastro_core.a"
)

add_custom_command(
    OUTPUT "${RUST_STATIC_LIBRARY}"

    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
        "CARGO_HOME=${CMAKE_CURRENT_BINARY_DIR}/cargo-home"
        "${CARGO_EXECUTABLE}" build
        --manifest-path "${CMAKE_SOURCE_DIR}/backend/astro-core/Cargo.toml"
        --target "${RUST_TARGET}"
        --release
        --locked
    DEPENDS
    DEPENDS
        backend/astro-core/Cargo.toml
        backend/astro-core/Cargo.lock
        backend/astro-core/src/lib.rs
        backend/astro-core/src/ffi.rs
        backend/astro-core/src/storage.rs
        backend/astro-core/src/matrix_client.rs
        backend/astro-core/src/notifications.rs
        backend/astro-core/src/media.rs
        backend/astro-core/src/session_storage.rs


    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/backend/astro-core"
    COMMENT "Building Astro Rust core for ${RUST_TARGET}"
    VERBATIM
)

add_custom_target(astro_rust_build ALL
    DEPENDS "${RUST_STATIC_LIBRARY}"
)
# Link the Rust core into the Qt backend plugin.
add_dependencies(AstroBackend astro_rust_build)

target_link_libraries(AstroBackend
    PRIVATE
        "${RUST_STATIC_LIBRARY}"
)
