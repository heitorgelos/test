idf_component_register(
    SRCS "main.c"  # ou o nome do seu arquivo principal
    PRIV_REQUIRES driver freertos spi_flash sdmmc fatfs
    INCLUDE_DIRS "."
)
