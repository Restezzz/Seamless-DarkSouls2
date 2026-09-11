# Original full-featured launcher
add_executable(ds2_seamless_coop_launcher
    main.cpp
)

target_include_directories(ds2_seamless_coop_launcher PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

set_target_properties(ds2_seamless_coop_launcher PROPERTIES
    OUTPUT_NAME "DS2SeamlessCoopLauncher"
    WIN32_EXECUTABLE OFF
)

target_link_libraries(ds2_seamless_coop_launcher PRIVATE
    Shlwapi
)

# Simple auto-launch version (just launches game with mod)
add_executable(ds2_auto_launcher
    simple_launcher.cpp
)

set_target_properties(ds2_auto_launcher PROPERTIES
    OUTPUT_NAME "PlayDS2WithMod"
    WIN32_EXECUTABLE OFF
)

target_link_libraries(ds2_auto_launcher PRIVATE
    Shlwapi
)

