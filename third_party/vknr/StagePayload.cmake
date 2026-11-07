# Stage only the reviewed runtime shader payload; compiler logs and intermediate
# SPIR-V stay in the build tree. No loader DLL or model data belongs here.
file(GLOB_RECURSE payload RELATIVE "${INPUT}" "${INPUT}/*.spv" "${INPUT}/*.txt")
list(FILTER payload EXCLUDE REGEX "\\.unoptimized\\.spv$")
foreach(relative IN LISTS payload)
    get_filename_component(directory "${OUTPUT}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    configure_file("${INPUT}/${relative}" "${OUTPUT}/${relative}" COPYONLY)
endforeach()
