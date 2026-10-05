# Vendored header-only libraries. SYSTEM include directories keep our warnings and
# clang-tidy out of them.

find_package(Threads REQUIRED)

add_library(paramesh_third_party_json INTERFACE)
target_include_directories(paramesh_third_party_json SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/include)
add_library(paramesh::json ALIAS paramesh_third_party_json)

add_library(paramesh_third_party_httplib INTERFACE)
target_include_directories(paramesh_third_party_httplib SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib/include)
target_link_libraries(paramesh_third_party_httplib INTERFACE Threads::Threads)
add_library(paramesh::httplib ALIAS paramesh_third_party_httplib)

add_library(paramesh_third_party_picosha2 INTERFACE)
target_include_directories(paramesh_third_party_picosha2 SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/picosha2/include)
add_library(paramesh::picosha2 ALIAS paramesh_third_party_picosha2)

add_library(paramesh_third_party_doctest INTERFACE)
target_include_directories(paramesh_third_party_doctest SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/doctest/include)
add_library(paramesh::doctest ALIAS paramesh_third_party_doctest)
