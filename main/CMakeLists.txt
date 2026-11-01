idf_component_register(SRCS "main.c" "board.c" "fan.c" "sensors.c" "wifi.c" "web.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_wifi esp_netif
                                     esp_event esp_http_server esp_timer esp_app_format
                                     app_update nvs_flash bt)

# Gzip the web UI at build time and embed it in the firmware image
idf_build_get_property(python PYTHON)
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${WEB_GZ}"
                   COMMAND ${python} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py" "${WEB_SRC}" "${WEB_GZ}"
                   DEPENDS "${WEB_SRC}"
                   VERBATIM)
add_custom_target(web_gz DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} web_gz)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
