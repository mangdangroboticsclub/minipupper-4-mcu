# Seal public keys

`prov_seal_pub.pem` here is the **development** key. It is an RSA-3072 public key
that the firmware embeds and encrypts a sealed identity blob to
(docs/ROBOT_IDENTITY_AND_KEYS.md §6.4). The matching private key lives at
`tools/secrets/prov_seal_priv.pem` and is **not** committed: `.gitignore` ignores
`*.pem` and this directory is re-included narrowly, so only public halves can
ever be added here.

A development key is safe to commit for the same reason a test vector is: it
proves the plumbing without protecting anything real. Nothing seals to it but a
`CONFIG_APP_IDENTITY_SOURCE_EFUSE` dev image on a bench board.

## Replacing it for production

The production key is not a file in this repository. It is a KMS key
(`robot-seal-mpx-dog`, HSM protection level, in the `mpx-identity-prod` keyring)
and the cloud never exports its private half. Provisioning production images
therefore does not use a `.pem` in this tree at all:

1. `gcloud kms keys versions get-public-key` for the seal key, DER-encoded.
2. Convert to PEM and drop it here as slot 0 (slot 1 is reserved for a rotation,
   so both keys can be embedded during an overlap).
3. Rebuild with `CONFIG_APP_PROV_SEAL_KEY_ID=0` (or `1` for the new slot).

The station (`tools/mpx_provision.py`) never holds the production private key
either. In production the sealed blob goes to the cloud, which calls KMS to
decrypt it and immediately wraps the result under the robot's KEK; the plaintext
key is never written to a station disk. `--out` exists for the bench, where the
development key is local and losing a dev key costs nothing.

## Regenerating the development key

```
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 \
    -out tools/secrets/prov_seal_priv.pem
chmod 600 tools/secrets/prov_seal_priv.pem
openssl rsa -in tools/secrets/prov_seal_priv.pem -pubout \
    -out components/mpx_identity/certs/prov_seal_pub.pem
```

Regenerating invalidates every blob sealed with the old key, which only matters
for a unit that was provisioned and then never registered. Re-run `reseal` on
such a unit (it re-encrypts the same `K_v`, so the key does not change) or
provision it again from scratch.
