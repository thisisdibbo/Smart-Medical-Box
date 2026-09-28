# HTTP API

The controller runs a plain `WebServer` on port 80. Every endpoint is a `GET`, including the
ones that change state, so anything that can fetch a URL can drive the box.

Replace `192.168.0.101` with whatever the controller printed on the serial monitor at boot.

---

## `GET /`

The dashboard page. Plain HTML and JavaScript served from flash — no external assets, so it
works with no internet connection.

---

## `GET /status`

Everything the controller knows, as JSON. The dashboard polls this, and so does ESP32 #2.

```bash
curl http://192.168.0.101/status
```

```jsonc
{
  "motors": [ {"status":"Idle"}, {"status":"Running"}, {"status":"Idle"} ],
  "pumps":  [ {"status":"Idle"}, {"status":"Idle"} ],
  "dht":    { "temp": 28.4, "hum": 61.0 },
  "sequence": { "motors": "0,1,2", "pumps": "1,0" },
  "pumpLevels": [1, 0],
  "pills": {
    "target":    [5, 5, 5],
    "remaining": [3, 5, 5]
  },
  "uiMsg": "Pill detected on motor 0. Remaining=3",
  "ip": "192.168.0.101",
  "seqState": "RUN_MOTOR",          // IDLE | RUN_MOTOR | WAIT_AFTER_PILL | RUN_PUMP
  "timeReady": true,                // false until NTP has synced
  "nowHHMM": "08:14",
  "activePart": "MORNING",          // MORNING | DAY | NIGHT | NONE | TIME_NOT_READY
  "runtimeSource": "MORNING",       // DEFAULT | WEB | MORNING | DAY | NIGHT
  "remote": {                       // pushed by ESP32 #2
    "ok": true,
    "ageMs": 820,                   // how stale the reading is
    "bpm": 76,
    "bodyTempC": 36.60,
    "irTempC": 36.60,
    "waterRaw": 2480,
    "waterPct": 70
  }
}
```

`remote.ageMs` is the honest one to watch — `ok` stays `true` after the first successful push,
so a sensor node that has gone offline shows as `ok: true` with a growing age. Treat anything
over a few seconds as stale.

---

## `GET /startSequence`

Starts a dispensing run, exactly as the physical button does.

```bash
curl http://192.168.0.101/startSequence
```

| Response | Meaning |
|---|---|
| `200 {"ok":true,"msg":"Sequence started"}` | Running |
| `409 {"ok":false,"msg":"Sequence already running"}` | One is already in progress |
| `400 {"ok":false,"msg":"All motors have 0 remaining pills"}` | Refill first |

Unlike the button, this does **not** re-read the schedule — it uses the current default
sequence. The button path calls `pickRuntimeForButton()` and loads the active window.

---

## `GET /setSequence`

Sets the default silo and pump order.

```bash
curl "http://192.168.0.101/setSequence?motors=0,2&pumps=1"
```

| Parameter | Values |
|---|---|
| `motors` | CSV of `0`–`2`, 1 to 3 entries, no duplicates |
| `pumps` | CSV of `0`–`1`, 1 to 2 entries, no duplicates |

Order matters — `motors=2,0,1` dispenses from silo 3 first. Saved to NVS.

---

## `GET /setPills`

Sets the target count for each silo. Remaining is set to the same value, so this is also how you
tell the box you have refilled it.

```bash
curl "http://192.168.0.101/setPills?m0=10&m1=10&m2=6"
```

All three parameters are required and must be `>= 0`.

---

## `GET /resetPills`

Puts remaining back up to target without changing the targets.

```bash
curl http://192.168.0.101/resetPills
```

---

## `GET /setPumpLevels`

Dose preset per pump.

```bash
curl "http://192.168.0.101/setPumpLevels?p0=2&p1=1"
```

| Value | Preset | Pump 0 | Pump 1 |
|:--:|---|:--:|:--:|
| `0` | short | 1000 ms | 1500 ms |
| `1` | medium | 2000 ms | 2500 ms |
| `2` | long | 5000 ms | 4500 ms |

---

## `GET /getParts`

Returns all three schedule windows.

```bash
curl http://192.168.0.101/getParts
```

```jsonc
{
  "morning": { "enabled": false, "startMin": 360, "endMin": 600,
               "motors": "0,1,2", "pumps": "1,0", "levels": [1, 0] },
  "day":     { "enabled": false, "startMin": 600, "endMin": 1080,
               "motors": "0,2",   "pumps": "1",   "levels": [0, 1] },
  "night":   { "enabled": false, "startMin": 1080, "endMin": 360,
               "motors": "1",     "pumps": "0",   "levels": [2, 2] }
}
```

Times are minutes past midnight, `0`–`1439`. A window where `startMin > endMin` wraps past
midnight, which is how the night window covers 18:00 → 06:00.

---

## `GET /setParts`

Writes all three windows in one request. Every parameter is required — there is no partial
update, so read `/getParts` first if you only want to change one thing.

| Prefix | Window |
|---|---|
| `m_` | morning |
| `d_` | day |
| `n_` | night |

| Suffix | Meaning |
|---|---|
| `_en` | `1` enabled, `0` disabled |
| `_s` | start, minutes past midnight |
| `_e` | end, minutes past midnight |
| `_m` | silo order, CSV `0`–`2` |
| `_p` | pump order, CSV `0`–`1` |
| `_l0` | pump 0 dose preset, `0`–`2` |
| `_l1` | pump 1 dose preset, `0`–`2` |

Morning 07:00–09:30 with all three silos, day disabled, night 21:00–05:00 with silo 2 only:

```bash
curl "http://192.168.0.101/setParts?\
m_en=1&m_s=420&m_e=570&m_m=0,1,2&m_p=1,0&m_l0=1&m_l1=0&\
d_en=0&d_s=600&d_e=1080&d_m=0,2&d_p=1&d_l0=0&d_l1=1&\
n_en=1&n_s=1260&n_e=300&n_m=1&n_p=0&n_l0=2&n_l1=2"
```

Errors come back as `400` with the reason: `Missing schedule args`, `Invalid start/end`,
`Invalid motor seq`, `Invalid pump seq` or `Invalid pump levels`. Nothing is written unless all
three windows validate.

---

## `GET /ingest`

Used by ESP32 #2 to push its readings. You can call it by hand to test the dashboard without the
sensor node connected.

```bash
curl "http://192.168.0.101/ingest?bpm=72&body=36.6&water=2400&ir=36.6"
```

| Parameter | Required | Meaning |
|---|:--:|---|
| `bpm` | yes | Heart rate |
| `body` | yes | Body temperature, °C |
| `water` | yes | Raw water ADC reading, not a percentage |
| `ir` | no | IR temperature, °C |

Missing any of the three required parameters returns `400`.

---

## Notes

**Configuration is locked while running.** `/setSequence`, `/setPills`, `/resetPills`,
`/setPumpLevels` and `/setParts` all return `409` unless `seqState` is `IDLE`. Poll `/status`
first if you are scripting against the box.

**No authentication.** Anything on the same network can dispense pills. Keep the box off guest
Wi-Fi, and don't port-forward it.

**Writes are immediate.** Every accepted change is committed to NVS before the response is sent,
so a power cut right after a `200` cannot lose the setting.
