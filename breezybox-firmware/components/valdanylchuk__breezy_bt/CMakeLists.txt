# Local override of the managed valdanylchuk/breezy_bt component.
#
# Two reasons this lives in components/ rather than managed_components/:
#
#  1. it carries a local addition, bt_keyboard_has_saved_target() (LOCAL_PATCH.md)
#  2. it must compile to nothing when Bluetooth is off
#
# The slim ESP-Claw profile builds with CONFIG_BT_ENABLED=n to free ~33 KB of
# internal SRAM for the TLS handshake, which removes esp_bt.h and would make
# bt_keyboard.c fail to compile. EXCLUDE_COMPONENTS cannot help: the component
# manager re-injects anything named in main/idf_component.yml regardless of it.
# Registering with an empty SRCS list is what actually works.
#
# REQUIRES must stay outside the conditional. IDF evaluates this file once in
# an "early expansion" pass to collect requirements, before CONFIG_* exists --
# so a REQUIRES inside if(CONFIG_BT_ENABLED) silently resolves to empty and the
# sources then compile without their include paths.
set(BREEZY_BT_SRCS "")
if(CONFIG_BT_ENABLED)
    set(BREEZY_BT_SRCS "bt_keyboard.c")
endif()

idf_component_register(
    SRCS ${BREEZY_BT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES bt esp_hid nvs_flash
)
