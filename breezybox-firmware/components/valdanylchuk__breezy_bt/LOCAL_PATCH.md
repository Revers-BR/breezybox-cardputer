# Local override of valdanylchuk/breezy_bt

Vendored copy of upstream **1.0.2** with one local addition:

- `bt_keyboard_has_saved_target()` — exposes the existing `s_have_target`
  static so `btconnect` can report "No saved keyboard" instead of failing
  silently. Upstream has no equivalent.

It lives in `components/` rather than `managed_components/` on purpose. The
component manager rewrites `managed_components/` whenever it re-resolves
dependencies, and that directory is gitignored, so a patch applied there is
both invisible to git and silently lost on the next re-resolve. Project
components take precedence over managed ones, which is the same mechanism
`valdanylchuk__breezy_rgb_lcd` and `valdanylchuk__breezy_term` already use here.

If you bump the upstream version, re-apply this accessor.
