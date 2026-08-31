# Pinned roots

**Not currently used by any backend.** Gemini pinned `gts_root_r1.pem` for a
while on the theory below; that turned out to be wrong and was reverted. The
mechanism is kept because pinning is occasionally the right answer, but pinning
a *single* root is fragile: it breaks the moment the server presents a path to a
different one, which is what `mbedtls_ssl_handshake -0x2700`
(`X509_CERT_VERIFY_FAILED`) turned out to be here. The ESP-IDF bundle carries
every root and needs no maintenance when a provider rotates.

The original reasoning, kept because it is a plausible failure mode and may
apply to some other host:


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
