set(AGENT_CORE_TARGET llama-agent-core)

set(AGENT_CORE_SOURCES
    agent-loop.cpp
    agent-loop-compaction.cpp
    agent-loop-completion.cpp
    agent-loop-tools.cpp
    agent-resources.cpp
    clipboard-image.cpp
    compaction.cpp
    session-file.cpp
    tool-registry.cpp
    permission-policy.cpp
    permission.cpp
    permission-async.cpp
    skills/skills-manager.cpp
    agents-md/agents-md-manager.cpp
    tools/tool-bash.cpp
    tools/tool-read.cpp
    tools/tool-write.cpp
    tools/tool-edit.cpp
    tools/tool-glob.cpp
    tools/tool-plan.cpp
)

# MCP support only available on Unix (uses fork, pipe, poll)
if(NOT WIN32)
    list(APPEND AGENT_CORE_SOURCES
        mcp/mcp-client.cpp
        mcp/mcp-server-manager.cpp
        mcp/mcp-tool-wrapper.cpp
    )
endif()

add_library(${AGENT_CORE_TARGET} STATIC ${AGENT_CORE_SOURCES})
target_link_libraries(${AGENT_CORE_TARGET} PUBLIC server-context llama-common ${CMAKE_THREAD_LIBS_INIT})
target_compile_features(${AGENT_CORE_TARGET} PUBLIC cxx_std_17)
target_include_directories(${AGENT_CORE_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../server
    ${CMAKE_SOURCE_DIR}/vendor
)

set(TARGET llama-agent)

add_executable(${TARGET}
    agent.cpp
    agent-loop-run.cpp
    agent-loop-completion-cli.cpp
    agent-loop-tools-cli.cpp
    permission-prompt.cpp
    stb-image-impl.cpp
)
target_link_libraries(${TARGET} PRIVATE ${AGENT_CORE_TARGET})
target_compile_features(${TARGET} PRIVATE cxx_std_17)

include_directories(../server)
include_directories(${CMAKE_SOURCE_DIR}/vendor)  # for stb/stb_image.h (terminal image preview)

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()

# llama-agent-server (HTTP API for agent)
if(LLAMA_HTTPLIB)
    set(AGENT_SERVER_TARGET llama-agent-server)

    set(AGENT_SERVER_SOURCES
        server/agent-server.cpp
        server/agent-session.cpp
        server/agent-routes.cpp
        ../server/server-http.cpp
    )

    add_executable(${AGENT_SERVER_TARGET} ${AGENT_SERVER_SOURCES})
    target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE ${AGENT_CORE_TARGET} server-context llama-ui PUBLIC llama-common cpp-httplib ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(${AGENT_SERVER_TARGET} PRIVATE cxx_std_17)
    target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ../server)
    target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
    # Include directory for generated server assets (index.html.gz.hpp, loading.html.hpp)
    target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_BINARY_DIR}/tools/server)
    # Depend on llama-server to ensure generated assets exist
    add_dependencies(${AGENT_SERVER_TARGET} llama-server)

    if(WIN32)
        target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE ws2_32)
    endif()

    if(LLAMA_TOOLS_INSTALL)
        install(TARGETS ${AGENT_SERVER_TARGET} RUNTIME)
    endif()
endif()

# Agent unit tests
if(LLAMA_BUILD_TESTS)
    add_executable(test-agent-compaction test-compaction.cpp compaction.cpp)
    target_link_libraries(test-agent-compaction PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-compaction PRIVATE cxx_std_17)
    target_include_directories(test-agent-compaction PRIVATE ../server)
    add_test(NAME agent-compaction COMMAND test-agent-compaction)

    add_executable(test-agent-session-file test-session-file.cpp session-file.cpp)
    target_link_libraries(test-agent-session-file PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-session-file PRIVATE cxx_std_17)
    target_include_directories(test-agent-session-file PRIVATE ../server)
    add_test(NAME agent-session-file COMMAND test-agent-session-file)

    add_executable(test-agent-resources
        test-resources.cpp
        agent-resources.cpp
        skills/skills-manager.cpp
        agents-md/agents-md-manager.cpp
    )
    target_link_libraries(test-agent-resources PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-resources PRIVATE cxx_std_17)
    target_include_directories(test-agent-resources PRIVATE ../server)
    add_test(NAME agent-resources COMMAND test-agent-resources)

    add_executable(test-agent-permission
        test-permission.cpp
        permission-policy.cpp
        permission.cpp
        permission-async.cpp
    )
    target_link_libraries(test-agent-permission PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-permission PRIVATE cxx_std_17)
    target_include_directories(test-agent-permission PRIVATE ../server)
    add_test(NAME agent-permission COMMAND test-agent-permission)
endif()
