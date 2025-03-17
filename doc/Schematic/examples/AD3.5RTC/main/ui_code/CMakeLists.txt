SET(SOURCES screens/ui_Screen1.c
    screens/ui_configwifi.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_daobai_icon_png.c
    fonts/ui_font_Big.c
    fonts/ui_font_Medium.c
    fonts/ui_font_Number.c
    fonts/ui_font_Small.c
    fonts/ui_font_Font22.c
    fonts/ui_font_Font32.c)

add_library(ui ${SOURCES})
