get_filename_component(SVS_BRIDGE_ROOT ${COMPONENT_DIR}/../../../.. ABSOLUTE)
set(CORE ${SVS_BRIDGE_ROOT}/src/core)

idf_component_register(SRCS "main.cpp"
                            "svs_usb.cpp"
                            "wifi_manager.cpp"
                            "dns_server.cpp"
                            "tls_cert.cpp"
                            "web_server.cpp"
                            "factory_reset.cpp"
                            "auth.cpp"
                            "svs_flasher.cpp"
                            "svs_fw_repo.cpp"
                            "bridge_fw_repo.cpp"
                       INCLUDE_DIRS "." ${CORE}
                       PRIV_REQUIRES app_update
                                     bootloader_support
                                     esp_driver_gpio
                                     esp_driver_uart
                                     esp_event
                                     esp_http_client
                                     esp_http_server
                                     esp_https_server
                                     esp_netif
                                     esp_timer
                                     esp_wifi
                                     lwip
                                     mbedtls
                                     nvs_flash)

# The pages, embedded as C arrays (as in Cruller, see src/web/embed.cmake).
foreach(asset index.html:web_index_html portal.html:web_portal_html)
    string(REPLACE ":" ";" asset ${asset})
    list(GET asset 0 file)
    list(GET asset 1 name)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}.c)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -DIN=${SVS_BRIDGE_ROOT}/src/web/${file} -DOUT=${out} -DNAME=${name}
            -P ${SVS_BRIDGE_ROOT}/src/web/embed.cmake
        DEPENDS ${SVS_BRIDGE_ROOT}/src/web/${file} ${SVS_BRIDGE_ROOT}/src/web/embed.cmake
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE ${out})
endforeach()
