add_executable(subgraph-extraction-cpu subgraph_extraction_cli.cpp)
add_dependencies(apps subgraph-extraction-cpu)
target_link_libraries(subgraph-extraction-cpu PRIVATE Katana::galois lonestar)
install(TARGETS subgraph-extraction-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)