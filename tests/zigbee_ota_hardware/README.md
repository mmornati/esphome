# Zigbee OTA hardware test

A manual test of `ota: - platform: zigbee` on a real board with Zigbee2MQTT (Z2M) or ZHA.
It is not run by CI.

| File | What it is |
|---|---|
| `zigbee-ota-test.yaml` | Test device: XIAO ESP32-C6 as a router, with uptime and chip temperature |
| `esphome_ota_test.mjs` | Z2M external converter for the test device, with `ota: true` |

The config pulls the `zigbee` component from `github://mmornati/esphome@zigbee-ota` through
`external_components`, so a recent ESPHome dev is enough. Remove that block when you build from
this branch directly.

## Steps with Zigbee2MQTT

1. **Install the converter.** Copy `esphome_ota_test.mjs` into `<z2m data>/external_converters/`
   and restart Z2M, or register it without a restart by publishing
   `{"name": "esphome_ota_test.mjs", "code": "<file contents>"}` to
   `zigbee2mqtt/bridge/request/converter/save`.
2. **First flash, by USB.** This is the only USB flash. It also installs the bootloader with
   rollback enabled.
   ```bash
   esphome run zigbee-ota-test.yaml
   ```
   Permit join in Z2M. The device shows up as `ESPHomeOTATest`, with uptime and chip temperature.
3. **Build a second version.** Change something you can see, for example the uptime
   `update_interval` from 60s to 30s, then compile:
   ```bash
   esphome compile zigbee-ota-test.yaml
   ```
   The log ends with `Zigbee OTA file firmware.zigbee.ota: version 0x..., ... bytes`.
4. **Send the file to the device.** Publish to `zigbee2mqtt/bridge/request/device/ota_update/update`,
   either with a path Z2M can read:
   ```json
   {"id": "<friendly name>", "url": "<path as Z2M sees it>/firmware.zigbee.ota"}
   ```
   or with the file inline as hex:
   ```json
   {"id": "<friendly name>", "hex": {"data": "<hex of the file>", "file_name": "firmware.zigbee.ota"}}
   ```
   The file is `.esphome/build/zigbee-ota-test/build/firmware.zigbee.ota`. Z2M answers on
   `zigbee2mqtt/bridge/response/device/ota_update/update` when the update ends.
5. **Watch the serial log** (`esphome logs zigbee-ota-test.yaml`). The `zigbee.ota` tag logs
   `Image offered`, `Starting update`, `Progress: 10%` ... `Download complete`,
   `Image verified and selected for the next boot` and `Rebooting into the new firmware`.
   A `Block at ... skipped` line followed by `FileOffset ... set to ...` is the client recovering
   from a late duplicate block. After the reboot, Z2M shows the new file version and the device
   behaves as the second version.

With the Z2M default of 50-byte blocks, a 360 KB file takes about 30 minutes.

Z2M can also take the file from an index instead of a request with a path: set
`ota.zigbee_ota_override_index_location` to the index described in ZHA step 1 below. The update
check then offers the new version like for any other device.

## Steps with ZHA

ZHA needs no quirk: it creates a firmware update entity for every device with the OTA Upgrade
client cluster. It offers the image files in a folder.

1. **Add a folder provider** to `configuration.yaml` once, then restart Home Assistant:
   ```yaml
   zha:
     zigpy_config:
       ota:
         extra_providers:
           - type: advanced
             path: /config/zigpy_ota
             warning: "I understand I can *destroy* my devices by enabling OTA updates from files. Some OTA updates can be mistakenly applied to the wrong device, breaking it. I am consciously using this at my own risk."
   ```
   Instead of a folder, ZHA can also read an index in the Z2M format from a URL, the same file
   that Z2M takes as `ota.zigbee_ota_override_index_location`:
   ```yaml
           - type: z2m
             url: http://<host>/zigbee_ota/index.json
   ```
   Each entry needs `fileVersion`, `fileSize`, `manufacturerCode`, `imageType`, `sha512` and
   `url`, the URL of the `.ota` file.
2. **First flash by USB** as in step 2 above. Pair the device with *Add device* in ZHA.
3. **Build a second version** as in step 3 above.
4. **Copy the file** `firmware.zigbee.ota` into `/config/zigpy_ota`, under any name that ends in
   `.ota`, or add it to the index. ZHA reads the folder and the index only once a day, so reload
   the ZHA integration afterwards. If the update entity still shows no new version, call
   `homeassistant.update_entity` on it, which makes ZHA check at once.
5. **Press Install** on the update entity, or call `update.install`. The serial log shows the
   same lines as in step 5 above. After the reboot the device sends Query Next Image, which ZHA
   answers at once, so `Coordinator reached, new image confirmed` follows within a second. ZHA
   then reads the new file version and interviews the device again by itself.

zigpy sends at most 50 bytes per block, whatever `block_size` asks for, but answers each block at
once: a 356 KB file took about 10 minutes on an ESP32-H2 router with a strong link, 11 to 13
minutes when two devices updated at the same time. If the device is not on the network when the
install starts, ZHA gives up after about 15 seconds with `Update was not successful: Status.FAILURE`.

## Optional rollback check

Build a third version that crashes at boot, for example an `on_boot` lambda that dereferences
`nullptr`, and send it over Zigbee. The bootloader should fall back to the second version.

The server does not see this rollback: the device sent a successful Upgrade End before it
rebooted. ZHA reports the update as successful, then reads the old file version from the device
again and offers the crashing image once more, so remove it from the folder.

## Results with ZHA

ESP32-H2 router without BLE and XIAO ESP32-C6 router, Home Assistant 2026.10 with a ConBee III
coordinator. The H2 ran `zigbee-ota-test.yaml` with `variant: esp32h2` and
`board: esp32-h2-devkitm-1`:

| Check | Result |
|---|---|
| Update, zlib image of 356 KB, H2 alone | 10 minutes, no skipped blocks, back on the network 1 s after the reboot |
| Update of both at the same time, 356 KB (H2) and 365 KB (C6) | 13 and 11 minutes, no skipped blocks |
| Confirmation after the reboot | Query Next Image answered at once, image confirmed within a second |
| Free heap while inflating | lowest 139 KB of 184 KB on the H2, 335 KB of 380 KB on the C6 |
| Index in the Z2M format from a URL | update offered and installed |
| Rollback check | crashing image installed over ZHA, the second version ran again from the next boot on |
