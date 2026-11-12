set(http_panel_srcs
    "http_panel.c"
    "core/http_panel_server.c"
    "core/http_panel_transfer.c"
    "pages/dashboard/dashboard.c"
    "pages/web_console/web_console.c"
    "pages/ota/ota_page.c"
    "pages/camera/camera_page.c"
    "pages/data_manager/data_manager_page.c"
    "pages/sysinfo/sysinfo_page.c"
    "pages/common/common_page.c"
    "pages/network/network_page.c"
    "pages/files/files_page.c"
    "providers/http_panel_providers.c"
)

set(http_panel_assets
    "pages/dashboard/dashboard.html"
    "pages/web_console/web_console.html"
    "pages/ota/ota_page.html"
    "pages/camera/camera_page.html"
    "pages/data_manager/data_manager_page.html"
    "pages/sysinfo/sysinfo_page.html"
    "pages/common/panel.css"
    "pages/network/network_page.html"
    "pages/files/files_page.html"
)

set(http_panel_include_dirs
    "."
    "core"
    "providers"
    "pages/dashboard"
    "pages/web_console"
    "pages/ota"
    "pages/camera"
    "pages/data_manager"
    "pages/sysinfo"
    "pages/common"
    "pages/network"
    "pages/files"
)

set(http_panel_priv_requires
    camera
    common
    data
    board
    esp_wifi
    esp_netif
    esp_timer
    esp_app_format
    app_update
    spi_flash
    esp_hw_support
    esp_psram
    sysmon
    net_diag
    heap
    lwip
    system
    sensors
    storage
)

# The tab bar is part of panel.js, so a board without smart_io gets a copy
# without the IO tab, and no IO page, sources or dependency.
set(panel_js_source "${CMAKE_CURRENT_LIST_DIR}/pages/common/panel.js")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${panel_js_source}")
file(READ "${panel_js_source}" panel_js_content)

idf_build_get_property(board_has_smart_io BOARD_HAS_SMART_IO)

if(board_has_smart_io)
    list(APPEND http_panel_srcs
        "pages/smart_io/smart_io_page.c"
        "pages/smart_io/smart_io_ws.c"
    )
    list(APPEND http_panel_assets "pages/smart_io/smart_io_page.html")
    list(APPEND http_panel_include_dirs "pages/smart_io")
    list(APPEND http_panel_priv_requires smart_io)
else()
    string(REGEX REPLACE "  \\['/io',[^\n]*\n" "" panel_js_content "${panel_js_content}")
endif()

set(panel_js_embedded "${CMAKE_CURRENT_BINARY_DIR}/panel.js")
file(GENERATE OUTPUT "${panel_js_embedded}" CONTENT "${panel_js_content}")
list(APPEND http_panel_assets "${panel_js_embedded}")

idf_component_register(
    SRCS ${http_panel_srcs}
    EMBED_FILES ${http_panel_assets}
    INCLUDE_DIRS ${http_panel_include_dirs}
    REQUIRES
        esp_http_server
        shell
        cjson
        log
        network
        freertos
        esp_event
        ota
    PRIV_REQUIRES ${http_panel_priv_requires}
)
