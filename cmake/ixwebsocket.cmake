# Fetch IXWebSocket for the script API server. TLS and compression are not needed for a localhost-only server.

include(FetchContent)

set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_WS OFF CACHE BOOL "" FORCE)
set(USE_TEST OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

# IXWebSocket declares an old cmake_minimum_required that CMake 4 no longer accepts.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
    ixwebsocket
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v11.4.5
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(ixwebsocket)

unset(CMAKE_POLICY_VERSION_MINIMUM)
