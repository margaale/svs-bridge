# USB host, mDNS and ArduinoJson come in through idf_component.yml
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
                       INCLUDE_DIRS "."
                       EMBED_FILES "web/index.html" "web/portal.html"
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
