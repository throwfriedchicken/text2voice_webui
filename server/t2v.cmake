# Included from qwentts.cpp/CMakeLists.txt by build.bat.
add_executable(t2v-server ${CMAKE_CURRENT_LIST_DIR}/t2v-server.cpp)
target_link_libraries(t2v-server PRIVATE qwen-core httplib yyjson)
link_ggml_backends(t2v-server)
if(WIN32)
    target_link_libraries(t2v-server PRIVATE shell32)
endif()
