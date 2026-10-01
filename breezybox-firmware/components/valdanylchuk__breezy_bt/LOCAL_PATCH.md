# Local override of valdanylchuk/breezy_bt

Vendored copy of upstream **1.0.2** with two local changes:

- `bt_keyboard_has_saved_target()` — exposes the existing `s_have_target`
  static so `bt connect` can report "No saved keyboard" instead of failing
  silently. Upstream has no equivalent.
- `bt_keyboard_init()` checks the results of `nimble_port_init()`,
  `esp_hidh_init()` and the connect task/timer creation, and returns the error
  after undoing what it had set up. Upstream ignored them, so when the heap was
  too small for NimBLE's buffer pools (`hci inits failed`) it went on to call
  `ble_gap_event_listener_register()` on an uninitialised host and panicked
  with LoadProhibited.
- The `hidh_connect` task stack is 5120 bytes instead of 8192. It is created
  before `nimble_port_init()` because it is the largest single block BT
  allocates. Init logs the largest free block before and after NimBLE, and the
  task logs its stack high-water mark after each connect attempt.

It lives in `components/` rather than `managed_components/` on purpose. The
component manager rewrites `managed_components/` whenever it re-resolves
dependencies, and that directory is gitignored, so a patch applied there is
both invisible to git and silently lost on the next re-resolve. Project
components take precedence over managed ones, which is the same mechanism
`valdanylchuk__breezy_rgb_lcd` and `valdanylchuk__breezy_term` already use here.

If you bump the upstream version, re-apply both changes.
