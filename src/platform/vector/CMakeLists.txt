add_executable(mgba-vector
        main.c
        video.c
        audio.c
        #input.c
        ${CORE_SRC}
        ${CORE_VFS_SRC}
        ${GBA_SRC}
        ${CMAKE_SOURCE_DIR}/src/core/thread.c
        ${CMAKE_SOURCE_DIR}/src/core/log.c
        ${CMAKE_SOURCE_DIR}/src/feature/thread-proxy.c
        ${CMAKE_SOURCE_DIR}/src/feature/video-logger.c
        ${CMAKE_SOURCE_DIR}/src/feature/video-backend.c
        ${CMAKE_SOURCE_DIR}/src/feature/proxy-backend.c
        ${CMAKE_SOURCE_DIR}/src/gba/extra/proxy.c
        ${CMAKE_SOURCE_DIR}/src/gb/extra/proxy.c
)

target_compile_options(mgba-vector PRIVATE
        -g
        -Wall
        -mno-unaligned-access
)

target_compile_definitions(mgba-vector PRIVATE
        MGBA_STANDALONE
        ENABLE_VFS
        ENABLE_VFS_FD
        M_CORE_GBA
        USE_PTHREADS
)

target_include_directories(mgba-vector PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(mgba-vector ${OS_LIB} pthread)

install(TARGETS mgba-vector DESTINATION bin)