idf_component_register(
    SRCS "onewire_sim.c"
    INCLUDE_DIRS "include"
    REQUIRES onewire
    PRIV_REQUIRES ds18b20)
