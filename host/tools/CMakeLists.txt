add_executable(fades_run fades_run.c)
set_target_properties(fades_run PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
target_include_directories(fades_run PRIVATE "${PROJECT_SOURCE_DIR}/host/driver/include")
target_compile_options(fades_run PRIVATE -Wall -Wextra -Werror)
