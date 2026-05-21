include_directories(../../de1soc_io)

add_executable(gpio_toggle gpio_toggle.c)

target_link_libraries(gpio_toggle pthread de1soc_io)