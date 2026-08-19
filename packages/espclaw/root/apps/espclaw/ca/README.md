# Pinned roots

`breezy.https` normally uses the ESP-IDF certificate bundle. These PEMs exist
for hosts the bundle cannot verify.

## gts_root_r1.pem — Google Trust Services Root R1

Needed for `generativelanguage.googleapis.com` (Gemini). Google's chain is:

```
leaf <- WR2 <- GTS Root R1 <- GlobalSign Root CA   (cross-signed)
```

`esp_crt_bundle` looks up a root by issuer name, takes the first match, and
fails if the signature does not verify -- it never tries the other bundle
entries sharing that name (`esp_crt_bundle.c`, "Certificate matched but
signature verification failed"). The bundle holds several GlobalSign entries,
so the lookup picks the wrong one and the handshake dies with mbedTLS 0x4290.

Pinning GTS Root R1 -- the actual root for this leaf -- sidesteps the
cross-signed cert entirely. Valid until 2036-06-22.

Source: macOS system trust store; also published at https://pki.goog/repository/
