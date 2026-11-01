#pragma once

#include "esphome/core/defines.h"
#if defined(USE_ESP32) && defined(USE_ZIGBEE_OTA)

#include <atomic>
#include <memory>

#include "esphome/components/ota/ota_backend.h"
#include "esphome/components/ota/ota_backend_factory.h"
#include "esphome/components/zigbee/zigbee_esp32.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#ifdef USE_ZIGBEE_OTA_CONFIRM
#include "esphome/components/safe_mode/safe_mode.h"
#endif

namespace esphome::zigbee {

/// ZCL OTA Upgrade client. The Zigbee stack downloads the OTA file from the
/// coordinator block by block; this unpacks the image from the file and writes
/// it through the ESP-IDF OTA backend.
///
/// The stack's callbacks run on the Zigbee task, everything else in the main loop:
/// the state they share goes through atomics.
class ZigbeeOTAComponent : public ota::OTAComponent, public ZigbeeOTAListener, public Parented<ZigbeeComponent> {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  void set_endpoint(uint8_t endpoint) { this->endpoint_ = endpoint; }
  void set_file_version(uint32_t file_version) { this->file_version_ = file_version; }
#ifdef USE_ZIGBEE_OTA_CONFIRM
  void set_safe_mode(safe_mode::SafeModeComponent *safe_mode) { this->safe_mode_ = safe_mode; }
  void set_confirm_timeout(uint32_t confirm_timeout_ms) { this->confirm_timeout_ms_ = confirm_timeout_ms; }
#endif

  void on_ota_action(ezb_zcl_core_action_callback_id_t callback_id, void *message) override;

 protected:
  enum class ImageFormat : uint8_t { NONE, RAW, ZLIB };
  // miniz's decompressor state; its type is only known in the .cpp.
  struct Inflater;
  struct InflaterDeleter {
    void operator()(Inflater *inflater) const;
  };

  ezb_zcl_status_t on_start_(uint32_t file_size, uint32_t file_version);
  ezb_zcl_status_t on_receiving_(uint32_t block_offset, const uint8_t *data, uint32_t len);
  ezb_zcl_status_t on_check_();
  void on_abort_();

  bool begin_element_(uint16_t tag, uint32_t length);
  bool write_element_(const uint8_t *data, uint32_t len, bool last);
  bool inflate_(const uint8_t *data, uint32_t len, bool last);
  bool write_image_(const uint8_t *data, size_t len);
  void release_inflater_();
  void end_download_();
  void reset_upgrade_status_();
  void resync_file_offset_(uint32_t block_len);
  void notify_(ota::OTAState state, float progress, uint8_t error);
  void check_stall_();
#ifdef USE_ZIGBEE_OTA_CONFIRM
  void confirm_image_();
#endif

  uint8_t endpoint_{1};
  uint32_t file_version_{0};
#ifdef USE_ZIGBEE_OTA_CONFIRM
  safe_mode::SafeModeComponent *safe_mode_{nullptr};
  uint32_t confirm_timeout_ms_{0};
#endif

  // Zigbee task only.
  ota::OTABackendPtr backend_{nullptr};
  uint32_t file_size_{0};
  uint32_t header_length_{0};
  uint32_t element_left_{0};
  uint32_t image_written_{0};
  uint8_t pending_[8]{};
  uint8_t pending_len_{0};
  ImageFormat format_{ImageFormat::NONE};
  std::unique_ptr<Inflater, InflaterDeleter> inflater_;
  RAMUniquePtr<uint8_t[]> window_;
  size_t window_pos_{0};
  bool inflate_done_{false};
  bool failed_{false};
  uint8_t last_percent_{0};
  uint8_t resyncs_{0};

  // Shared with the main loop.
  std::atomic<uint32_t> offset_{0};
  std::atomic<bool> downloading_{false};
  std::atomic<bool> ended_{false};  // image checked and selected for the next boot
  std::atomic<bool> reboot_pending_{false};
  std::atomic<uint32_t> reboot_delay_ms_{0};
  std::atomic<uint32_t> blocks_{0};  // callbacks received, for the stall check

  // Main loop only.
  uint32_t last_blocks_{0};
  bool stall_armed_{false};
  bool reboot_scheduled_{false};
};

}  // namespace esphome::zigbee

#endif  // USE_ESP32 && USE_ZIGBEE_OTA
