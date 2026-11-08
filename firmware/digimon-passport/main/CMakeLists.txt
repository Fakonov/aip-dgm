# The ROMs are NOT embedded any more: they live in the 'roms' data partition
# and are uploaded over the USB console (ROMIN).  See main/dg_rom_store.h.
# main/roms/*.bin stays in the tree as the images the PC tool uploads; nothing
# from that directory ends up inside the app binary.
# D-Link battles are wired only (round 91): the PC relays frames over USB.
# There is no ESP-NOW / Wi-Fi dependency in the app any more.
idf_component_register(
    SRCS "main.c" "digimon_app.c" "core/digimon_core.c" "dg_rom_store.c" "dg_link.c" "dg_link_session.c" "dg_cards.c"
    INCLUDE_DIRS "." "core"
    REQUIRES bsp nvs_flash esp_timer esp_partition heap driver
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE DG_HOST_ESP32=1)

# Verification builds only: the wasm emulator models UART and no
# USB-Serial-JTAG, so the console has to be read from UART0 there.
# Leave OFF for anything that goes onto the board -- UART0 TX is the backlight pin.
option(DG_SIM_UART0_INPUT "Read the console from UART0 (simulator builds)" OFF)
if(DG_SIM_UART0_INPUT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DG_SIM_UART0_INPUT=1)
    message(STATUS "digimon: console on UART0 (simulator verification build)")
endif()
