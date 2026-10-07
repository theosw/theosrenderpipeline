# Checks the packaged Community Shaders settings override. CS reads
# {ModName}_{FeatureShortName}.json from SKSE/Plugins/CommunityShaders/Overrides,
# merges it over the user's saved feature settings at startup and ignores keys
# starting with "_". The file must keep CS frame generation and Reflex off and
# leave every other Upscaling setting to the user.

if(NOT DEFINED OVERRIDE_FILE OR NOT EXISTS "${OVERRIDE_FILE}")
    message(FATAL_ERROR "OVERRIDE_FILE is missing: ${OVERRIDE_FILE}")
endif()

get_filename_component(name "${OVERRIDE_FILE}" NAME)
if(NOT name STREQUAL "TheosRenderPipeline_Upscaling.json")
    message(FATAL_ERROR "CS matches overrides to features by file name; got ${name}")
endif()

file(READ "${OVERRIDE_FILE}" json)

string(JSON type ERROR_VARIABLE error TYPE "${json}")
if(error OR NOT type STREQUAL "OBJECT")
    message(FATAL_ERROR "Override must be a JSON object: ${error}")
endif()

set(expected
    "frameGenerationMode|NUMBER|0"
    "frameGenerationForceEnable|NUMBER|0"
    "reflexLowLatencyMode|BOOLEAN|OFF"
    "reflexLowLatencyBoost|BOOLEAN|OFF")

string(JSON count LENGTH "${json}")
math(EXPR last "${count} - 1")
set(settings 0)
foreach(index RANGE ${last})
    string(JSON key MEMBER "${json}" ${index})
    if(key MATCHES "^_")
        continue()
    endif()
    math(EXPR settings "${settings} + 1")
    set(known FALSE)
    foreach(entry IN LISTS expected)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 expectedKey)
        list(GET parts 1 expectedType)
        list(GET parts 2 expectedValue)
        if(key STREQUAL expectedKey)
            set(known TRUE)
            string(JSON actualType TYPE "${json}" "${key}")
            string(JSON actualValue GET "${json}" "${key}")
            if(NOT actualType STREQUAL expectedType OR NOT actualValue STREQUAL expectedValue)
                message(FATAL_ERROR "${key} is ${actualType} ${actualValue}; expected ${expectedType} ${expectedValue}")
            endif()
        endif()
    endforeach()
    if(NOT known)
        message(FATAL_ERROR "Unexpected setting ${key}; leave other CS Upscaling settings to the user")
    endif()
endforeach()

list(LENGTH expected expectedCount)
if(NOT settings EQUAL expectedCount)
    message(FATAL_ERROR "Expected ${expectedCount} settings, found ${settings}")
endif()

string(JSON modName ERROR_VARIABLE error GET "${json}" _metadata modName)
string(JSON enabled ERROR_VARIABLE enabledError GET "${json}" _metadata enabled)
if(error OR enabledError OR modName STREQUAL "" OR NOT enabled STREQUAL "ON")
    message(FATAL_ERROR "_metadata needs a modName and enabled=true")
endif()
