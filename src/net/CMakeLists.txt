# pspc_net: host networking shared by the network HLE libraries and pspc's ad hoc server
# (Winsock sockets with readiness events, the poller thread, the host's network configuration).
file(GLOB PSPC_NET_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(pspc_net STATIC ${PSPC_NET_SOURCES})
target_include_directories(pspc_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(pspc_net PUBLIC pspc_common ws2_32 iphlpapi)
