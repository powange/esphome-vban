#pragma once
#include "esphome.h"
#include "esphome/components/microphone/microphone.h"

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <arpa/inet.h>

#include <cstring>
#include <algorithm>
#include <atomic>
#include <vector>

namespace esphome {
namespace vban_sender {

static const uint8_t VBAN_SR_16000 = 8;

class VBANSender : public Component {
 public:
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void add_target(const std::string &ip, uint16_t port) {
    Target t;
    t.ip = ip;
    t.port = port;
    targets_.push_back(t);
  }
  void set_stream_name(const std::string &name) { stream_name_ = name; }
  void set_gain(float gain) { gain_ = gain; }

  float get_setup_priority() const override { return setup_priority::LATE; }

  void setup() override {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
      ESP_LOGE("vban", "socket() failed: errno=%d", errno);
      mark_failed();
      return;
    }

    uint8_t ttl = 2;
    ::setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    int bcast = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    for (auto &t : targets_) {
      std::memset(&t.addr, 0, sizeof(t.addr));
      t.addr.sin_family = AF_INET;
      t.addr.sin_port = htons(t.port);
      if (::inet_aton(t.ip.c_str(), &t.addr.sin_addr) == 0) {
        ESP_LOGE("vban", "invalid target ip: %s", t.ip.c_str());
        ::close(sock_);
        sock_ = -1;
        mark_failed();
        return;
      }
    }

    if (mic_ != nullptr) {
      mic_->add_data_callback([this](const std::vector<uint8_t> &data) {
        this->send_audio(data.data(), data.size());
      });
    }

    ESP_LOGI("vban", "VBAN sender ready -> %u target(s) stream=%s",
             (unsigned) targets_.size(), stream_name_.c_str());
  }

  void loop() override {
    if (!mic_started_ && mic_ != nullptr) {
      mic_->start();
      mic_started_ = true;
      ESP_LOGI("vban", "Microphone started, streaming to %u target(s)",
               (unsigned) targets_.size());
    }
  }

  void dump_config() override {
    ESP_LOGCONFIG("vban", "VBAN Sender:");
    for (const auto &t : targets_) {
      ESP_LOGCONFIG("vban", "  Target: %s:%d (send errors: %u)",
                    t.ip.c_str(), t.port, (unsigned) t.send_errors);
    }
    ESP_LOGCONFIG("vban", "  Stream name: %s", stream_name_.c_str());
    ESP_LOGCONFIG("vban", "  Socket: %s", sock_ >= 0 ? "OK" : "FAILED");
    ESP_LOGCONFIG("vban", "  Mic started: %s", mic_started_ ? "YES" : "NO");
    ESP_LOGCONFIG("vban", "  Frames sent: %u", (unsigned) frame_counter_.load());
  }

  void send_audio(const uint8_t *data, size_t len) {
    if (!data || len == 0 || sock_ < 0) return;
    size_t sample_count = len / 2;
    if (sample_count == 0) return;

    const int16_t *src = reinterpret_cast<const int16_t *>(data);
    const int16_t *samples = src;
    if (gain_ != 1.0f) {
      if (gain_scratch_.size() < sample_count) {
        gain_scratch_.resize(sample_count);
      }
      for (size_t i = 0; i < sample_count; i++) {
        int32_t v = (int32_t)(src[i] * gain_);
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        gain_scratch_[i] = (int16_t) v;
      }
      samples = gain_scratch_.data();
    }

    size_t offset = 0;
    while (offset < sample_count) {
      size_t chunk = std::min(sample_count - offset, (size_t)256);
      send_packet_(reinterpret_cast<const uint8_t *>(samples + offset), chunk);
      offset += chunk;
    }
  }

 protected:
  void send_packet_(const uint8_t *data, size_t sample_count) {
    uint8_t packet[28 + 512];
    uint8_t *header = packet;

    header[0] = 'V'; header[1] = 'B'; header[2] = 'A'; header[3] = 'N';
    header[4] = VBAN_SR_16000;
    header[5] = (uint8_t)(sample_count - 1);
    header[6] = 0;      // mono
    header[7] = 0x01;   // PCM 16-bit

    std::memset(&header[8], 0, 16);
    std::strncpy((char *)&header[8], stream_name_.c_str(), 16);

    uint32_t fc = frame_counter_.fetch_add(1, std::memory_order_relaxed);
    header[24] = fc & 0xFF;
    header[25] = (fc >> 8) & 0xFF;
    header[26] = (fc >> 16) & 0xFF;
    header[27] = (fc >> 24) & 0xFF;

    size_t payload = sample_count * 2;
    std::memcpy(packet + 28, data, payload);

    // Same packet (and frame counter) for every target.
    for (auto &t : targets_) {
      if (::sendto(sock_, packet, 28 + payload, 0,
                   (struct sockaddr *)&t.addr, sizeof(t.addr)) < 0) {
        t.send_errors++;
        uint32_t now = millis();
        if (now - t.last_error_log_ms >= 5000) {
          t.last_error_log_ms = now;
          ESP_LOGW("vban", "sendto %s:%d failed: errno=%d (%u errors)",
                   t.ip.c_str(), t.port, errno, (unsigned) t.send_errors);
        }
      }
    }
  }

  struct Target {
    std::string ip;
    uint16_t port{6980};
    struct sockaddr_in addr{};
    uint32_t send_errors{0};
    uint32_t last_error_log_ms{0};
  };

  microphone::Microphone *mic_{nullptr};
  bool mic_started_{false};
  int sock_{-1};
  std::vector<Target> targets_;
  std::string stream_name_{"AtomEcho"};
  float gain_{1.0f};
  std::vector<int16_t> gain_scratch_;
  std::atomic<uint32_t> frame_counter_{0};
};

}  // namespace vban_sender
}  // namespace esphome
