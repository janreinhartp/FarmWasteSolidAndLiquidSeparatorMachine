SET(SOURCES screens/ui_scrSplashScreen.c
    screens/ui_scrRunAuto.c
    screens/ui_scrMainMenu.c
    screens/ui_scrSettings.c
    screens/ui_scrTestMachine.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_1396634952.c
    images/ui_img_1839083635.c
    images/ui_img_process_flow_water_sepparator_png.c)

add_library(ui ${SOURCES})
