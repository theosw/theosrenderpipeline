set(ARP_RELEASE_VERSION "${PROJECT_VERSION}")
set(ARP_RELEASE_LABEL "${ARP_RELEASE_VERSION}")
string(REPLACE "." ", " ARP_RELEASE_NUMERIC "${ARP_RELEASE_VERSION}")

# Keep diagnostic locations useful without embedding the build machine's paths.
# MSVC requires deterministic compilation for /pathmap, including /FC locations.
if(MSVC)
    file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}" arp_source_prefix)
    set(arp_path_options /experimental:deterministic "/pathmap:${arp_source_prefix}=.")
    # CommonLib may be supplied outside this checkout and embeds source locations.
    if(DEFINED TRP_COMMONLIBSSE_NG_DIR)
        file(TO_NATIVE_PATH "${TRP_COMMONLIBSSE_NG_DIR}" arp_commonlib_prefix)
        list(APPEND arp_path_options "/pathmap:${arp_commonlib_prefix}=./.dependencies/CommonLibSSE-NG")
    endif()
    add_compile_options(${arp_path_options})
    # Directory options initialize later targets; the plugin already exists.
    target_compile_options(${PROJECT_NAME} PRIVATE ${arp_path_options})
    target_link_options(${PROJECT_NAME} PRIVATE "/PDBALTPATH:TheosRenderPipeline.pdb")
endif()
