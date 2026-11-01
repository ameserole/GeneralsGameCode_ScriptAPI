# Script API unit tests. These only need core_scriptapi, so they run without the game or its data.
add_executable(scriptapi_googletest)

target_sources(scriptapi_googletest PRIVATE
    ScriptApiCommandsTest.cpp
    ScriptApiEventsTest.cpp
    ScriptApiGameStateTest.cpp
    ScriptApiAgentFixesTest.cpp
    ScriptApiHardeningTest.cpp
    ScriptApiLanTest.cpp
    ScriptApiLiveTest.cpp
    ScriptApiProtocolTest.cpp
    ScriptApiRouterTest.cpp
    ScriptApiServerTest.cpp
    ScriptApiSkirmishTest.cpp
    ScriptApiTournamentTest.cpp
    ScriptApiWorldTest.cpp
)

target_link_libraries(scriptapi_googletest PRIVATE
    core_scriptapi
    deps_config
    GTest::gtest_main
    ixwebsocket
)

add_test(NAME scriptapi_googletest COMMAND scriptapi_googletest)
