idf_component_register(
    SRCS "onewire.c" "onewire_gpio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES esp_rom)
