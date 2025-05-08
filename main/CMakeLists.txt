file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

idf_component_register(SRCS ${SOURCES}
    INCLUDE_DIRS "ui"
    PRIV_INCLUDE_DIRS "priv_include")

target_compile_options(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)