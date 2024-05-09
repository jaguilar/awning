add_library(pico_rolling_code_storage pico_rolling_code_storage.cc pico_rolling_code_storage.h)
target_link_libraries(pico_rolling_code_storage PUBLIC pico_flash somfy jagsarduino hardware_flash)

add_executable(pico_rolling_code_storage_test pico_rolling_code_storage_test.cc)
target_link_libraries(pico_rolling_code_storage_test PUBLIC pico_rolling_code_storage pico_stdlib somfy hardware_flash pico_flash)

add_library(rfm69 rfm69.cc)
target_link_libraries(rfm69 PUBLIC pico_stdlib hardware_spi hardware_gpio)
target_compile_features(rfm69 PRIVATE cxx_std_23)

add_pico_executable(rfm69_test rfm69_test.cc)
target_link_libraries(rfm69_test PRIVATE rfm69 pico_stdlib pico_time somfy)

add_pico_executable(awning awning.cc)
target_link_libraries(awning common jagsarduino somfy pico_rolling_code_storage pico_stdlib rfm69 pico_lwip_mqtt)
target_compile_options(awning PRIVATE -DSOMFY_RADIO_ADDRESS=0x12345678 -DMQTT_HOST="${MQTT_HOST}" -DMQTT_USER="${MQTT_USER}" -DMQTT_PASSWORD="${MQTT_PASSWORD}" -DMQTT_CLIENT_ID="awning_controller" -DMQTT_TOPIC="/homeautomation/awning/weathercontrol")