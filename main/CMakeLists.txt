# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.c         # list the source files of this component
    INCLUDE_DIRS        # optional, add here public include directories
        "."
    PRIV_INCLUDE_DIRS   # optional, add here private include directories
    REQUIRES            # optional, list the public requirements (component names)
        fatfs
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_spi
        esp_timer
        libhelix-mp3
    PRIV_REQUIRES       # optional, list the private requirements
)
