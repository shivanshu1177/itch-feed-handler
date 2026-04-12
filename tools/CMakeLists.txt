# Optional: link against libpcap for PCAP replay/inspect tools
find_library(PCAP_LIB pcap)

if(PCAP_LIB)
    message(STATUS "libpcap found: ${PCAP_LIB} — building PCAP tools")

    add_executable(itch_inspect itch_inspect.cpp)
    target_link_libraries(itch_inspect PRIVATE itch_protocol itch_headers ${PCAP_LIB})
    set_project_warnings(itch_inspect)

    add_executable(pcap_replay pcap_replay.cpp)
    target_link_libraries(pcap_replay PRIVATE itch_protocol itch_headers ${PCAP_LIB})
    set_project_warnings(pcap_replay)
else()
    message(STATUS "libpcap not found — skipping PCAP tools (install libpcap-dev to enable)")
endif()
