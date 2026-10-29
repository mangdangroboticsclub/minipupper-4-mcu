# First-connect enrolment (design B)

The robot introduces itself to the cloud. The bench station burns fuses and
prints a label; it does not hand key material to the cloud.

```
factory                         the dog                       cloud
─────────────────────────────── ───────────────────────────── ────────────────────────
tools/mpx_provision.py           @MPXPROV {"cmd":"provision"}
  → generate ID + S (32 B)         S → eFuse BLOCK_KEYn (HMAC_UP, RD_DIS)
  → burn 20-byte record            ID/format/flags/kv_base → BLOCK_USR_DATA (WR_DIS)
  → label + key_fp                 K_v = HMAC_UP(S, mpx/v1/robot-key ‖ id ‖ kv)
                                   seal K_v → prov_seal_pub.pem
                                                               ← POST /v1/provisioning/enroll
                                   {robot_uuid, kv, seal_key_id, blob}
                                                                 unwrap with KMS
                                                                 INSERT robots
                                   ← 201 {status,key_fp}
  label the unit with the ID       (K_v used as the AES-GCM key from here on)
```

## Why the robot posts it

The alternative (A) was: the station derives `K_v`, writes it into the cloud's
database and mounts the PEM holding the private key. That means the production
gateway mounts a file that decrypts every seal ever made, the station needs a
database credential, and no single component ever proves it saw the robot. Here
the only thing the gateway can do with its secret is *open an envelope*, and the
key it ends up storing was produced by the robot's own fuses.

## Wire

`POST /v1/provisioning/enroll` (`core_gateway/provisioning.py`)

```json
{"robot_uuid": "MPX-7PNMQHYWYQ64", "kv": 1, "seal_key_id": 0, "blob": "<784 hex>"}
```

| status | meaning |
| --- | --- |
| 201 | created; `robots` row inserted, key also cached in the gateway's memory |
| 200 | already enrolled with the same key (retry after a lost response) |
| 409 | already enrolled with a **different** key — replace the unit or rotate |
| 422 | envelope malformed, undecryptable, or naming a different robot/key version |

The envelope is 392 bytes: `"MPXS"`, format `1`, seal-key id, 2 reserved bytes,
then 384 bytes of RSA-3072 OAEP-SHA256 over the 149-byte plaintext
(`"MPXK"`, format, `kv`, robot ID, nonce, key, mac, sec flags, secure version,
app version, `sha256(DS public key)`). The layouts live in
`components/mpx_identity/mpx_seal.h`; `tools/mpx_seal.py` and
`core_gateway/tests/test_keywrap.py` hold the two sides' vectors.

The firmware retries after a failed post and gives up into
`mpx_enroll_activated()` staying false — provisioning succeeded either way, so
the robot still boots and reports its state on the provisioning console
(`APP_ENROLL_ENABLE`, default y, plus `APP_ENROLL_PATH` for the endpoint).

## What is still open

- **First-claim-wins.** With no account and no device attestation (the pilot
  image has no Secure Boot), the gateway cannot prove the poster is the silicon
  that ID was burned into. A false claim costs the attacker a real robot's
  name, not its key: the sealed key never leaves the envelope. Closing this
  properly is a shipment-manifest gate (HMAC over `{robot_uuid, kv}` with a
  station secret the cloud also holds) — the endpoint shape does not change.
- **Rate limiting.** `enroll` is unauthenticated like `POST /v1/robots`. It is
  more expensive to abuse (RSA-3072 decrypt per request), so it wants a
  per-IP limit before it faces the internet.
- **Key rotation.** `kv` is stored per robot but nothing rotates it yet. A
  rotation means a new `RESEAL` on the robot side plus a 409-handled update;
  the envelope already carries `kv`, so this is a policy change, not a format
  change.
- **KMS.** `MPX_ENV=prod` requires `MPX_KMS_SEAL_KEY` and refuses a PEM path
  (`keywrap.py:get_wrapper`). The KMS unwrap/wrap branch is deliberately not
  implemented until the prod keyring exists (plan D12).
- **Revocation.** Deleting the `robots` row is the only tool. Nothing expires a
  key, and a stolen unit keeps working until someone notices.

## Operating it

```bash
# firmware: pilot profile, provisioning console on UART0
idf.py -B build/pilot -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.pilot" build

# station
python3 tools/mpx_provision.py scan
python3 tools/mpx_provision.py provision --kv 1
python3 tools/mpx_provision.py status

# cloud (dev): point the gateway at the private half of the seal key
cp ../md-mpx-dog/tools/secrets/prov_seal_priv.pem secrets/prov_seal_priv.pem
docker compose up -d core_gateway
```

Boot log on the robot side: `enroll: posting 392-byte envelope to …` then
`enroll: enrolled, key_fp=…`. That fingerprint must equal the one the station
printed on the label; if it does not, the robot is not the one you provisioned.
