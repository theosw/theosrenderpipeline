# Upstream source and bundled decoder are provisioned in the ignored dependency
# snapshot. See README.md for the pinned revision and THIRD-PARTY.md for notices.
set(ARP_DETOURS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.dependencies/detours" CACHE PATH
    "Nukem9/Detours source checkout or snapshot")
if(NOT EXISTS "${ARP_DETOURS_DIR}/detours/HideStaticLibSymbols.c")
    message(FATAL_ERROR "Detours source missing at '${ARP_DETOURS_DIR}'. See README.md or set ARP_DETOURS_DIR.")
endif()

add_library(ARPDetours STATIC
    "${ARP_DETOURS_DIR}/detours/Detours.cpp"
    "${ARP_DETOURS_DIR}/detours/Detours64.cpp"
    "${ARP_DETOURS_DIR}/detours/HideStaticLibSymbols.c")
target_compile_features(ARPDetours PRIVATE cxx_std_17)
target_include_directories(ARPDetours
    PUBLIC "${ARP_DETOURS_DIR}"
    PRIVATE "${ARP_DETOURS_DIR}/detours/zydis/msvc"
            "${ARP_DETOURS_DIR}/detours/zydis/src"
            "${ARP_DETOURS_DIR}/detours/zydis/include"
            "${ARP_DETOURS_DIR}/detours/zydis/dependencies/zycore/include")
# Preserve upstream's decoder amalgamation and symbol isolation. There is no
# separate Zydis library or second decoder implementation linked into the plugin.
target_compile_definitions(ARPDetours PRIVATE ZYDIS_STATIC_DEFINE WIN32 _LIB)
set_target_properties(ARPDetours PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
if(MSVC)
    target_compile_options(ARPDetours PRIVATE /W3 /Z7)
endif()
