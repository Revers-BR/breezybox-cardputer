cmake_minimum_required(VERSION 3.16)

set(BREEZY_BOARD "cardputer" CACHE STRING "Target BreezyBox board")
set_property(CACHE BREEZY_BOARD PROPERTY STRINGS cardputer cardputer-adv cardputer-claw sticks3)

if(BREEZY_BOARD STREQUAL "sticks3")
    add_compile_definitions(
        BREEZY_BOARD_STICKS3=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        BREEZY_VTERM_COUNT=1
        BREEZY_VTERM_SCROLLBACK_LINES=64
    )
elseif(BREEZY_BOARD STREQUAL "cardputer-claw")
    # Slim ESP-Claw profile: same hardware as "cardputer", but Bluetooth, SSH and
    # the ELF loader are removed to free internal SRAM for the TLS session.
    # See docs/claw.md for the measurements that motivate this.
    add_compile_definitions(
        BREEZY_BOARD_CARDPUTER=1
        BREEZY_BOARD_CARDPUTER_UNIVERSAL=1
        BREEZY_SLIM=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        # One terminal, short scrollback. A vterm cell is 2 bytes, so each VT
        # costs (40*16 + scrollback*40) * 2 bytes: 8,960 B at 2 VTs x 96 lines,
        # versus 3,840 B here -- about 14 KB back for the TLS handshake, which
        # is the scarcest resource on this profile.
        BREEZY_VTERM_COUNT=1
        BREEZY_VTERM_SCROLLBACK_LINES=32
    )
elseif(BREEZY_BOARD STREQUAL "cardputer-adv")
    add_compile_definitions(
        BREEZY_BOARD_CARDPUTER=1
        BREEZY_BOARD_CARDPUTER_ADV=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        BREEZY_VTERM_COUNT=2
        BREEZY_VTERM_SCROLLBACK_LINES=96
    )
elseif(BREEZY_BOARD STREQUAL "cardputer")
    add_compile_definitions(
        BREEZY_BOARD_CARDPUTER=1
        BREEZY_BOARD_CARDPUTER_UNIVERSAL=1
        BREEZY_DISPLAY_COLS=40
        BREEZY_DISPLAY_ROWS=16
        BREEZY_VTERM_COLS=40
        BREEZY_VTERM_ROWS=16
        BREEZY_VTERM_COUNT=2
        BREEZY_VTERM_SCROLLBACK_LINES=96
    )
endif()

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../breezybox-cardputer
)


include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(breezybox_cardputer)

set(BREEZY_FS_IMAGE_DIR ${CMAKE_CURRENT_LIST_DIR}/fs_image)

file(MAKE_DIRECTORY ${BREEZY_FS_IMAGE_DIR})

# Stage the espclaw Lua package into the filesystem image so it ships inside the
# firmware .bin rather than having to be copied onto the device by hand.
# packages/espclaw/ stays the canonical source; this only mirrors it, so the
# files are not duplicated in git. Runs at configure time, so `idf.py
# reconfigure` (which `make build` does) picks up edits.
# The agent core is now native C (breezybox-cardputer/claw/); only the pinned
# CA still ships on the filesystem, because claw_agent.c reads it at runtime.
set(ESPCLAW_PKG ${CMAKE_CURRENT_LIST_DIR}/../packages/espclaw/root/apps/espclaw)
if(EXISTS ${ESPCLAW_PKG}/ca)
    file(COPY ${ESPCLAW_PKG}/ca DESTINATION ${BREEZY_FS_IMAGE_DIR}/apps/espclaw)
endif()
if(EXISTS ${ESPCLAW_PKG}/lua_api.md)
    file(COPY ${ESPCLAW_PKG}/lua_api.md DESTINATION ${BREEZY_FS_IMAGE_DIR}/apps/espclaw)
endif()
if(EXISTS ${ESPCLAW_PKG}/models.json)
    # Shipped default only: it is copied to /sd/claw/models.json on first run,
    # and that copy is what users edit.
    file(COPY ${ESPCLAW_PKG}/models.json DESTINATION ${BREEZY_FS_IMAGE_DIR}/apps/espclaw)
endif()
# The bundled Lua examples, which docs/lua.md has always said live in
# /root/lua. They did not actually ship until now -- and the agent uses them as
# worked examples, since a runnable script answers "how do I use breezy.gfx"
# better than a list of function names.
set(BREEZY_LUA_APPS ${CMAKE_CURRENT_LIST_DIR}/../lua-apps)
if(EXISTS ${BREEZY_LUA_APPS})
    file(GLOB BREEZY_LUA_APP_FILES ${BREEZY_LUA_APPS}/*.lua ${BREEZY_LUA_APPS}/*.txt)
    file(COPY ${BREEZY_LUA_APP_FILES} DESTINATION ${BREEZY_FS_IMAGE_DIR}/lua)
    list(LENGTH BREEZY_LUA_APP_FILES BREEZY_LUA_APP_COUNT)
    message(STATUS "espclaw: staged CA, model catalogue, Lua API and "
                   "${BREEZY_LUA_APP_COUNT} example scripts into fs_image")
else()
    message(STATUS "espclaw: staged CA, model catalogue and Lua API into fs_image")
endif()

littlefs_create_partition_image(spiffs ${BREEZY_FS_IMAGE_DIR} FLASH_IN_PROJECT)

# Copy our custom symbol table into the elf_loader managed component.
# Runs every build; copy_if_different avoids needless recompilation.
if(NOT BREEZY_BOARD STREQUAL "cardputer-claw")
add_custom_target(copy_all_symbols ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_DIR}/main/all_my_symbols.c
        ${PROJECT_DIR}/components/espressif__elf_loader/src/esp_all_symbol.c
    COMMENT "Copying all_my_symbols.c -> esp_all_symbol.c"
)
# Ensure the copy finishes before the elf_loader component compiles.
if(TARGET __idf_espressif__elf_loader)
    add_dependencies(__idf_espressif__elf_loader copy_all_symbols)
endif()
endif()
