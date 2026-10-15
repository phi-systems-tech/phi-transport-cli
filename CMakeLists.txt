cmake_minimum_required(VERSION 3.21)

project(phi-transport-cli
    VERSION 1.0.0
    DESCRIPTION "CLI transport plugin and local phi-cli client"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# No Qt. The plugin runs on the runtime loop (contract 2.0.0) and phi-cli is a
# blocking client: nlohmann for JSON, the transport api for the envelope rules
# both sides of the wire have to agree on, and poll() for the waiting.
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(phi-transport-api 2.2.0 CONFIG QUIET)

if(NOT TARGET phicore::transport-api AND NOT TARGET phi_transport_api)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../phi-transport-api/CMakeLists.txt")
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/../phi-transport-api"
            "${CMAKE_CURRENT_BINARY_DIR}/_deps/phi-transport-api"
        )
    endif()
endif()

if(TARGET phicore::transport-api)
    set(PHI_TRANSPORT_API_TARGET phicore::transport-api)
elseif(TARGET phi_transport_api)
    set(PHI_TRANSPORT_API_TARGET phi_transport_api)
else()
    message(FATAL_ERROR "phi-transport-api target not found. Install phi-transport-api or place repo next to this project.")
endif()

add_library(phi_transport_cli MODULE
    src/clitransport.cpp
    src/clitransport.h
)

target_link_libraries(phi_transport_cli
    PRIVATE
        nlohmann_json::nlohmann_json
        ${PHI_TRANSPORT_API_TARGET}
)

set_target_properties(phi_transport_cli PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/transports"
)

add_executable(phi-cli
    src/phi_cli_main.cpp
    src/cli_socket.cpp
    src/cli_socket.h
    src/scram_verifier.cpp
    src/scram_verifier.h
)

target_link_libraries(phi-cli
    PRIVATE
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        ${PHI_TRANSPORT_API_TARGET}
)

install(TARGETS phi_transport_cli
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/phi/plugins/transports
)

install(TARGETS phi-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
