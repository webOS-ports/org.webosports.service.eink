org.webosports.service.eink
===========================

Summary
-------
Refresh-mode control for E Ink panels, on the luna-service2 bus.

Description
-----------
Known hardware: the Minimal Phone MP01 (Pango CPLD, stock
`panel-z10-eink-i2c.ko`). Its command register is
`/sys/bus/i2c/drivers/eink_cpld/*/eink_cpld_registers`; the values written and
the full-refresh sequence follow stock Android's `MinimalRefreshService`
(`services.jar`). See the comment at the top of `src/main.c`.

API
---
All methods are in the `eink.operation` ACG group.

### `getStatus`

`{"subscribe": true}` optional.

```json
{
  "returnValue": true,
  "available": true,
  "mode": "slow",
  "modes": [
    {"id": "slow",  "label": "Slow",  "description": "Best quality; for reading and text."},
    {"id": "ultra", "label": "Ultra", "description": "Fast refresh; more ghosting, better for scrolling and video."}
  ]
}
```

`available` is false on a device without a supported panel; `modes` is still
listed so a client can lay itself out before the hardware answers.

### `setMode`

`{"mode": "<id>"}` — applies the mode and stores it as the systemservice
preference `einkRefreshMode`, from which it is restored at boot. Replies with
the status.

### `refresh`

`{}` — a full refresh (clear) that removes ghosting; the current mode is put
back afterwards. Replies with the status.

### `setActive`

`{"active": bool}` — the shell's word on whether the screen is moving. In the
`auto` mode the panel is on its fast waveform while active and on the clean
one otherwise; the other modes ignore it. luna-next-cardshell derives it from
the frames the compositor renders (`Connectors/EinkRefresh.qml`).

### `watchKey`

`{"subscribe": true}` — posts `{"event": "shortPress"|"longPress"}` for the
refresh key.

Modes
-----
In order of speed: `slow` (register 1, full greyscale), `balanced` (2,
greyscale, clearer with less ghosting - but entering it is a double clearing
flash), `auto` (1, and 4 while active - stock's "Hybrid"; 1<->4 is silent),
`text` (3, nearly two-level), `ultra` (4, fastest). The status carries
`active` alongside `mode`.

Refresh key
-----------
The service also watches evdev for key code 252 (Android's `AREFRESH`, the
button between volume up and down on the MP01): a short press is a full
refresh; a press held for 400 ms is posted to `watchKey` subscribers (the
shell opens its refresh menu), or, with none, opens
`org.webosports.app.settings.display`.

Copyright and License Information
---------------------------------
Copyright (c) 2026 Herman van Hazendonk

Licensed under the Apache License, Version 2.0.
