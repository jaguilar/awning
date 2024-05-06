add_library(rfm_ook rfm_ook.cc)
target_link_libraries(rfm_ook PUBLIC pico_stdlib hardware_spi hardware_gpio)
target_compile_features(rfm_ook PRIVATE cxx_std_23)

add_pico_executable(rfm_ook_test rfm_ook_test.cc)
target_link_libraries(rfm_ook_test PRIVATE rfm_ook pico_stdlib pico_time somfy)

add_pico_executable(awning awning.cc)
target_link_libraries(awning common jagsarduino somfy hardware_flash)