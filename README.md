# JudeLiu_DHT — ESP32 + DHT22 Temperature & Humidity Logger

An ESP32 reads a DHT22 temperature/humidity sensor every 3 seconds, shows the values on a small
OLED, and pushes every reading to a Postgres backend in the cloud so the data can be viewed from
anywhere. Readings that fail to upload are buffered in flash and backfilled when the network
returns, so a network outage does not leave a gap in the record.

ROV Team DHT22 mini project — firmware for the sensor node.

**Backend:** [supabase.com/dashboard/project/iohgczqztyckdejsretk](https://supabase.com/dashboard/project/iohgczqztyckdejsretk)
· API base `https://iohgczqztyckdejsretk.supabase.co` · see [Cloud backend](#cloud-backend)

---

## Repository contents

This repository holds the two firmware sketches and nothing else.

| Sketch | Role | What it is |
|---|---|---|
| **`dht22_monitor/`** | **Production** | The real device: Wi-Fi, cloud upload, offline buffering, setup portal, OLED, local fallback dashboard. This is the sketch that runs on the finished unit. |
| **`dht22_serial_logger/`** | **Test / bench** | Phase 1 prototype. Serial-only: no Wi-Fi, no cloud. Used to verify the sensor and the display wiring in isolation before adding the network stack. |

Keeping the test sketch matters: when something breaks, this one tells you whether the fault is in
the **sensor or display** (test sketch also fails) or in the **network/cloud path** (test sketch
works, monitor fails). That single split saves most of the debugging time.

```
dht22_serial_logger/     test   → sensor + OLED only, over serial      (no network)
dht22_monitor/           prod   → the same, plus Wi-Fi + cloud + buffering
```

---

## Wiring

Both sketches use the same three connections. Only GPIO 4, 21 and 22 are used.

| Module | Module pin | ESP32 pin | GPIO | Notes |
|---|---|---|---|---|
| DHT22 | VCC | 3V3 | — | 3.3 V part — do **not** use 5 V |
| DHT22 | DATA | D4 | GPIO 4 | Not a strapping pin, free of special boot roles |
| DHT22 | GND | GND | — | |
| OLED | SDA | D21 | GPIO 21 | ESP32's default I²C data |
| OLED | SCL | D22 | GPIO 22 | ESP32's default I²C clock |
| OLED | VCC | 3V3 | — | |
| OLED | GND | GND | — | |

Both modules can share the same 3V3 and GND pins — together they draw roughly 25 mA, far below the
devkit regulator's rating, and each module carries its own decoupling.

**Notes that matter:**

- The 3-pin DHT22 **module** includes its own 10 kΩ pull-up on the data line. A bare 4-pin sensor
  does not — add a 10 kΩ resistor between VCC and DATA in that case.
- The 0.96" OLED is expected at I²C address `0x3C` (a few modules use `0x3D`).
- Read the silkscreen on your own modules before connecting. Module pin orders differ between
  suppliers, and swapping VCC and GND destroys the module.
- `GPIO 4, 21 and 22` were chosen because they are safe: GPIO 0, 2, 5, 12 and 15 affect boot mode,
  GPIO 34–39 are input-only, and GPIO 6–11 are wired to the internal flash.

---

## `dht22_monitor` — production sketch

### Behaviour

| | |
|---|---|
| **Sampling** | Every 3 s, fixed. The DHT22 datasheet requires ≥ 2 s between reads; 3 s keeps a margin. |
| **OLED** | Temperature, humidity, state (`cloud` / `local` / `setup`) and the running reading count, driven **directly by the ESP32's own readings** — never from the database, so it keeps working with no internet. |
| **Cloud** | Each reading is POSTed to the Supabase `readings` table over HTTPS, authenticated with an `x-device-key` header. |
| **Offline buffer** | Failed uploads queue in LittleFS flash (max 16 KB, oldest dropped first) and drain automatically on reconnect, paced at one per 700 ms to stay under the server's rate limit. |
| **Setup portal** | Wi-Fi credentials are stored in NVS flash. If joining fails for 60 s at boot, the device raises its own access point `ROV-DHT22-Setup`; join it and open `http://192.168.4.1` to enter new credentials — no re-flashing. |
| **Local fallback** | Web page on `http://<device-ip>/` with a live WebSocket feed on port 81, so readings are visible on the LAN even with the cloud unreachable. |
| **mDNS** | Reachable as `http://rov-dht22.local`. |
| **Onboard LED** | GPIO 2, lit while running. |

### Build and flash

1. **Arduino IDE** with the ESP32 board package (Boards Manager → install `esp32` by Espressif).
2. **Libraries** (Library Manager):
   - `DHT sensor library` (Adafruit) **and** `Adafruit Unified Sensor` — the DHT library depends on it
   - `Adafruit SSD1306` — choose **"Install all"** so `Adafruit GFX` and `Adafruit BusIO` come too
   - `WebSockets` by Markus Sattler (links2004)
3. **Credentials:**
   ```bash
   cd dht22_monitor
   cp secrets.h.example secrets.h
   # edit secrets.h: Wi-Fi SSID + password, and the Supabase device key
   ```
   `secrets.h` is git-ignored — the real credentials are never committed.
4. Board **"ESP32 Dev Module"**, then upload at **115200 baud**.

On macOS, install the CP210x USB-serial driver from Silicon Labs if no port appears. If an upload
fails with `Resource busy`, close the Arduino Serial Monitor first — it holds the port open.

### Serial output (115200 baud)

```csv
count,status,temperature,humidity
142,ok,24.7,55.1
143,ok,24.7,55.1
144,ERR,nan,nan        ← sensor read failed; counted, not uploaded
```

Wi-Fi state, buffer depth and cloud errors are printed alongside it.

### First boot

The device joins the network from `secrets.h` and starts logging. If it cannot join within 60 s it
raises the `ROV-DHT22-Setup` access point instead; credentials entered there persist in NVS flash,
so the portal is also how you move the device to a different network later.

---

## `dht22_serial_logger` — test sketch

Serial-only bench tool. No Wi-Fi, no cloud, nothing to configure — it starts reading as soon as it
is powered, which is what makes it useful for isolating a fault.

Type these into the Serial Monitor (**115200 baud**):

| Command | Effect |
|---|---|
| `start` | Log one reading every 3 s, in CSV |
| `stop` | Pause logging and print session statistics (min / avg / max, error count) |
| `help` | List the commands |

The onboard LED (GPIO 2) is lit while logging, and the OLED shows live values with units, the sample
number and the start/stopped state. Writes the same CSV format as the monitor sketch, so a log
captured here and one captured there can be compared directly.

---

## Cloud backend

**Supabase project `rov-dht22`** — region `ap-northeast-2` (Seoul), Postgres 17, free tier.

| | |
|---|---|
| Dashboard | https://supabase.com/dashboard/project/iohgczqztyckdejsretk |
| Project ref | `iohgczqztyckdejsretk` |
| API base | `https://iohgczqztyckdejsretk.supabase.co` |
| Table used by the firmware | `public.readings` |
| Insert endpoint | `POST https://iohgczqztyckdejsretk.supabase.co/rest/v1/readings` |

### Data flow

```
ESP32 (dht22_monitor)
  │
  │  every 3 s:  POST /rest/v1/readings
  │              apikey + Authorization: Bearer <publishable key>
  │              x-device-key: <device key>          ← the real credential
  │              {"device":"esp32-01","temperature":24.7,"humidity":55.1}
  │
  │  offline → LittleFS queue → drained on reconnect
  ▼
Supabase
  ├─ RLS INSERT policy   checks x-device-key + payload range   ──┐
  ├─ BEFORE INSERT trigger  rate limit ≤ 120 rows/min per device ┤ both must pass
  ├─ table public.readings  (append-only)                        ┘
  ├─ realtime publication   supabase_realtime on readings  → pushes to subscribers
  ├─ RPC readings_history() averages raw rows into ~120 buckets for charts
  └─ pg_cron                03:00 UTC daily, deletes rows older than 7 days
```

### Table `public.readings`

| Column | Type | Null | Default | Notes |
|---|---|---|---|---|
| `id` | `bigint` | no | identity | Primary key |
| `device` | `text` | no | `'esp32-01'` | Device identifier; the policy pins this value |
| `temperature` | `double precision` | no | — | °C |
| `humidity` | `double precision` | no | — | %RH |
| `created_at` | `timestamptz` | no | `now()` | Set by the server, not by the ESP32 |

Index: `readings_device_created_idx` on `(device, created_at DESC)`.

Append-only by design: rows are never updated, only inserted and eventually deleted by age. The
ESP32 never sends a timestamp — it has no battery-backed clock, so the server's `now()` is the
authoritative time.

### Access control (Row Level Security)

| Policy | Command | Role | Rule |
|---|---|---|---|
| `device key insert` | INSERT | `anon` | `x-device-key` header must equal the device key **and** `device = 'esp32-01'` **and** temperature in −40…80 **and** humidity in 0…100 |
| `invited users read readings` | SELECT | `authenticated` | `true` — any signed-in user may read |

There is **no** policy granting UPDATE or DELETE to anyone, so those are denied by default. The
`anon` role is what the publishable key in the firmware uses, so the key visible in the source
grants read access to nothing and write access only with the correct device key.

Read access uses Supabase Auth with the Google provider and **new sign-ups disabled** — only
accounts pre-registered in Authentication → Users can obtain a session. A revoked user's already
issued token stays valid for up to ~1 h (stateless JWT lifetime).

### Rate limit

`BEFORE INSERT` trigger `readings_rate_limit` → `enforce_insert_rate()`: if the device already has
≥ 120 rows in the last 60 s, the insert raises an exception. Live logging is 20/min, and the
offline-backfill drain runs at ~85/min, so both fit comfortably under the cap while still bounding
abuse.

### Chart aggregation

`readings_history(window_minutes integer DEFAULT 60, bucket_count integer DEFAULT 120) RETURNS json`

Groups rows into `bucket_count` time buckets with `width_bucket()`, averages temperature and
humidity per bucket, and returns `[{"ts":…,"t":…,"h":…}, …]` ordered by time. This keeps chart
requests small — a 7-day window is still one compact response instead of tens of thousands of rows.

### Retention

pg_cron job `delete-readings-older-than-7d`, schedule `0 3 * * *` (03:00 UTC daily):

```sql
delete from public.readings where created_at < now() - interval '7 days';
```

The free tier has a 500 MB database limit, so retention is deliberate, not incidental.

### Recreating the backend from scratch

```sql
create table public.readings (
  id          bigint generated by default as identity primary key,
  device      text not null default 'esp32-01',
  temperature double precision not null,
  humidity    double precision not null,
  created_at  timestamptz not null default now()
);
create index readings_device_created_idx on public.readings (device, created_at desc);

alter table public.readings enable row level security;

-- device inserts: shared secret + payload sanity
create policy "device key insert" on public.readings
  for insert to anon
  with check (
    (current_setting('request.headers', true)::json ->> 'x-device-key') = '<DEVICE_KEY>'
    and device = 'esp32-01'
    and temperature between -40 and 80
    and humidity between 0 and 100
  );

-- humans read: any authenticated (invited) user
create policy "invited users read readings" on public.readings
  for select to authenticated using (true);

-- rate limit
create or replace function public.enforce_insert_rate() returns trigger
language plpgsql as $$
declare recent int;
begin
  select count(*) into recent from public.readings
   where device = new.device and created_at > now() - interval '60 seconds';
  if recent >= 120 then
    raise exception 'rate limit exceeded for device %', new.device using errcode = 'P0001';
  end if;
  return new;
end $$;

create trigger readings_rate_limit before insert on public.readings
  for each row execute function public.enforce_insert_rate();

-- realtime for live viewers
alter publication supabase_realtime add table public.readings;

-- nightly retention
select cron.schedule('delete-readings-older-than-7d', '0 3 * * *',
  $$delete from public.readings where created_at < now() - interval '7 days'$$);
```

`<DEVICE_KEY>` is the value that also goes in `secrets.h`. Generate one with
`openssl rand -hex 32`.

---

## Security

The firmware holds two secrets, both in `dht22_monitor/secrets.h`, which is **git-ignored and has
never been committed to this repository**:

- the Wi-Fi password, and
- the device key checked by the RLS insert policy.

`secrets.h.example` is the committed template. **The repository can therefore be made public
without rotating anything.** If a device key is ever pasted into an issue, a log or a chat, rotate
it: `openssl rand -hex 32` → update the policy in Supabase → update `secrets.h` → re-flash. About
ten minutes.

Known limitations, documented rather than hidden:

- The sketch calls `setInsecure()` — TLS without certificate pinning. An attacker positioned on the
  network could recover the device key. Certificate pinning is the top hardening item.
- Anyone with physical access to the device can read the key out of flash.
- Replayed traffic can create duplicate rows; there is no per-client throttle below the platform level.
- Backfilled readings are stamped with their **reconnect** time, not their original measurement
  time, because the ESP32 has no real-time clock. A gap in the chart after an outage is compressed,
  not accurate.

---

## Author

Jude Liu — Form 4, ROV Team. Built solo as a school mini project, September 2026.

Uses the Adafruit DHT, Adafruit SSD1306/GFX and arduinoWebSockets libraries under their own
licences. Code written for this project is free to reuse for coursework.
