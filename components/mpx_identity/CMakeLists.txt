# The robot's identity: the 16-byte ID and the 32-byte AES key, in one place.
#
# mpx_id.c and mpx_derive.c are ESP-free on purpose -- they are the two files
# tools/host_tests compiles, so the ID format and the derivation byte layout are
# checked against the cloud's Python on every push instead of on a board.
# mpx_identity.cc is the only file here that knows about eFuse, NVS or logging.
idf_component_register(
    SRCS
        "mpx_id.c"
        "mpx_derive.c"
        "mpx_seal.c"
        "mpx_seal_key.c"
        "mpx_enroll.c"
        "mpx_seal_device.c"
        "mpx_efuse_record.c"
        "mpx_efuse.c"
        "mpx_prov_json.c"
        "mpx_prov_sm.c"
        "mpx_provision.c"
        "mpx_identity.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    # The KMS seal public key is compiled in: at provisioning time the robot has
    # exactly one party to talk to, and a key from the image cannot be swapped
    # for one a station chose. certs/prov_seal_pub.pem is the development key.
    EMBED_FILES
        "certs/prov_seal_pub.pem"
    REQUIRES
        nvs_flash
        mbedtls
        # esp_hmac_calculate (HMAC_UP) arrives with the eFuse source, and the
        # custom table with it; the requirements are declared here so that switch
        # is a config change rather than a build-system change.
        efuse
        esp_hw_support
        # esp_app_desc.h (secure_version, app version in the sealed plaintext)
        app_update
        # bootloader_random_enable/-disable: D9, entropy while the radio is down.
        bootloader_support
        # esp_http_client: first-connect enrolment.
        esp_http_client
        # The provisioning listener reads the console UART.
        esp_driver_uart
        driver
        json
)
