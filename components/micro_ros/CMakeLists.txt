# micro-ROS client libraries (ROS 2 Jazzy, Micro XRCE-DDS over UDP), PREBUILT.
#
# libmicroros.a and include/ come from micro_ros_espidf_component (jazzy) built
# on Linux against mpv2-uros: same ESP-IDF (5.5), same sdkconfig, the sizes
# in ../../app-colcon.meta. Building them needs colcon and a Linux shell, which
# a Windows `idf.py build` does not have, so they are kept here ready-made.
# To rebuild (Linux or WSL2): tools/uros/rebuild_microros.sh -- see
# its header (micro-ROS is off on the rl branch; see the ros branch).
idf_component_register(REQUIRES lwip LDFRAGMENTS linker.lf)

add_prebuilt_library(libmicroros-prebuilt "${COMPONENT_DIR}/libmicroros.a" REQUIRES lwip)
target_include_directories(libmicroros-prebuilt INTERFACE "${COMPONENT_DIR}/include")
file(GLOB _uros_pkgs LIST_DIRECTORIES true "${COMPONENT_DIR}/include/*")
foreach(_d IN LISTS _uros_pkgs)
    if(IS_DIRECTORY "${_d}")
        target_include_directories(libmicroros-prebuilt INTERFACE "${_d}")
    endif()
endforeach()
target_link_libraries(${COMPONENT_LIB} INTERFACE libmicroros-prebuilt)
