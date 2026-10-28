#pragma once
#include "esphome.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/core/helpers.h"

#include <lwip/sockets.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <cstring>
#include <vector>
#include <algorithm>

namespace esphome {
namespace vban_receiver {

static const uint8_t VBAN_SR_16000 = 8;
static const uint32_t VBAN_BYTES_PER_MS = 32;  // 16 kHz, mono, 16-bit
static const int VBAN_RCVBUF_BYTES = 32 * 1024;
static const int VBAN_MAX_PACKETS_PER_LOOP = 64;

// IDLE -> WAITING_MIC -> BUFFERING <-> PLAYING -> STOPPING -> IDLE
enum class PlaybackState : uint8_t { IDLE, WAITING_MIC, BUFFERING, PLAYING, STOPPING };

class VBANReceiver : public Component {
 public:
  void set_speaker(speaker::Speaker *spk) { speaker_ = spk; }
  void set_microphone(microphone::Microphone *mic) { mic_ = mic; }
  void set_listen_port(uint16_t port) { listen_port_ = port; }
  void set_stream_name(const std::string &name) { stream_name_ = name; }
  void set_idle_timeout_ms(uint32_t ms) { idle_timeout_ms_ = ms; }
  void set_prebuffer_ms(uint32_t ms) { prebuffer_ms_ = ms; }

  float get_setup_priority() const override { return setup_priority::LATE; }

  void setup() override {
    ring_.assign(kRingCapacity, 0);

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
      ESP_LOGE("vban_rx", "socket() failed: errno=%d", errno);
      mark_failed();
      return;
    }

    int reuse = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Absorb sender bursts between two loop() calls. Needs CONFIG_LWIP_SO_RCVBUF;
    // the packet count is capped separately by CONFIG_LWIP_UDP_RECVMBOX_SIZE.
    int rcvbuf = VBAN_RCVBUF_BYTES;
    if (::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
      ESP_LOGW("vban_rx", "setsockopt(SO_RCVBUF, %d) failed: errno=%d (enable CONFIG_LWIP_SO_RCVBUF)",
               rcvbuf, errno);
    }

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(listen_port_);

    if (::bind(sock_, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
      ESP_LOGE("vban_rx", "bind(%d) failed: errno=%d", listen_port_, errno);
      ::close(sock_);
      sock_ = -1;
      mark_failed();
      return;
    }

    int flags = ::fcntl(sock_, F_GETFL, 0);
    ::fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

    ESP_LOGI("vban_rx", "Listening on UDP %d for VBAN stream '%s'",
             listen_port_, stream_name_.c_str());
  }

  void loop() override {
    uint8_t buf[2048];
    struct sockaddr_in from;

    // Drain the socket completely: lwIP drops datagrams once its queue is full.
    for (int i = 0; i < VBAN_MAX_PACKETS_PER_LOOP; i++) {
      socklen_t fromlen = sizeof(from);
      int n = ::recvfrom(sock_, buf, sizeof(buf), 0,
                        (struct sockaddr *)&from, &fromlen);
      if (n < 0) break;  // EAGAIN/EWOULDBLOCK: queue empty (or socket error)
      if (n < 28) continue;
      handle_packet_(buf, n);
    }

    update_playback_();
  }

  void dump_config() override {
    ESP_LOGCONFIG("vban_rx", "VBAN Receiver:");
    ESP_LOGCONFIG("vban_rx", "  Listen port: %d", listen_port_);
    ESP_LOGCONFIG("vban_rx", "  Stream name: %s", stream_name_.c_str());
    ESP_LOGCONFIG("vban_rx", "  Idle timeout: %u ms", (unsigned) idle_timeout_ms_);
    ESP_LOGCONFIG("vban_rx", "  Prebuffer: %u ms", (unsigned) prebuffer_ms_);
    ESP_LOGCONFIG("vban_rx", "  Socket: %s", sock_ >= 0 ? "OK" : "FAILED");
    ESP_LOGCONFIG("vban_rx", "  Packets received: %u", (unsigned) packets_received_);
    ESP_LOGCONFIG("vban_rx", "  Packets lost:     %u", (unsigned) packets_lost_);
    ESP_LOGCONFIG("vban_rx", "  Out of order:     %u", (unsigned) packets_out_of_order_);
    ESP_LOGCONFIG("vban_rx", "  Underruns:        %u", (unsigned) underruns_);
  }

 protected:
  void handle_packet_(const uint8_t *buf, int n) {
    if (buf[0] != 'V' || buf[1] != 'B' || buf[2] != 'A' || buf[3] != 'N') return;

    // Filter by stream name first: the port also carries other devices' streams.
    if (std::strncmp((const char *) buf + 8, stream_name_.c_str(), 16) != 0) return;

    uint8_t sub_protocol = buf[4] & 0xE0;
    if (sub_protocol != 0x00) return;  // only audio sub-protocol

    uint8_t format_bit = buf[7] & 0x07;
    if (format_bit != 0x01) {
      log_format_warning_("format", format_bit);
      return;
    }

    uint8_t sr_index = buf[4] & 0x1F;
    if (sr_index != VBAN_SR_16000) {
      log_format_warning_("sample_rate", sr_index);
      return;
    }

    uint8_t channels = buf[6] + 1;
    if (channels != 1) {
      log_format_warning_("channels", channels);
      return;
    }

    const uint8_t *pcm = buf + 28;
    size_t pcm_len = n - 28;
    if (pcm_len == 0) return;

    uint32_t frame = ((uint32_t) buf[24])
                   | ((uint32_t) buf[25] << 8)
                   | ((uint32_t) buf[26] << 16)
                   | ((uint32_t) buf[27] << 24);
    if (state_ == PlaybackState::IDLE) {
      begin_stream_();
    }

    // Sequence tracking restarts with each stream: senders may reset their counter.
    if (stream_.received > 0) {
      uint32_t expected = last_frame_counter_ + 1;
      if (frame != expected) {
        int32_t diff = (int32_t)(frame - expected);
        if (diff > 0) {
          stream_.lost += diff;
          packets_lost_ += diff;
        } else {
          stream_.out_of_order++;
          packets_out_of_order_++;
        }
      }
    }
    last_frame_counter_ = frame;

    stream_.received++;
    packets_received_++;
    last_packet_ms_ = millis();

    // The pipeline ran dry and audio is still coming: that was a real underrun,
    // not the end of the stream.
    if (underrun_pending_) {
      underrun_pending_ = false;
      stream_.underruns++;
      underruns_++;
    }

    // Always go through the ring: it is the jitter buffer.
    ring_write_(pcm, pcm_len);
  }

  void update_playback_() {
    uint32_t now = millis();
    switch (state_) {
      case PlaybackState::IDLE:
        break;

      case PlaybackState::WAITING_MIC:
        // mic_->stop() is asynchronous: starting the speaker before the mic has
        // released the shared I2S bus fails with "Parent bus is busy".
        if (mic_ == nullptr || mic_->is_stopped()) {
          start_speaker_();
        } else if (now - state_since_ms_ > kMicStopTimeoutMs) {
          ESP_LOGW("vban_rx", "Microphone still not stopped after %u ms, starting speaker anyway",
                   (unsigned) kMicStopTimeoutMs);
          start_speaker_();
        }
        break;

      case PlaybackState::BUFFERING:
        // Start once enough audio is queued, or flush a short reply that will
        // never reach the threshold.
        if (!ring_empty_() &&
            (ring_size_ >= buffer_target_bytes_ || now - last_packet_ms_ > kFlushAfterMs)) {
          set_state_(PlaybackState::PLAYING);
          speaker_dry_at_us_ = micros();
        }
        break;

      case PlaybackState::PLAYING:
        drain_ring_to_speaker_();
        if (ring_empty_() && (int32_t)(micros() - speaker_dry_at_us_) >= 0) {
          // Nothing left to play anywhere: rebuffer with a smaller threshold.
          // Counted as an underrun only if another packet arrives (otherwise it
          // is just the end of the stream).
          buffer_target_bytes_ = prebuffer_ms_ / 2 * VBAN_BYTES_PER_MS;
          underrun_pending_ = true;
          set_state_(PlaybackState::BUFFERING);
        }
        break;

      case PlaybackState::STOPPING:
        if (speaker_ == nullptr || speaker_->is_stopped()) {
          finish_stop_();
        } else if (now - state_since_ms_ > kSpeakerStopTimeoutMs) {
          ESP_LOGW("vban_rx", "Speaker still not stopped after %u ms",
                   (unsigned) kSpeakerStopTimeoutMs);
          finish_stop_();
        }
        return;
    }

    if (state_ != PlaybackState::IDLE && now - last_packet_ms_ > idle_timeout_ms_) {
      stop_playback_();
    }
  }

  void drain_ring_to_speaker_() {
    if (speaker_ == nullptr || !speaker_->is_running()) return;
    while (!ring_empty_()) {
      size_t tail = (write_idx_ + kRingCapacity - ring_size_) % kRingCapacity;
      size_t contig = std::min(ring_size_, kRingCapacity - tail);
      size_t written = speaker_->play(ring_.data() + tail, contig);
      if (written == 0) return;
      ring_size_ -= written;
      // Estimate when the speaker will have played everything handed to it.
      uint32_t now = micros();
      if ((int32_t)(now - speaker_dry_at_us_) > 0) speaker_dry_at_us_ = now;
      speaker_dry_at_us_ += written * 1000 / VBAN_BYTES_PER_MS;
      if (written < contig) return;  // speaker back-pressure
    }
  }

  void ring_write_(const uint8_t *data, size_t len) {
    size_t room = kRingCapacity - ring_size_;
    size_t to_copy = std::min(len, room);
    if (to_copy == 0) return;
    size_t first = std::min(to_copy, kRingCapacity - write_idx_);
    std::memcpy(ring_.data() + write_idx_, data, first);
    if (to_copy > first) {
      std::memcpy(ring_.data(), data + first, to_copy - first);
    }
    write_idx_ = (write_idx_ + to_copy) % kRingCapacity;
    ring_size_ += to_copy;
  }

  bool ring_empty_() const { return ring_size_ == 0; }

  void ring_reset_() {
    ring_size_ = 0;
    write_idx_ = 0;
  }

  void log_format_warning_(const char *field, uint8_t value) {
    uint32_t now = millis();
    if (now - last_format_warning_ms_ < 5000) return;
    last_format_warning_ms_ = now;
    ESP_LOGW("vban_rx", "Rejecting packet: unsupported %s=%u (expected mono PCM16 @ 16kHz)",
             field, (unsigned) value);
  }

  void set_state_(PlaybackState state) {
    state_ = state;
    state_since_ms_ = millis();
    // Run loop() continuously while a stream is active so the socket is read
    // often enough.
    if (state == PlaybackState::IDLE) {
      high_freq_.stop();
    } else {
      high_freq_.start();
    }
  }

  void begin_stream_() {
    if (mic_ != nullptr && mic_->is_running()) {
      mic_->stop();
    }
    buffer_target_bytes_ = prebuffer_ms_ * VBAN_BYTES_PER_MS;
    underrun_pending_ = false;
    set_state_(PlaybackState::WAITING_MIC);
    ESP_LOGD("vban_rx", "Stream '%s' started", stream_name_.c_str());
  }

  void start_speaker_() {
    if (speaker_ != nullptr && !speaker_->is_running()) {
      speaker_->start();
    }
    set_state_(PlaybackState::BUFFERING);
  }

  void stop_playback_() {
    if (speaker_ != nullptr && !speaker_->is_stopped()) {
      speaker_->stop();
    }
    ring_reset_();
    underrun_pending_ = false;
    ESP_LOGD("vban_rx", "Stream '%s' ended: %u packets, %u lost, %u out of order, %u underruns",
             stream_name_.c_str(), (unsigned) stream_.received, (unsigned) stream_.lost,
             (unsigned) stream_.out_of_order, (unsigned) stream_.underruns);
    stream_ = {};
    set_state_(PlaybackState::STOPPING);
  }

  void finish_stop_() {
    if (!ring_empty_()) {
      // A new stream started while the speaker was stopping; the mic is still paused.
      begin_stream_();
      return;
    }
    if (mic_ != nullptr && mic_->is_stopped()) {
      mic_->start();
    }
    set_state_(PlaybackState::IDLE);
    ESP_LOGD("vban_rx", "Stream idle, mic resumed");
  }

  struct StreamStats {
    uint32_t received{0};
    uint32_t lost{0};
    uint32_t out_of_order{0};
    uint32_t underruns{0};
  };

  static constexpr uint32_t kMicStopTimeoutMs = 500;
  static constexpr uint32_t kSpeakerStopTimeoutMs = 500;
  static constexpr uint32_t kFlushAfterMs = 150;

  speaker::Speaker *speaker_{nullptr};
  microphone::Microphone *mic_{nullptr};
  int sock_{-1};
  uint16_t listen_port_{6980};
  std::string stream_name_;
  uint32_t idle_timeout_ms_{1500};
  uint32_t prebuffer_ms_{200};
  uint32_t last_packet_ms_{0};
  uint32_t last_format_warning_ms_{0};
  uint32_t packets_received_{0};
  uint32_t packets_lost_{0};
  uint32_t packets_out_of_order_{0};
  uint32_t underruns_{0};
  uint32_t last_frame_counter_{0};
  StreamStats stream_;

  PlaybackState state_{PlaybackState::IDLE};
  uint32_t state_since_ms_{0};
  size_t buffer_target_bytes_{0};
  uint32_t speaker_dry_at_us_{0};
  bool underrun_pending_{false};
  HighFrequencyLoopRequester high_freq_;

  // Fixed-capacity circular buffer (~2 s at 16 kHz 16-bit mono).
  static constexpr size_t kRingCapacity = 64 * 1024;
  std::vector<uint8_t> ring_;
  size_t write_idx_{0};
  size_t ring_size_{0};
};

}  // namespace vban_receiver
}  // namespace esphome
