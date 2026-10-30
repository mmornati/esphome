// Zigbee2MQTT external converter for the ESPHome Zigbee OTA test device
// (zigbee-ota-test.yaml). `ota: true` is what makes Z2M show the firmware
// update entity and accept OTA requests for this device.
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

export default {
    zigbeeModel: ['ESPHomeOTATest'],
    model: 'ESPHomeOTATest',
    vendor: 'ESPHome',
    description: 'ESPHome Zigbee OTA test (XIAO ESP32-C6)',
    extend: [
        m.deviceEndpoints({endpoints: {uptime: 1, temperature: 2}}),
        m.numeric({
            name: 'uptime',
            cluster: 'genAnalogInput',
            attribute: 'presentValue',
            reporting: {min: 10, max: 300, change: 1},
            unit: 's',
            description: 'Uptime of the device',
            access: 'STATE_GET',
            endpointNames: ['uptime'],
        }),
        m.numeric({
            name: 'chip_temperature',
            cluster: 'genAnalogInput',
            attribute: 'presentValue',
            reporting: {min: 10, max: 300, change: 0.5},
            unit: '°C',
            precision: 1,
            description: 'ESP32-C6 internal temperature',
            access: 'STATE_GET',
            endpointNames: ['temperature'],
        }),
    ],
    ota: true,
};
