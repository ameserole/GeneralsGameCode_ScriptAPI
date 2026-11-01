# Script API transport and protocol. Game-independent, so it can be unit tested without the engine.
set(SCRIPTAPI_SRC
    ScriptApiCommands.cpp
    ScriptApiCommands.h
    ScriptApiEvents.cpp
    ScriptApiEvents.h
    ScriptApiGameState.cpp
    ScriptApiGameState.h
    ScriptApiLan.cpp
    ScriptApiLan.h
    ScriptApiProtocol.cpp
    ScriptApiProtocol.h
    ScriptApiRouter.cpp
    ScriptApiRouter.h
    ScriptApiServer.cpp
    ScriptApiServer.h
    ScriptApiSkirmish.cpp
    ScriptApiSkirmish.h
    ScriptApiWorld.cpp
    ScriptApiWorld.h
)

add_library(core_scriptapi STATIC)
set_target_properties(core_scriptapi PROPERTIES OUTPUT_NAME scriptapi)

target_sources(core_scriptapi PRIVATE ${SCRIPTAPI_SRC})

target_include_directories(core_scriptapi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(core_scriptapi PRIVATE
    deps_config
    ixwebsocket
)

target_link_libraries(core_scriptapi PUBLIC
    nlohmann_json::nlohmann_json
)
