if(NOT EXISTS "${DEPLOY_DIR}/wafflecps.exe" OR NOT IS_DIRECTORY "${MINGW_BIN}")
    message(FATAL_ERROR "Windows deployment paths are invalid")
endif()

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

file(GLOB_RECURSE deployed_dlls "${DEPLOY_DIR}/*.dll")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${DEPLOY_DIR}/wafflecps.exe"
    MODULES ${deployed_dlls}
    DIRECTORIES "${DEPLOY_DIR}" "${MINGW_BIN}"
    PRE_EXCLUDE_REGEXES "^api-ms-win-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "^[A-Za-z]:/[Ww]indows/[Ss]ystem32/"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved)

if(unresolved)
    message(FATAL_ERROR "Unresolved Windows DLLs: ${unresolved}")
endif()

foreach(dependency IN LISTS resolved)
    cmake_path(IS_PREFIX MINGW_BIN "${dependency}" NORMALIZE from_mingw)
    if(from_mingw)
        file(COPY "${dependency}" DESTINATION "${DEPLOY_DIR}")
    endif()
endforeach()
