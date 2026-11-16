include_guard(GLOBAL)

function(minidb_find_drogon)
    if(NOT MINIDB_FETCH_DROGON)
        find_package(Drogon 1.9.14 CONFIG REQUIRED)
        return()
    endif()

    include(FetchContent)

    # Scope upstream's generic options to this function. In particular, keep
    # Drogon/Trantor tests disabled without disabling this project's CTest tests.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_CTL OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_ORM OFF)
    set(BUILD_POSTGRESQL OFF)
    set(BUILD_MYSQL OFF)
    set(BUILD_SQLITE OFF)
    set(BUILD_REDIS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_DOC OFF)
    set(BUILD_BROTLI OFF)
    set(BUILD_YAML_CONFIG OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_C-ARES OFF)
    set(USE_SUBMODULE ON)
    set(USE_SPDLOG OFF)
    set(USE_COROUTINE OFF)
    set(DROGON_USE_BOOST_UUID OFF)
    set(TRANTOR_USE_TLS none CACHE STRING "TLS provider for the Phase 3 HTTP dependency")

    # Drogon v1.9.14. Its gitlink also pins Trantor to
    # 78e480f5e138acdc05f76c55a2716718cd846364. JsonCpp, uuid and zlib are
    # system dependencies; the existing database is never replaced with an ORM.
    # FETCHCONTENT_SOURCE_DIR_DROGON can point at an already prepared checkout
    # (including its Trantor submodule) for an offline/local-source build.
    FetchContent_Declare(drogon
        GIT_REPOSITORY https://github.com/drogonframework/drogon.git
        GIT_TAG 3aae769cc405cf5abb9d477675c6430924f02c7c
        GIT_SUBMODULES trantor
        GIT_SUBMODULES_RECURSE OFF
        GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(drogon)
endfunction()
