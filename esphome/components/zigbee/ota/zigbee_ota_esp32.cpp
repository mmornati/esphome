#include "zigbee_ota_esp32.h"
#if defined(USE_ESP32) && defined(USE_ZIGBEE_OTA)

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "esp_ota_ops.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/zcl/cluster/ota_upgrade.h"
#include "miniz.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::zigbee {

static const char *const TAG = "zigbee.ota";

// OTA file layout (ZCL spec 11.4): a header whose length sits at offset 6, then
// sub-elements of tag (2 bytes) + length (4 bytes) + data. The app image is in
// one sub-element, as is or zlib-compressed (make_zigbee_ota.py); any other
// sub-element is skipped.
static constexpr uint32_t OTA_FILE_IDENTIFIER = 0x0BEEF11E;
static constexpr uint16_t TAG_UPGRADE_IMAGE = 0x0000;
static constexpr uint16_t TAG_IMAGE_ZLIB = 0xF100;
static constexpr uint8_t SUB_ELEMENT_HEADER_LEN = 6;
static constexpr uint32_t MIN_HEADER_LEN = 56;
// The stack asks again for a block whose answer has not come within this time
// (its default is 5 s, short for a server that answers a sleepy device through
// its parent).
static constexpr uint32_t BLOCK_RETRY_MS = 10000;
static constexpr uint8_t BLOCK_RETRIES = 3;
// Without a block for this long the server has walked away. The stack does not
// always report that (a block response with an error status ends it silently).
static constexpr uint32_t STALL_MS = 60000;
static constexpr uint32_t REBOOT_DELAY_MIN_MS = 2000;
static constexpr uint32_t REBOOT_DELAY_MAX_S = 86400;
// Out-of-place blocks in a row tolerated before giving up.
static constexpr uint8_t MAX_RESYNCS = 50;
// While a new image waits for confirmation, ask the server this often.
static constexpr uint32_t CONFIRM_QUERY_MS = 30000;
// After a query, stay awake (no deep sleep) this long for the answer.
static constexpr uint32_t QUERY_ANSWER_MS = 15000;

#ifdef USE_ZIGBEE_OTA_CONFIRM
static ZigbeeOTAComponent *pending_confirm_instance = nullptr;  // NOLINT

// Any frame from the coordinator proves the new image reaches the network:
// Zigbee2MQTT re-interviews a device right after the announce that follows an
// update, but answers Query Next Image only once per update_check_interval (a
// day by default), so its answer alone is not enough. On the Zigbee task;
// never consumes the frame.
static bool on_aps_indication(const ezb_apsde_data_ind_t *ind) {
  if (pending_confirm_instance != nullptr && ind->status == 0 && ind->src_address.addr_mode == EZB_ADDR_MODE_SHORT &&
      ind->src_address.u.short_addr == 0x0000) {
    pending_confirm_instance->on_coordinator_frame();
  }
  return false;
}
#endif

void ZigbeeOTAComponent::setup() {
  this->parent_->set_ota_listener(this);
#ifdef USE_ZIGBEE_OTA_CONFIRM
  esp_ota_img_states_t state;
  if (this->safe_mode_ != nullptr && this->confirm_timeout_ms_ > 0 &&
      esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    this->safe_mode_->hold_app_confirmation();
    this->pending_confirm_ = true;
    this->boot_ms_ = millis();
    ESP_LOGI(TAG, "New image, confirmed once the coordinator is heard (rollback after %" PRIu32 " s)",
             this->confirm_timeout_ms_ / 1000);
  }
#endif
  this->parent_->add_on_start_callback([this]() {
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_ota_upgrade_set_download_block_size(this->endpoint_, this->block_size_);
    ezb_zcl_ota_upgrade_set_download_retry(this->endpoint_, BLOCK_RETRIES, BLOCK_RETRY_MS);
#ifdef USE_ZIGBEE_OTA_CONFIRM
    if (this->pending_confirm_) {
      pending_confirm_instance = this;
      ezb_apsde_data_indication_handler_register(on_aps_indication);
    }
#endif
    esp_zigbee_lock_release();
  });
}

void ZigbeeOTAComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Over-The-Air updates via Zigbee:\n"
                "  Endpoint: %u\n"
                "  File version: 0x%08" PRIX32 "\n"
                "  Block size: %u",
                this->endpoint_, this->file_version_, this->block_size_);
}

void ZigbeeOTAComponent::loop() {
  const uint32_t now = millis();
#ifdef USE_ZIGBEE_OTA_CONFIRM
  if (this->pending_confirm_)
    this->confirm_loop_(now);
#endif

  if (this->query_on_join_ && !this->queried_ && this->parent_->is_joined()) {
    this->queried_ = true;
    this->query_sent_ms_ = now;
#ifdef USE_DEEP_SLEEP
    if (this->deep_sleep_ != nullptr) {
      this->awaiting_answer_ = true;
      this->deep_sleep_->prevent_deep_sleep();
    }
#endif
    this->send_query_();
  }
#ifdef USE_DEEP_SLEEP
  // No answer (or no offer) in time: let the device sleep again.
  if (this->awaiting_answer_.load() && !this->updating_.load() && now - this->query_sent_ms_ > QUERY_ANSWER_MS) {
    this->awaiting_answer_ = false;
    this->deep_sleep_->allow_deep_sleep();
  }
#endif

  if (this->reboot_pending_.load()) {
    if (this->reboot_at_ms_ == 0) {
      const uint32_t delay_ms = std::max(this->reboot_delay_ms_.load(), REBOOT_DELAY_MIN_MS);
      this->reboot_at_ms_ = now + delay_ms;
      ESP_LOGI(TAG, "Rebooting into the new firmware in %" PRIu32 " ms", delay_ms);
    } else if (static_cast<int32_t>(now - this->reboot_at_ms_) >= 0) {
      App.safe_reboot();
    }
    return;
  }

  // Signed: the Zigbee task may store a block's time after `now` was taken.
  const bool stalled = this->downloading_.load() &&
                       static_cast<int32_t>(now - this->last_progress_ms_.load()) > static_cast<int32_t>(STALL_MS);
  if (stalled && esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS)) {
    // Checked again under the stack's lock, where no block can come in between.
    if (this->downloading_.load()) {
      ESP_LOGE(TAG, "No block for %" PRIu32 " s, download abandoned at %" PRIu32 " bytes", STALL_MS / 1000,
               this->offset_.load());
      this->end_download_();
      // The stack refuses later offers while it thinks a download runs.
      this->reset_upgrade_status_();
      this->notify_(ota::OTA_ERROR, 0.0f, static_cast<uint8_t>(ota::OTA_RESPONSE_ERROR_UNKNOWN));
    }
    esp_zigbee_lock_release();
  }
}

#ifdef USE_ZIGBEE_OTA_CONFIRM
void ZigbeeOTAComponent::confirm_loop_(uint32_t now) {
  if (this->server_answered_.load()) {
    this->pending_confirm_ = false;
    pending_confirm_instance = nullptr;
    ESP_LOGI(TAG, "Coordinator reached, new image confirmed");
    this->safe_mode_->release_app_confirmation();
  } else if (now - this->boot_ms_ > this->confirm_timeout_ms_) {
    ESP_LOGE(TAG, "Nothing from the coordinator within %" PRIu32 " s, rolling back", this->confirm_timeout_ms_ / 1000);
    App.run_safe_shutdown_hooks();
    esp_ota_mark_app_invalid_rollback_and_reboot();
  } else if (this->parent_->is_joined() &&
             (this->last_query_ms_ == 0 || now - this->last_query_ms_ > CONFIRM_QUERY_MS)) {
    this->last_query_ms_ = now;
    this->send_query_();
  }
}

#endif

void ZigbeeOTAComponent::send_query_() {
  ezb_zcl_ota_upgrade_query_next_image_req_cmd_t req = {};
  req.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
  req.cmd_ctrl.dst_addr.u.short_addr = 0x0000;  // the coordinator
  req.cmd_ctrl.dst_ep = 1;
  req.cmd_ctrl.src_ep = this->endpoint_;
  req.payload.fc = 0;
  req.payload.manuf_code = this->manufacturer_code_;
  req.payload.image_type = this->image_type_;
  req.payload.file_version = this->file_version_;
  if (!esp_zigbee_lock_acquire(10 / portTICK_PERIOD_MS))
    return;
  ezb_err_t err = ezb_zcl_ota_upgrade_query_next_image_cmd_req(&req);
  esp_zigbee_lock_release();
  ESP_LOGD(TAG, "Query Next Image sent (%d)", err);
}

// From both tasks: the stack calls it on its own task, the stall check from
// the main loop holding the stack's lock.
void ZigbeeOTAComponent::set_updating_(bool updating) {
  if (this->updating_.exchange(updating) == updating)
    return;
  if (this->parent_->is_sleepy()) {
    if (updating) {
      this->saved_poll_ms_ = ezb_nwk_get_keepalive_interval();
      ezb_nwk_set_keepalive_interval(this->fast_poll_ms_);
    } else if (this->saved_poll_ms_ != 0) {
      ezb_nwk_set_keepalive_interval(this->saved_poll_ms_);
    }
    ESP_LOGD(TAG, "Poll every %" PRIu32 " ms", updating ? this->fast_poll_ms_ : this->saved_poll_ms_);
  }
#ifdef USE_DEEP_SLEEP
  if (this->deep_sleep_ != nullptr) {
    if (updating) {
      this->deep_sleep_->prevent_deep_sleep();
    } else {
      this->deep_sleep_->allow_deep_sleep();
    }
  }
#endif
}

void ZigbeeOTAComponent::notify_(ota::OTAState state, float progress, uint8_t error) {
#ifdef USE_OTA_STATE_LISTENER
  this->notify_state_deferred_(state, progress, error);
#endif
}

void ZigbeeOTAComponent::on_ota_action(ezb_zcl_core_action_callback_id_t callback_id, void *message) {
  if (callback_id == EZB_ZCL_CORE_OTA_UPGRADE_QUERY_NEXT_IMAGE_RSP_CB_ID) {
    auto *msg = static_cast<ezb_zcl_ota_upgrade_query_next_image_rsp_message_t *>(message);
#ifdef USE_ZIGBEE_OTA_CONFIRM
    // Any answer, "no image" included, proves the round trip to the server.
    this->server_answered_ = true;
#endif
    if (msg->in.image.status != EZB_ZCL_OTA_UPGRADE_STATUS_CODE_SUCCESS && this->awaiting_answer_.exchange(false)) {
#ifdef USE_DEEP_SLEEP
      this->deep_sleep_->allow_deep_sleep();
#endif
    }
    if (msg->in.image.status == EZB_ZCL_OTA_UPGRADE_STATUS_CODE_SUCCESS) {
      ESP_LOGI(TAG, "Image offered: type 0x%04X, version 0x%08" PRIX32 ", %" PRIu32 " bytes", msg->in.image.image_type,
               msg->in.image.file_version, msg->in.image.size);
    } else {
      ESP_LOGD(TAG, "No image available (0x%02X)", msg->in.image.status);
    }
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
    return;
  }
  if (callback_id != EZB_ZCL_CORE_OTA_UPGRADE_CLIENT_PROGRESS_CB_ID)
    return;

  auto *msg = static_cast<ezb_zcl_ota_upgrade_client_progress_message_t *>(message);
  ezb_zcl_status_t result = EZB_ZCL_STATUS_SUCCESS;
  this->last_progress_ms_ = millis();

  switch (msg->in.progress) {
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_START:
      result = this->on_start_(msg->in.start.image_size, msg->in.start.file_version);
      break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_RECEIVING:
      result =
          this->on_receiving_(msg->in.receiving.file_offset, msg->in.receiving.block, msg->in.receiving.block_size);
      break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_CHECK:
      result = this->on_check_();
      break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_APPLY:
      // end() in CHECK already selected the new image for the next boot.
      if (!this->ended_.load())
        result = EZB_ZCL_STATUS_ABORT;
      break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH: {
      if (!this->ended_.load()) {
        ESP_LOGW(TAG, "Upgrade finished without a new image, no reboot");
        break;
      }
      // 0xFFFFFFFF asks to wait for an Upgrade End command from the server,
      // which Zigbee2MQTT and ZHA do not send.
      const uint32_t delay_s = msg->in.finish.count_down_delay;
      ESP_LOGI(TAG, "Update complete, the server asks for a reboot in %" PRIu32 " s", delay_s);
      if (delay_s != UINT32_MAX)
        this->reboot_delay_ms_ = std::min(delay_s, REBOOT_DELAY_MAX_S) * 1000;
      this->notify_(ota::OTA_COMPLETED, 100.0f, 0);
      this->reboot_pending_ = true;
      App.wake_loop_threadsafe();
      break;
    }
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT:
      this->on_abort_();
      break;
    default:
      break;
  }
  msg->out.result = result;
}

ezb_zcl_status_t ZigbeeOTAComponent::on_start_(uint32_t file_size, uint32_t file_version) {
#ifdef USE_ZIGBEE_OTA_CONFIRM
  // An offer is an answer from the server too (and begin() below confirms the running image).
  this->server_answered_ = true;
#endif
  if (this->ended_.load() || this->reboot_pending_.load()) {
    // The other slot holds the image selected for the next boot.
    ESP_LOGW(TAG, "Offer refused, a new image waits for the reboot");
    return EZB_ZCL_STATUS_ABORT;
  }
  ESP_LOGI(TAG, "Starting update: version 0x%08" PRIX32 ", %" PRIu32 " bytes", file_version, file_size);
  this->end_download_();
  this->file_size_ = file_size;
  this->header_length_ = 0;
  this->element_left_ = 0;
  this->image_written_ = 0;
  this->pending_len_ = 0;
  this->failed_ = false;
  this->last_percent_ = 0;
  this->resyncs_ = 0;
  this->offset_ = 0;

  this->backend_ = ota::make_ota_backend();
  // The image size is not known before the file is unpacked.
  ota::OTAResponseTypes err = this->backend_->begin(0);
  if (err != ota::OTA_RESPONSE_OK) {
    ESP_LOGE(TAG, "OTA begin failed (%d)", err);
    this->backend_->abort();
    this->backend_.reset();
    this->notify_(ota::OTA_ERROR, 0.0f, static_cast<uint8_t>(err));
    return EZB_ZCL_STATUS_ABORT;
  }
  this->downloading_ = true;
  this->awaiting_answer_ = false;
  this->set_updating_(true);
  this->notify_(ota::OTA_STARTED, 0.0f, 0);
  return EZB_ZCL_STATUS_SUCCESS;
}

ezb_zcl_status_t ZigbeeOTAComponent::on_receiving_(uint32_t block_offset, const uint8_t *data, uint32_t len) {
  if (this->failed_ || !this->downloading_.load())
    return EZB_ZCL_STATUS_ABORT;
  const uint32_t block_len = len;
  const uint32_t expected = this->offset_.load();
  if (block_offset > expected || block_offset + len <= expected) {
    // Not the block we need: a late answer to a block the stack already asked
    // for again, or a block past a gap that such an answer left behind.
    if (++this->resyncs_ > MAX_RESYNCS) {
      ESP_LOGE(TAG, "Block at %" PRIu32 ", expected %" PRIu32 ", giving up after %u skipped blocks in a row",
               block_offset, expected, MAX_RESYNCS);
      this->failed_ = true;
      return EZB_ZCL_STATUS_ABORT;
    }
    ESP_LOGW(TAG, "Block at %" PRIu32 " (expected %" PRIu32 "), skipped", block_offset, expected);
    this->resync_file_offset_(block_len);
    return EZB_ZCL_STATUS_SUCCESS;
  }
  if (block_offset < expected) {
    // Overlaps data already written: keep only the new part.
    const uint32_t skip = expected - block_offset;
    data += skip;
    len -= skip;
    block_offset = expected;
  }
  uint32_t pos = block_offset;
  this->offset_ = block_offset + len;
  this->resyncs_ = 0;

  while (len > 0) {
    if (this->header_length_ == 0 || pos < this->header_length_) {
      // File header: only identifier and length matter here, the stack already
      // matched manufacturer code, image type and version.
      if (pos < 8) {
        const uint32_t take = std::min<uint32_t>(len, 8 - pos);
        memcpy(this->pending_ + pos, data, take);
        pos += take;
        data += take;
        len -= take;
        if (pos == 8) {
          uint32_t ident;
          memcpy(&ident, this->pending_, 4);
          this->header_length_ = this->pending_[6] | (this->pending_[7] << 8);
          if (ident != OTA_FILE_IDENTIFIER || this->header_length_ < MIN_HEADER_LEN) {
            ESP_LOGE(TAG, "Not an OTA file (identifier 0x%08" PRIX32 ", header %" PRIu32 ")", ident,
                     this->header_length_);
            this->failed_ = true;
            return EZB_ZCL_STATUS_INVALID_IMAGE;
          }
        }
        continue;
      }
      const uint32_t skip = std::min<uint32_t>(len, this->header_length_ - pos);
      pos += skip;
      data += skip;
      len -= skip;
      continue;
    }
    if (this->element_left_ == 0) {
      // Sub-element header, possibly split across two blocks.
      const uint32_t take = std::min<uint32_t>(len, SUB_ELEMENT_HEADER_LEN - this->pending_len_);
      memcpy(this->pending_ + this->pending_len_, data, take);
      this->pending_len_ += take;
      pos += take;
      data += take;
      len -= take;
      if (this->pending_len_ == SUB_ELEMENT_HEADER_LEN) {
        const uint16_t tag = this->pending_[0] | (this->pending_[1] << 8);
        uint32_t length;
        memcpy(&length, this->pending_ + 2, 4);
        this->pending_len_ = 0;
        this->element_left_ = length;
        if (!this->begin_element_(tag, length)) {
          this->failed_ = true;
          return EZB_ZCL_STATUS_INVALID_IMAGE;
        }
      }
      continue;
    }
    const uint32_t take = std::min(len, this->element_left_);
    if (!this->write_element_(data, take, take == this->element_left_)) {
      this->failed_ = true;
      return EZB_ZCL_STATUS_ABORT;
    }
    this->element_left_ -= take;
    pos += take;
    data += take;
    len -= take;
  }
  this->resync_file_offset_(block_len);

  const uint8_t percent = this->file_size_ ? static_cast<uint8_t>(uint64_t(pos) * 100 / this->file_size_) : 0;
  if (percent / 10 != this->last_percent_ / 10) {
    ESP_LOGI(TAG, "Progress: %u%%", percent);
  }
  if (percent != this->last_percent_) {
    this->last_percent_ = percent;
    this->notify_(ota::OTA_IN_PROGRESS, percent, 0);
  }
  return EZB_ZCL_STATUS_SUCCESS;
}

ezb_zcl_status_t ZigbeeOTAComponent::on_check_() {
  this->downloading_ = false;
  ESP_LOGI(TAG, "Download complete: %" PRIu32 " bytes, image %" PRIu32 " bytes", this->offset_.load(),
           this->image_written_);
  const bool complete = !this->failed_ && this->backend_ != nullptr && this->offset_.load() == this->file_size_ &&
                        this->element_left_ == 0 && this->image_written_ > 0 &&
                        (this->format_ != ImageFormat::ZLIB || this->inflate_done_);
  this->release_inflater_();
  if (!complete) {
    ESP_LOGE(TAG, "Incomplete image");
    this->end_download_();
    this->notify_(ota::OTA_ERROR, 0.0f, static_cast<uint8_t>(ota::OTA_RESPONSE_ERROR_UPDATE_END));
    return EZB_ZCL_STATUS_INVALID_IMAGE;
  }
  // Checks the app image (magic, segments, SHA-256) and selects it for the next boot.
  ota::OTAResponseTypes err = this->backend_->end();
  if (err != ota::OTA_RESPONSE_OK) {
    ESP_LOGE(TAG, "Image check failed (%d)", err);
    this->end_download_();
    this->notify_(ota::OTA_ERROR, 0.0f, static_cast<uint8_t>(err));
    return EZB_ZCL_STATUS_INVALID_IMAGE;
  }
  this->backend_.reset();
  this->ended_ = true;
  ESP_LOGI(TAG, "Image verified and selected for the next boot");
  return EZB_ZCL_STATUS_SUCCESS;
}

void ZigbeeOTAComponent::on_abort_() {
  if (this->reboot_pending_.load())
    return;
  if (this->ended_.load()) {
    // The new image was already selected: boot the running one again.
    ESP_LOGW(TAG, "Update aborted after the image check");
    esp_err_t err = esp_ota_set_boot_partition(esp_ota_get_running_partition());
    if (err != ESP_OK)
      ESP_LOGE(TAG, "Could not select the running image again: %s", esp_err_to_name(err));
    this->ended_ = false;
  }
  ESP_LOGW(TAG, "Update aborted at %" PRIu32 " bytes", this->offset_.load());
  // A failure on this side (bad block, flash write) shows as an error, an
  // abort from the server as an abort.
  const bool failed = this->failed_;
  this->failed_ = false;
  this->end_download_();
  if (failed) {
    this->notify_(ota::OTA_ERROR, 0.0f, static_cast<uint8_t>(ota::OTA_RESPONSE_ERROR_UNKNOWN));
  } else {
    this->notify_(ota::OTA_ABORT, 0.0f, 0);
  }
}

bool ZigbeeOTAComponent::begin_element_(uint16_t tag, uint32_t length) {
  this->release_inflater_();
  switch (tag) {
    case TAG_UPGRADE_IMAGE:
      this->format_ = ImageFormat::RAW;
      break;
    case TAG_IMAGE_ZLIB:
      this->inflater_ = calloc(1, sizeof(tinfl_decompressor));
      this->window_ = static_cast<uint8_t *>(malloc(TINFL_LZ_DICT_SIZE));
      if (this->inflater_ == nullptr || this->window_ == nullptr) {
        ESP_LOGE(TAG, "No memory for the inflater");
        this->release_inflater_();
        return false;
      }
      tinfl_init(static_cast<tinfl_decompressor *>(this->inflater_));
      this->format_ = ImageFormat::ZLIB;
      break;
    default:
      // Some other sub-element (signature, certificate, ...): skipped.
      this->format_ = ImageFormat::NONE;
      return true;
  }
  ESP_LOGD(TAG, "Image sub-element 0x%04X, %" PRIu32 " bytes", tag, length);
  return true;
}

bool ZigbeeOTAComponent::write_element_(const uint8_t *data, uint32_t len, bool last) {
  switch (this->format_) {
    case ImageFormat::RAW:
      return this->write_image_(data, len);
    case ImageFormat::ZLIB:
      return this->inflate_(data, len, last);
    case ImageFormat::NONE:
      break;
  }
  return true;
}

bool ZigbeeOTAComponent::write_image_(const uint8_t *data, size_t len) {
  // The backend takes a non-const pointer but does not change the data.
  ota::OTAResponseTypes err = this->backend_->write(const_cast<uint8_t *>(data), len);
  if (err != ota::OTA_RESPONSE_OK) {
    ESP_LOGE(TAG, "Writing the image failed at %" PRIu32 " (%d)", this->image_written_, err);
    return false;
  }
  this->image_written_ += len;
  return true;
}

bool ZigbeeOTAComponent::inflate_(const uint8_t *data, uint32_t len, bool last) {
  auto *inflater = static_cast<tinfl_decompressor *>(this->inflater_);
  const int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
  while (!this->inflate_done_) {
    size_t in_bytes = len;
    size_t out_bytes = TINFL_LZ_DICT_SIZE - this->window_pos_;
    tinfl_status status = tinfl_decompress(inflater, data, &in_bytes, this->window_, this->window_ + this->window_pos_,
                                           &out_bytes, flags);
    data += in_bytes;
    len -= in_bytes;
    if (out_bytes > 0) {
      if (!this->write_image_(this->window_ + this->window_pos_, out_bytes))
        return false;
      this->window_pos_ = (this->window_pos_ + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
    }
    if (status < TINFL_STATUS_DONE) {
      ESP_LOGE(TAG, "Inflate failed (%d)", status);
      return false;
    }
    if (status == TINFL_STATUS_DONE) {
      this->inflate_done_ = true;
    } else if (status == TINFL_STATUS_NEEDS_MORE_INPUT) {
      break;
    }
    // TINFL_STATUS_HAS_MORE_OUTPUT: the window is full, go round again.
  }
  return true;
}

void ZigbeeOTAComponent::release_inflater_() {
  free(this->inflater_);
  this->inflater_ = nullptr;
  free(this->window_);
  this->window_ = nullptr;
  this->window_pos_ = 0;
  this->inflate_done_ = false;
  this->format_ = ImageFormat::NONE;
}

void ZigbeeOTAComponent::end_download_() {
  this->downloading_ = false;
  // A checked image keeps the fast poll until the reboot (APPLY, FINISH).
  if (!this->ended_.load())
    this->set_updating_(false);
  this->release_inflater_();
  if (this->backend_ != nullptr) {
    this->backend_->abort();
    this->backend_.reset();
  }
}

void ZigbeeOTAComponent::reset_upgrade_status_() {
  uint8_t status = EZB_ZCL_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_NORMAL;
  ezb_zcl_set_attr_value(this->endpoint_, EZB_ZCL_CLUSTER_ID_OTA_UPGRADE, EZB_ZCL_CLUSTER_CLIENT,
                         EZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_UPGRADE_STATUS_ID, EZB_ZCL_STD_MANUF_CODE, &status, false);
}

// The stack does not check the offset of a block answer: it adds every
// answer's length to its FileOffset attribute, a repeated block's too, and asks
// for the next block from there. After a late duplicate it runs ahead of the
// data written and leaves a gap. When FileOffset is past where the data ends,
// it is set back by one block: the answer already on its way (if any) is
// skipped and the request after it asks for the right block; if nothing is on
// its way, the stack asks for a block that overlaps the data written, whose
// new part is kept. Either way it converges on the next answer, and the
// download cannot end early with the last block still missing.
// Idea from github.com/jabla/esphome-hiflow-zigbee.
void ZigbeeOTAComponent::resync_file_offset_(uint32_t block_len) {
  ezb_zcl_attr_desc_t attr =
      ezb_zcl_get_attr_desc(this->endpoint_, EZB_ZCL_CLUSTER_ID_OTA_UPGRADE, EZB_ZCL_CLUSTER_CLIENT,
                            EZB_ZCL_ATTR_OTA_UPGRADE_FILE_OFFSET_ID, EZB_ZCL_STD_MANUF_CODE);
  uint32_t stack_offset = 0;
  if (attr == EZB_INVALID_ZCL_ATTR_DESC || ezb_zcl_attr_desc_get_value(attr, &stack_offset) != EZB_ERR_NONE)
    return;
  const uint32_t offset = this->offset_.load();
  if (stack_offset <= offset)
    return;
  uint32_t target = offset > block_len ? offset - block_len : 0;
  ESP_LOGW(TAG, "FileOffset %" PRIu32 " set to %" PRIu32 " (data written up to %" PRIu32 ")", stack_offset, target,
           offset);
  ezb_zcl_set_attr_value(this->endpoint_, EZB_ZCL_CLUSTER_ID_OTA_UPGRADE, EZB_ZCL_CLUSTER_CLIENT,
                         EZB_ZCL_ATTR_OTA_UPGRADE_FILE_OFFSET_ID, EZB_ZCL_STD_MANUF_CODE, &target, false);
}

}  // namespace esphome::zigbee

#endif  // USE_ESP32 && USE_ZIGBEE_OTA
