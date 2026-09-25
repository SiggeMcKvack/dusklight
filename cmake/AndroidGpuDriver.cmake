include_guard(GLOBAL)

# Custom GPU driver support (arm64 only): libadrenotools plus the libvulkan.so shim for Dawn. The
# extra shared objects are listed in dusk-android.properties for Gradle to stage into the APK.
function(setup_android_gpu_driver target internal_target)
    if (NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
        message(STATUS "dusklight: custom GPU driver support disabled (requires Android arm64-v8a)")
        return()
    endif ()

    include(FetchContent)
    message(STATUS "dusklight: Fetching libadrenotools")
    FetchContent_Declare(adrenotools
        GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
        GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
        GIT_PROGRESS TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(adrenotools)
    if (NOT TARGET adrenotools)
        message(FATAL_ERROR "Failed to make libadrenotools available")
    endif ()

    add_library(dusk_vulkan_shim SHARED ${CMAKE_SOURCE_DIR}/src/dusk/android/vulkan_shim.c)
    set_target_properties(dusk_vulkan_shim PROPERTIES C_VISIBILITY_PRESET hidden)

    set(_hook_targets main_hook hook_impl file_redirect_hook gsl_alloc_hook)
    target_link_libraries(${internal_target} PRIVATE adrenotools)
    target_compile_definitions(${internal_target} PRIVATE DUSK_HAS_GPU_DRIVER=1)
    target_compile_definitions(${target} PRIVATE DUSK_HAS_GPU_DRIVER=1)
    add_dependencies(${target} dusk_vulkan_shim ${_hook_targets})

    set(_lines "dusk.extraNativeLibraries=$<TARGET_FILE:dusk_vulkan_shim>")
    foreach (_hook IN LISTS _hook_targets)
        string(APPEND _lines ",$<TARGET_FILE:${_hook}>")
    endforeach ()
    set(_properties "${CMAKE_BINARY_DIR}/dusk-android.properties")
    file(GENERATE OUTPUT "${_properties}" CONTENT "${_lines}\n")
    message(STATUS "dusklight: Android GPU driver properties: ${_properties}")
endfunction()
