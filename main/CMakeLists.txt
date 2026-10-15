idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
)

# ESP-Claw's app_claw (submodule, not edited) starts its event router with an
# 8 KB stack, which this robot overflows. The linker redirects that one init
# call to __wrap_claw_event_router_init() in claw_overrides.c, which raises
# task_stack_size to CONFIG_MP4_EVENT_ROUTER_STACK and calls the real init.
if(CONFIG_MP4_ROBOT_ENABLE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=claw_event_router_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_claw_event_router_init")
endif()
