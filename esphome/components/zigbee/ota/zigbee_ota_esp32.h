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
  void set_block_size(uint8_t block_size) { this->block_size_ = block_size; }
  void set_file_version(uint32_t file_version) { this->file_version_ = file_version; }
  void set_manufacturer_code(uint16_t manufacturer_code) { this->manufacturer_code_ = manufacturer_code; }
  void set_image_type(uint16_t image_type) { this->image_type_ = image_type; }
#ifdef USE_ZIGBEE_OTA_CONFIRM
  void set_safe_mode(safe_mode::SafeModeComponent *safe_mode) { this->safe_mode_ = safe_mode; }
  void set_confirm_timeout(uint32_t confirm_timeout_ms) { this->confirm_timeout_ms_ = confirm_timeout_ms; }
#endif

  void on_ota_action(ezb_zcl_core_action_callback_id_t callback_id, void *message) override;
#ifdef USE_ZIGBEE_OTA_CONFIRM
  /// A frame from the coordinator arrived (Zigbee task).
  void on_coordinator_frame() { this->server_answered_ = true; }
#endif

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
#ifdef USE_ZIGBEE_OTA_CONFIRM
  void confirm_loop_(uint32_t now);
  void send_query_();
#endif

  uint8_t endpoint_{1};
  uint8_t block_size_{50};
  uint32_t file_version_{0};
  uint16_t manufacturer_code_{0};
  uint16_t image_type_{0};
#ifdef USE_ZIGBEE_OTA_CONFIRM
  // A new image stays unconfirmed (bootloader rollback armed) until the
  // coordinator is heard (any frame from it, or an answer to Query Next
  // Image): an image that boots but cannot reach the Zigbee network
  // could not be updated again, so it rolls back after confirm_timeout_ms_.
  safe_mode::SafeModeComponent *safe_mode_{nullptr};
  uint32_t confirm_timeout_ms_{0};
  uint32_t boot_ms_{0};
  uint32_t last_query_ms_{0};
  bool pending_confirm_{false};
  std::atomic<bool> server_answered_{false};
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
  std::atomic<uint32_t> last_progress_ms_{0};
  uint32_t reboot_at_ms_{0};  // main loop only
};

}  // namespace esphome::zigbee

#endif  // USE_ESP32 && USE_ZIGBEE_OTA
