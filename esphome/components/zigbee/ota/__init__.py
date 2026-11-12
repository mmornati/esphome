"""Firmware updates over Zigbee: the ZCL OTA Upgrade client (0x0019).

The coordinator (Zigbee2MQTT, ZHA) acts as the OTA server and serves the
``firmware.zigbee.ota`` file that the build writes next to ``firmware.ota.bin``.
The received image goes through the regular ESP-IDF OTA backend, so rollback
and safe mode work as for the other OTA platforms.
"""

import logging
from pathlib import Path
import time

import esphome.codegen as cg
from esphome.components.esp32 import add_extra_build_file, add_src_cmake
from esphome.components.ota import BASE_OTA_SCHEMA, OTAComponent, ota_to_code
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADVANCED,
    CONF_ENABLE_OTA_ROLLBACK,
    CONF_ENDPOINT,
    CONF_FRAMEWORK,
    CONF_ID,
    CONF_TYPE,
    CONF_VALUE,
)
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority
import esphome.final_validate as fv
from esphome.types import ConfigType

from ..const import CONF_MANUFACTURER_CODE, KEY_ZIGBEE, ZigbeeComponent
from ..const_esp32 import CONF_ATTRIBUTE_ID, CONF_ATTRIBUTES, CONF_CLUSTERS, ROLE
from ..const_zephyr import CONF_ZIGBEE_ID
from ..zigbee_ep_esp32 import add_clusters_to_first_ep, add_ep, get_first_ep_num
from ..zigbee_esp32 import setup_attributes

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@mmornati"]
DEPENDENCIES = ["zigbee"]

CONF_IMAGE_TYPE = "image_type"
CONF_FILE_VERSION = "file_version"
CONF_BLOCK_SIZE = "block_size"
CONF_COMPRESS = "compress"
CONF_CONFIRM_TIMEOUT = "confirm_timeout"

OTA_FILE_NAME = "firmware.zigbee.ota"
PACKER_SCRIPT = "make_zigbee_ota.py"
KEY_ZIGBEE_OTA_MANUFACTURER_ATTR = "zigbee_ota_manufacturer_attr"

# OTA Upgrade client attributes (ZCL 11.10)
ATTR_CURRENT_FILE_VERSION = 0x0002
ATTR_MANUFACTURER_ID = 0x0007
ATTR_IMAGE_TYPE_ID = 0x0008
# The stack asks again for a block whose answer has not come within this time
# (its default is 5 s, short for a server that answers a sleepy device through
# its parent).
BLOCK_RETRY_MS = 10000
BLOCK_RETRIES = 3

zigbee_ns = cg.esphome_ns.namespace("zigbee")
ZigbeeOTAComponent = zigbee_ns.class_("ZigbeeOTAComponent", OTAComponent)


def _validate_zigbee_ota(config: ConfigType) -> ConfigType:
    # Fixed here so the cluster attribute and the OTA file always agree.
    # Defaults to the build time, so every build is newer than the last.
    if CONF_FILE_VERSION not in config:
        config[CONF_FILE_VERSION] = int(time.time()) & 0xFFFFFFFF
    manufacturer_attr = {
        CONF_ATTRIBUTE_ID: ATTR_MANUFACTURER_ID,
        CONF_TYPE: "UINT16",
        CONF_VALUE: 0,  # set in _final_validate
    }
    cl = {
        CONF_ID: "OTA_UPGRADE",
        ROLE: "CLIENT",
        CONF_ATTRIBUTES: [
            {
                CONF_ATTRIBUTE_ID: ATTR_CURRENT_FILE_VERSION,
                CONF_TYPE: "UINT32",
                CONF_VALUE: config[CONF_FILE_VERSION],
            },
            manufacturer_attr,
            {
                CONF_ATTRIBUTE_ID: ATTR_IMAGE_TYPE_ID,
                CONF_TYPE: "UINT16",
                CONF_VALUE: config[CONF_IMAGE_TYPE],
            },
        ],
    }
    setup_attributes(config, [cl])
    CORE.data.setdefault(KEY_ZIGBEE, {})[KEY_ZIGBEE_OTA_MANUFACTURER_ATTR] = (
        manufacturer_attr
    )
    if (endpoint := config.get(CONF_ENDPOINT)) is None:
        add_clusters_to_first_ep([cl])
    else:
        add_ep({CONF_CLUSTERS: [cl]}, endpoint, None)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ZigbeeOTAComponent),
            cv.GenerateID(CONF_ZIGBEE_ID): cv.use_id(ZigbeeComponent),
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
            # A new image is confirmed only once the device has joined the
            # network again; without that in this time the bootloader rolls back
            # to the previous image. 0s, or a build without OTA rollback, leaves
            # it to safe_mode alone.
            cv.Optional(
                CONF_CONFIRM_TIMEOUT, default="10min"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(BASE_OTA_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_zigbee_ota,
)


def _final_validate(config: ConfigType) -> ConfigType:
    # The manufacturer code is set on the zigbee component; its config is only
    # known here. The attribute dict is shared with the endpoint list, which the
    # zigbee component turns into code.
    zb_conf = fv.full_config.get().get(KEY_ZIGBEE, {})
    CORE.data[KEY_ZIGBEE][KEY_ZIGBEE_OTA_MANUFACTURER_ATTR][CONF_VALUE] = zb_conf.get(
        CONF_MANUFACTURER_CODE, 0
    )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.OTA_UPDATES)
async def to_code(config: ConfigType) -> None:
    cg.add_define("USE_ZIGBEE_OTA")
    var = cg.new_Pvariable(config[CONF_ID])
    # ota_to_code waits until past safe mode, so set up first.
    await cg.register_component(var, config)
    zb = await cg.get_variable(config[CONF_ZIGBEE_ID])
    cg.add(var.set_parent(zb))

    file_version = config[CONF_FILE_VERSION]
    manufacturer_code = CORE.data[KEY_ZIGBEE][KEY_ZIGBEE_OTA_MANUFACTURER_ATTR][
        CONF_VALUE
    ]
    # The zigbee component numbers its endpoints in final validation.
    endpoint = config.get(CONF_ENDPOINT) or get_first_ep_num()
    cg.add(var.set_endpoint(endpoint))
    cg.add(var.set_file_version(file_version))
    cg.add(
        zb.set_ota_download(
            endpoint, config[CONF_BLOCK_SIZE], BLOCK_RETRIES, BLOCK_RETRY_MS
        )
    )
    advanced = (
        CORE.config.get("esp32", {}).get(CONF_FRAMEWORK, {}).get(CONF_ADVANCED, {})
    )
    safe_mode_conf = CORE.config.get("safe_mode") or {}
    # Holding back safe_mode's confirmation needs the bootloader rollback, which
    # the esp32 component also turns off when safe_mode is disabled.
    if (
        config[CONF_CONFIRM_TIMEOUT].total_milliseconds
        and advanced.get(CONF_ENABLE_OTA_ROLLBACK)
        and CONF_ID in safe_mode_conf
    ):
        cg.add_define("USE_ZIGBEE_OTA_CONFIRM")
        cg.add(var.set_safe_mode(await cg.get_variable(safe_mode_conf[CONF_ID])))
        cg.add(var.set_confirm_timeout(config[CONF_CONFIRM_TIMEOUT]))
    await ota_to_code(var, config)

    # Wrap the app image into a Zigbee OTA file after every build.
    add_extra_build_file(PACKER_SCRIPT, Path(__file__).parent / PACKER_SCRIPT)
    args = [
        f"--manufacturer-code {manufacturer_code}",
        f"--image-type {config[CONF_IMAGE_TYPE]}",
        f"--file-version {file_version}",
        f'--header-string "{CORE.name[:32]}"',
    ]
    if not config[CONF_COMPRESS]:
        args.append("--no-compress")
    # With signed OTA verification the device only accepts a signed image.
    # A key in the config signs <name>.bin before gen_project_binary ends, so
    # the OTA file wraps the signed image. Signed outside the build, the image
    # has to be wrapped again after signing.
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
    COMMAND "${{_zb_ota_python}}" "${{CMAKE_SOURCE_DIR}}/{PACKER_SCRIPT}"
            "${{CMAKE_BINARY_DIR}}/${{CMAKE_PROJECT_NAME}}.bin"
            "${{CMAKE_BINARY_DIR}}/{OTA_FILE_NAME}"
            {" ".join(args)}
    BYPRODUCTS "${{CMAKE_BINARY_DIR}}/{OTA_FILE_NAME}"
    COMMENT "Generating Zigbee OTA file {OTA_FILE_NAME}"
    VERBATIM)
add_dependencies(zigbee_ota_file gen_project_binary)
"""
    )
