# Decide whether pwa-claw needs `npm run build` before ESP-IDF embeds its
# output (pwa-claw/www) into the firmware.
#
# Invoked from two places, both of which run it *before* the build graph is
# evaluated:
#
#   * idf_ext.py, as a global idf.py action callback for build/flash/app-flash
#     (ninja decides what is out of date once, when it starts, so a web build
#     that ran inside the graph could only be relinked by the *next* build --
#     i.e. the firmware being flashed would embed the previous web app);
#   * the `mpx_build_pwa` CMake target, for plain `cmake --build` / `ninja`
#     users, where that extension does not run.
#
# Either way the check is cheap and idempotent: `npm run build` rewrites every
# file in www/ with a fresh mtime, and each of those files is a MAIN_DEPENDENCY
# of a generated *.S embed object. Any build that runs npm therefore
# re-assembles ~5 MB of embedded assets and relinks the app -- over two minutes
# here, for a C change that never touched the phone app. Skipping npm leaves
# www/ untouched, so ninja keeps the existing embed objects and goes straight to
# flashing.
#
# Staleness is tracked with a stamp file in build/ that holds the newest mtime
# (epoch seconds) of the PWA inputs at the moment npm last ran. Inputs are
# pwa-claw/src, pwa-claw/public, the bundler config files and the lockfile.
#
# Environment knobs:
#   MPX_FORCE_PWA=1   rebuild the phone app even if the stamp says it is current
#   MPX_SKIP_PWA=1    never run npm; embed whatever pwa-claw/www already holds
#
# Required -D variables: MPX_PWA_DIR, MPX_NPM_EXECUTABLE, MPX_PWA_STAMP.
# Optional: -DMPX_PWA_QUIET=ON silences the "nothing to do" message.

foreach(mpx_required MPX_PWA_DIR MPX_NPM_EXECUTABLE MPX_PWA_STAMP)
    if(NOT DEFINED ${mpx_required})
        message(FATAL_ERROR "pwa_build.cmake: -D${mpx_required}=... is required")
    endif()
endforeach()

set(mpx_www_dir "${MPX_PWA_DIR}/www")
set(mpx_www_index "${mpx_www_dir}/index.html")
set(mpx_node_modules "${MPX_PWA_DIR}/node_modules")
set(mpx_installed_lock "${mpx_node_modules}/.package-lock.json")
set(mpx_package_lock "${MPX_PWA_DIR}/package-lock.json")

# ── environment knobs ───────────────────────────────────────────────────────
foreach(mpx_pair "MPX_FORCE_PWA;mpx_force" "MPX_SKIP_PWA;mpx_skip")
    list(GET mpx_pair 0 mpx_env_name)
    list(GET mpx_pair 1 mpx_var_name)
    string(TOLOWER "$ENV{${mpx_env_name}}" mpx_env_value)
    if(mpx_env_value MATCHES "^(1|on|true|yes)$")
        set(${mpx_var_name} TRUE)
    endif()
endforeach()

if(mpx_skip)
    message(STATUS "pwa-claw: MPX_SKIP_PWA is set -- embedding ${mpx_www_dir} as it is on disk")
    return()
endif()

# ── inputs ──────────────────────────────────────────────────────────────────
# No CONFIGURE_DEPENDS needed: this runs at build time, so the glob and the
# mtimes are always current.
file(GLOB_RECURSE mpx_inputs
    "${MPX_PWA_DIR}/src/*"
    "${MPX_PWA_DIR}/public/*")
list(APPEND mpx_inputs
    "${MPX_PWA_DIR}/package.json"
    "${mpx_package_lock}"
    "${MPX_PWA_DIR}/vite.config.js"
    "${MPX_PWA_DIR}/svelte.config.js"
    "${MPX_PWA_DIR}/postcss.config.js"
    "${MPX_PWA_DIR}/tailwind.config.js"
    "${MPX_PWA_DIR}/gzip-www.mjs")

set(mpx_newest 0)
foreach(mpx_file IN LISTS mpx_inputs)
    if(NOT EXISTS "${mpx_file}")
        continue()
    endif()
    file(TIMESTAMP "${mpx_file}" mpx_file_ts "%s" UTC)
    if(mpx_file_ts AND mpx_file_ts GREATER mpx_newest)
        set(mpx_newest "${mpx_file_ts}")
    endif()
endforeach()

# ── is www/ stale? ──────────────────────────────────────────────────────────
set(mpx_recorded "")
if(EXISTS "${MPX_PWA_STAMP}")
    file(READ "${MPX_PWA_STAMP}" mpx_recorded)
    string(STRIP "${mpx_recorded}" mpx_recorded)
endif()

set(mpx_reason "")
if(mpx_force)
    set(mpx_reason "MPX_FORCE_PWA is set")
elseif(NOT EXISTS "${mpx_www_index}")
    set(mpx_reason "${mpx_www_index} is missing")
elseif(mpx_recorded STREQUAL "")
    set(mpx_reason "www/ has never been recorded in ${MPX_PWA_STAMP}")
elseif(mpx_newest GREATER mpx_recorded)
    set(mpx_reason "the phone app sources are newer than www/")
endif()

if(mpx_reason STREQUAL "")
    if(NOT MPX_PWA_QUIET)
        message(STATUS "pwa-claw: www/ is up to date, npm not run "
                       "(MPX_FORCE_PWA=1 rebuilds it, MPX_SKIP_PWA=1 silences this)")
    endif()
    return()
endif()

message(STATUS "pwa-claw: rebuilding the phone app: ${mpx_reason}")

# ── node_modules: `npm ci` when it is absent or the lockfile moved on ───────
set(mpx_needs_ci FALSE)
if(NOT EXISTS "${mpx_node_modules}")
    set(mpx_needs_ci TRUE)
elseif(EXISTS "${mpx_package_lock}" AND EXISTS "${mpx_installed_lock}")
    file(TIMESTAMP "${mpx_package_lock}" mpx_lock_ts "%s" UTC)
    file(TIMESTAMP "${mpx_installed_lock}" mpx_installed_ts "%s" UTC)
    if(mpx_lock_ts AND mpx_installed_ts AND mpx_lock_ts GREATER mpx_installed_ts)
        set(mpx_needs_ci TRUE)
    endif()
endif()

if(mpx_needs_ci)
    message(STATUS "pwa-claw: node_modules/ is out of date, running 'npm ci'")
    execute_process(
        COMMAND "${MPX_NPM_EXECUTABLE}" ci
        WORKING_DIRECTORY "${MPX_PWA_DIR}"
        RESULT_VARIABLE mpx_rc)
    if(NOT mpx_rc EQUAL 0)
        message(FATAL_ERROR "pwa-claw: 'npm ci' failed (${mpx_rc})")
    endif()
endif()

# ── build ──────────────────────────────────────────────────────────────────
execute_process(
    COMMAND "${MPX_NPM_EXECUTABLE}" run build
    WORKING_DIRECTORY "${MPX_PWA_DIR}"
    RESULT_VARIABLE mpx_rc)
if(NOT mpx_rc EQUAL 0)
    message(FATAL_ERROR "pwa-claw: 'npm run build' failed (${mpx_rc})")
endif()
if(NOT EXISTS "${mpx_www_index}")
    message(FATAL_ERROR "pwa-claw: 'npm run build' did not produce ${mpx_www_index}")
endif()

# Record the newest input mtime this run covered. A source edited while npm was
# running is newer than that, so the next build picks it up.
file(WRITE "${MPX_PWA_STAMP}" "${mpx_newest}\n")
message(STATUS "pwa-claw: www/ rebuilt (stamp ${MPX_PWA_STAMP} = ${mpx_newest})")
