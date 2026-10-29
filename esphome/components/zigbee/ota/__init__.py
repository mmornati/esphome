"""Firmware updates over Zigbee: the ZCL OTA Upgrade client (0x0019).

The coordinator (Zigbee2MQTT, ZHA) acts as the OTA server and serves the
``firmware.zigbee.ota`` file that the build writes next to ``firmware.ota.bin``.
The received image goes through the regular ESP-IDF OTA backend, so rollback
and safe mode work as for the other OTA platforms.
"""

import importlib.util
import logging
from pathlib import Path
import sys
import time

import esphome.codegen as cg
from esphome.components.esp32 import (
    add_extra_build_file,
    add_idf_component,
    add_src_cmake,
)
from esphome.components.ota import BASE_OTA_SCHEMA, OTAComponent, ota_to_code
import esphome.config_validation as cv
from esphome.const import CONF_ADVANCED, CONF_ENDPOINT, CONF_FRAMEWORK, CONF_ID
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
from esphome.types import ConfigType

from ..const import ZigbeeComponent
from ..const_zephyr import CONF_ZIGBEE_ID
from ..zigbee_ep_esp32 import get_first_ep_num

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@mmornati"]
DEPENDENCIES = ["zigbee"]

CONF_MANUFACTURER_CODE = "manufacturer_code"
CONF_IMAGE_TYPE = "image_type"
CONF_FILE_VERSION = "file_version"
CONF_BLOCK_SIZE = "block_size"
CONF_COMPRESS = "compress"
CONF_CONFIRM_TIMEOUT = "confirm_timeout"
CONF_DELTA_BASE = "delta_base"

OTA_FILE_NAME = "firmware.zigbee.ota"
DELTA_FILE_NAME = "firmware.zigbee.delta.ota"
PACKER_SCRIPT = "make_zigbee_ota.py"

zigbee_ns = cg.esphome_ns.namespace("zigbee")
ZigbeeOTAComponent = zigbee_ns.class_("ZigbeeOTAComponent", OTAComponent)


def _require_detools(value):
    if importlib.util.find_spec("detools") is None:
        raise cv.Invalid(
            f"'{CONF_DELTA_BASE}' needs detools in ESPHome's Python: pip install detools"
        )
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ZigbeeOTAComponent),
            cv.GenerateID(CONF_ZIGBEE_ID): cv.use_id(ZigbeeComponent),
            # Espressif's manufacturer code, used by the esp-zigbee-sdk examples.
            cv.Optional(CONF_MANUFACTURER_CODE, default=0x131B): cv.hex_uint16_t,
            # The server offers a file to every device with the same manufacturer
            # code and image type: give each kind of device its own.
            cv.Required(CONF_IMAGE_TYPE): cv.hex_uint16_t,
            # Defaults to the build time, so every build is newer than the last.
            cv.Optional(CONF_FILE_VERSION): cv.hex_uint32_t,
            # Defaults to the first endpoint.
            cv.Optional(CONF_ENDPOINT): cv.int_range(min=1, max=240),
            # Largest block the client asks for; the server may send less. A
            # block response carries ~17 bytes of headers on top of the data and
            # must fit one Zigbee frame (less room on a routed or secured path):
            # 64 data bytes never arrived on a router behind Zigbee2MQTT, 50 did.
            cv.Optional(CONF_BLOCK_SIZE, default=50): cv.int_range(min=16, max=254),
            # zlib-compress the image in the OTA file (about 60 % of the size).
            cv.Optional(CONF_COMPRESS, default=True): cv.boolean,
            # A new image is confirmed only once the coordinator is heard (any
            # frame from it, or an answer to Query Next Image); without that in
            # this time the bootloader rolls back to the previous image. 0s
            # leaves it to safe_mode alone.
            cv.Optional(
                CONF_CONFIRM_TIMEOUT, default="10min"
            ): cv.positive_time_period_milliseconds,
            # Also write firmware.zigbee.delta.ota, a delta against the image the
            # device runs: its file version (every build keeps its image under
            # .esphome/zigbee_ota/<name>/) or the path of its .bin.
            cv.Optional(CONF_DELTA_BASE): cv.All(cv.string_strict, _require_detools),
        }
    )
    .extend(BASE_OTA_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


@coroutine_with_priority(CoroPriority.OTA_UPDATES)
async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_ZIGBEE_OTA")
    var = cg.new_Pvariable(config[CONF_ID])
    await ota_to_code(var, config)
    await cg.register_component(var, config)
    zb = await cg.get_variable(config[CONF_ZIGBEE_ID])
    cg.add(var.set_parent(zb))

    file_version = config.get(CONF_FILE_VERSION, int(time.time()) & 0xFFFFFFFF)
    endpoint = config.get(CONF_ENDPOINT) or get_first_ep_num()
    cg.add(var.set_endpoint(endpoint))
    cg.add(var.set_block_size(config[CONF_BLOCK_SIZE]))
    cg.add(var.set_file_version(file_version))
    cg.add(var.set_manufacturer_code(config[CONF_MANUFACTURER_CODE]))
    cg.add(var.set_image_type(config[CONF_IMAGE_TYPE]))
    safe_mode_conf = CORE.config.get("safe_mode") or {}
    if config[CONF_CONFIRM_TIMEOUT] and CONF_ID in safe_mode_conf:
        cg.add_define("USE_ZIGBEE_OTA_CONFIRM")
        cg.add(var.set_safe_mode(await cg.get_variable(safe_mode_conf[CONF_ID])))
        cg.add(var.set_confirm_timeout(config[CONF_CONFIRM_TIMEOUT]))
    CORE.add_job(
        _add_cluster,
        zb,
        endpoint,
        config[CONF_MANUFACTURER_CODE],
        config[CONF_IMAGE_TYPE],
        file_version,
    )

    # Applies delta images (tag 0xF101) on the device.
    add_idf_component(name="espressif/esp_delta_ota", ref="1.1.4")

    # Wrap the app image into a Zigbee OTA file after every build.
    add_extra_build_file(PACKER_SCRIPT, Path(__file__).parent / PACKER_SCRIPT)
    args = [
        f"--manufacturer-code {config[CONF_MANUFACTURER_CODE]}",
        f"--image-type {config[CONF_IMAGE_TYPE]}",
        f"--file-version {file_version}",
        f'--header-string "{CORE.name[:32]}"',
    ]
    if not config[CONF_COMPRESS]:
        args.append("--no-compress")
    archive = Path(CORE.data_dir) / "zigbee_ota" / CORE.name
    pack_args = [*args, f'--archive "{archive.as_posix()}"']
    # IDF's Python runs the packer; a delta needs detools, so ESPHome's own.
    python = '"${_zb_ota_python}"'
    if CONF_DELTA_BASE in config:
        pack_args += [
            f'--delta-base "{config[CONF_DELTA_BASE]}"',
            f'--delta-output "${{CMAKE_BINARY_DIR}}/{DELTA_FILE_NAME}"',
        ]
        python = f'"{Path(sys.executable).as_posix()}"'
    # With signed OTA verification the device only accepts a signed image.
    # A key in the config signs <name>.bin before gen_project_binary ends, so
    # the OTA file wraps the signed image. Signed outside the build, the image
    # has to be wrapped again after signing.
    advanced = (
        CORE.config.get("esp32", {}).get(CONF_FRAMEWORK, {}).get(CONF_ADVANCED, {})
    )
    signed = advanced.get("signed_ota_verification")
    if signed and "signing_key" not in signed:
        _LOGGER.warning(
            "Signed OTA verification without 'signing_key': %s holds the unsigned "
            "image, which the device rejects. Sign <build>/%s.bin, then wrap it with "
            "'python %s <signed .bin> %s %s'",
            OTA_FILE_NAME,
            CORE.name,
            PACKER_SCRIPT,
            OTA_FILE_NAME,
            " ".join(args),
        )
    add_src_cmake(
        f"""
idf_build_get_property(_zb_ota_python PYTHON)
# The app image comes from IDF's gen_project_binary target in the project
# directory, whose output is not visible as a file dependency from here.
add_custom_target(zigbee_ota_file ALL
    COMMAND {python} "${{CMAKE_SOURCE_DIR}}/{PACKER_SCRIPT}"
            "${{CMAKE_BINARY_DIR}}/${{CMAKE_PROJECT_NAME}}.bin"
            "${{CMAKE_BINARY_DIR}}/{OTA_FILE_NAME}"
            {" ".join(pack_args)}
    BYPRODUCTS "${{CMAKE_BINARY_DIR}}/{OTA_FILE_NAME}"
    COMMENT "Generating Zigbee OTA file {OTA_FILE_NAME}"
    VERBATIM)
add_dependencies(zigbee_ota_file gen_project_binary)
"""
    )


# The cluster goes onto an endpoint created by the zigbee component's code,
# so it is added after that and before the Zigbee stack starts in setup().
@coroutine_with_priority(CoroPriority.LATE)
async def _add_cluster(zb, endpoint, manufacturer_code, image_type, file_version):
    cg.add(
        zb.add_ota_client_cluster(endpoint, manufacturer_code, image_type, file_version)
    )
