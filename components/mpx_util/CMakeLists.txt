# mpx_util -- the log and trace rings.
#
# Both deliberately live in INTERNAL RAM, not PSRAM: the log is most valuable
# when something is wrong, and "something is wrong" includes PSRAM failing to
# come up.
#
# Their producers and consumers sit in different components -- mpx_wasm writes
# traces, the HTTP layer reads both -- so they live here rather than in either,
# which keeps those two out of a dependency cycle.

idf_component_register(
    SRCS
        "src/log_ring.cc"
        "src/trace_ring.cc"
        "src/mpx_rings.cc"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_timer
)
