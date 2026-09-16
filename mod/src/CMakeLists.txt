add_library(ds2_seamless_coop SHARED
    core/dllmain.cpp
    core/mod.cpp
    hooks/session_hooks.cpp
    hooks/network_hooks.cpp
    hooks/game_state_hooks.cpp
    network/peer_manager.cpp
    network/packet_handler.cpp
    session/session_manager.cpp
    sync/progress_sync.cpp
    sync/player_sync.cpp
    sync/world_sync.cpp
    sync/loot_sync.cpp
    sync/summon_accept.cpp
    sync/free_travel.cpp
    sync/death_sync.cpp
    sync/npc_talk.cpp
    sync/pvp_modes.cpp
    sync/join_direct.cpp
    sync/chr_death.cpp
    sync/guest_world.cpp
    sync/travel_sync.cpp
    sync/mp_gates.cpp
    sync/guest_drops.cpp
    utils/logger.cpp
    utils/crash_log.cpp
    utils/memory.cpp
    utils/pattern_scanner.cpp
    utils/address_resolver.cpp
    ui/overlay.cpp
    ui/renderer.cpp
    ui/title_notifier.cpp
    ui/overlay_hud.cpp
    ui/input_capture.cpp
    ui/ui_settings.cpp
    ui/ui_theme.cpp
    ui/ui_widgets.cpp
)

target_include_directories(ds2_seamless_coop PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${THIRD_PARTY_DIR}/minhook/include
    ${THIRD_PARTY_DIR}/imgui
    ${THIRD_PARTY_DIR}/imgui/backends
)

target_link_libraries(ds2_seamless_coop PRIVATE
    minhook
    imgui
    ws2_32
    d3d11
    dxgi
)

# Set output name and compiler flags
set_target_properties(ds2_seamless_coop PROPERTIES
    OUTPUT_NAME "ds2_seamless_coop"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Static link the C++ runtime to avoid dependency issues
target_compile_options(ds2_seamless_coop PRIVATE
    /utf-8
    $<$<CONFIG:Release>:/MT>
    $<$<CONFIG:Debug>:/MTd>
)

# Symbols for the release build too: the crash log reports dinput8.dll+offset,
# and the PDB kept per build (symbols/<md5>) turns that into a function name.
target_compile_options(ds2_seamless_coop PRIVATE $<$<CONFIG:Release>:/Zi>)
target_link_options(ds2_seamless_coop PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
# Only the PDB's file name goes into the DLL, not the builder's local path.
# Debuggers match the PDB by its GUID, so symbols/<md5> keeps working.
# The name is spelled out: the usual %_PDB% reached the linker mangled.
target_link_options(ds2_seamless_coop PRIVATE $<$<CONFIG:Release>:/PDBALTPATH:ds2_seamless_coop.pdb>)

# Version info in the file's properties, filled in from the project version
configure_file(version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
target_sources(ds2_seamless_coop PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)

# Also build as dinput8.dll (drop-in auto-load, no launcher needed)
add_custom_command(TARGET ds2_seamless_coop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    $<TARGET_FILE:ds2_seamless_coop>
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/dinput8.dll
)

