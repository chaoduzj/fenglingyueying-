set(GAME_NAME "Crab_Champions")
set(TRAINER_NAME "Crab Champions Trainer")
set(TRAINER_ARCH "x64")
set(UE_TARGET "${GAME_NAME}_UE")

set(CRAB_CHAMPIONS_UE_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/CppSDK")

set(VENV_PYTHON "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
set(SUBSET_FONT_TARGET "${GAME_NAME}_SubsetFont")
set(FONT_SUBSET_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/NotoSansSC-Subset.ttf")
set(FONT_SUBSET_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/font_processor.py")
set(UE_SDK_PRUNE_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/prune_cpp_sdk.py")
set(FONT_FILE "${CMAKE_SOURCE_DIR}/scripts/NotoSansSC-Regular.ttf")
set(TRANSLATION_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/translations.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/translation_extra.txt"
)

add_custom_command(
    OUTPUT ${FONT_SUBSET_OUTPUT}
    COMMAND ${VENV_PYTHON} ${FONT_SUBSET_SCRIPT} ${TRANSLATION_FILES}
            --font ${FONT_FILE} --output ${FONT_SUBSET_OUTPUT}
    DEPENDS ${FONT_SUBSET_SCRIPT} ${FONT_FILE} ${TRANSLATION_FILES}
    COMMENT "Subsetting NotoSansSC-Regular.ttf in ${CMAKE_CURRENT_SOURCE_DIR}"
)

add_custom_target(${SUBSET_FONT_TARGET} ALL
    DEPENDS ${FONT_SUBSET_OUTPUT}
)

if(NOT EXISTS "${CRAB_CHAMPIONS_UE_SDK_DIR}/SDK/Basic.hpp")
    message(FATAL_ERROR "Dumper-7 SDK not found: ${CRAB_CHAMPIONS_UE_SDK_DIR}")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MINHOOK_LIB "${CMAKE_SOURCE_DIR}/common/libs/x64/libMinHook.x64.lib")
else()
    set(MINHOOK_LIB "${CMAKE_SOURCE_DIR}/common/libs/x86/libMinHook.x86.lib")
endif()

# Compile this game's Unreal bridge and the generated Dumper-7 SDK code.
add_library(${UE_TARGET} SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/UE.cpp"
    "${CRAB_CHAMPIONS_UE_SDK_DIR}/SDK/Basic.cpp"
    "${CRAB_CHAMPIONS_UE_SDK_DIR}/SDK/CoreUObject_functions.cpp"
    "${CRAB_CHAMPIONS_UE_SDK_DIR}/SDK/Engine_functions.cpp"
    "${CRAB_CHAMPIONS_UE_SDK_DIR}/SDK/CrabChampions_functions.cpp"
)
target_include_directories(${UE_TARGET} PRIVATE "${CRAB_CHAMPIONS_UE_SDK_DIR}")
target_link_libraries(${UE_TARGET} PRIVATE Common ${MINHOOK_LIB})
target_compile_features(${UE_TARGET} PRIVATE cxx_std_20)
target_compile_definitions(${UE_TARGET} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    # Dumper-7 SDK translation units are large enough to need /bigobj.
    target_compile_options(${UE_TARGET} PRIVATE /bigobj /MP4 /W0)
endif()

set_target_properties(${UE_TARGET} PROPERTIES
    OUTPUT_NAME "UE"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

# Keep the built DLL beside resources.rc so the trainer can embed it.
add_custom_command(TARGET ${UE_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${UE_TARGET}>"
        "${CMAKE_CURRENT_SOURCE_DIR}/UE.dll"
    COMMENT "Moving UE.dll to ${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
)

if(MSVC)
    add_custom_command(TARGET ${UE_TARGET} POST_BUILD
        COMMAND ${VENV_PYTHON} ${UE_SDK_PRUNE_SCRIPT}
            "${CRAB_CHAMPIONS_UE_SDK_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/${UE_TARGET}.dir/$<CONFIG>"
            --configuration "$<CONFIG>"
        COMMENT "Pruning unused Dumper-7 CppSDK files after the Release build"
        VERBATIM
    )
endif()

add_executable(${GAME_NAME} main.cpp)
target_link_libraries(${GAME_NAME} PRIVATE Common)
add_dependencies(${GAME_NAME} ${SUBSET_FONT_TARGET} ${UE_TARGET})
set_target_properties(${GAME_NAME} PROPERTIES
    WIN32_EXECUTABLE $<$<NOT:$<CONFIG:Debug>>:ON>
    OUTPUT_NAME ${TRAINER_NAME}
)

if (WIN32)
    set(RESOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources.rc")
    target_sources(${GAME_NAME} PRIVATE ${RESOURCE_FILE})
    set_source_files_properties(${RESOURCE_FILE} PROPERTIES
        LANGUAGE RC
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/UE.dll"
    )
endif()

add_custom_command(TARGET ${GAME_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${GAME_NAME}>" "${CMAKE_BINARY_DIR}/bin/${TRAINER_NAME}/${TRAINER_NAME}.exe"
    COMMENT "Moving ${TRAINER_NAME}.exe to ${CMAKE_BINARY_DIR}/bin/${TRAINER_NAME}"
)
