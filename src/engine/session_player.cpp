#include "daw/session_player.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace daw {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free &&
              std::atomic<double>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
              "Playback status requires lock-free atomics");
void SessionPlayer::Ramp::set(double value) noexcept {
  target = value; remaining = kRampFrames;
}
double SessionPlayer::Ramp::next() noexcept {
  if (remaining) { current += (target - current) / remaining; --remaining; }
  return current;
}
SessionPlayer::SessionPlayer(const Session& session, std::vector<AudioBuffer> audio)
    : audio_(std::move(audio)) {
  validateSession(session);
  if (audio_.size() != session.routes.size()) throw std::invalid_argument("playback route/audio count mismatch");
  frames_ = audio_.front().frameCount();
  if (!frames_ || frames_ > kMaxAudioBytes / sizeof(float) / 2 / audio_.size())
    throw std::invalid_argument("playback audio is empty or exceeds 512 MiB");
  for (std::size_t i = 0; i < audio_.size(); ++i) {
    const auto& a = audio_[i];
    if (a.channels != 2 || a.sample_rate != 48000 || a.samples.size() != frames_ * 2)
      throw std::invalid_argument("playback requires aligned stereo 48 kHz audio");
    for (const auto sample : a.samples) if (!std::isfinite(sample))
      throw std::invalid_argument("playback audio contains non-finite samples");
  }
  initialize(session);
}
SessionPlayer::SessionPlayer(std::unique_ptr<StreamingAudio> audio, const Session& session)
    : stream_(std::move(audio)) {
  validateSession(session);
  if (!stream_ || stream_->trackCount() != session.routes.size())
    throw std::invalid_argument("playback route/stream count mismatch");
  frames_ = stream_->frameCount();
  initialize(session);
}
void SessionPlayer::initialize(const Session& session) {
  for (const auto& r : session.routes) {
    const auto i = part_ids_.size();
    part_ids_.push_back(r.part_id); instruments_.push_back(r.instrument);
    tracks_[i].gain = r.gain_db; tracks_[i].balance = r.balance;
    tracks_[i].mute = r.mute; tracks_[i].solo = r.solo;
  }
  master_.current = master_.target = std::pow(10., session.master_gain_db / 20.);
  targets(true);
}
bool SessionPlayer::acceptsFormat(double rate, std::uint32_t channels) const noexcept {
  return rate == 48000 && channels == 2;
}
bool SessionPlayer::matchesRoutes(const Session& session) const noexcept {
  if (session.routes.size() != part_ids_.size()) return false;
  for (std::size_t i = 0; i < part_ids_.size(); ++i)
    if (session.routes[i].part_id != part_ids_[i] || session.routes[i].instrument != instruments_[i]) return false;
  return true;
}
bool SessionPlayer::enqueue(const PlaybackCommand& c) noexcept {
  bool valid = std::isfinite(c.value);
  switch (c.action) {
    case PlaybackAction::Gain: case PlaybackAction::Master:
      valid = valid && c.value >= -60 && c.value <= 12; break;
    case PlaybackAction::Balance: valid = valid && c.value >= -1 && c.value <= 1; break;
    case PlaybackAction::Mute: case PlaybackAction::Solo: valid = valid && (c.value == 0 || c.value == 1); break;
    case PlaybackAction::Seek: valid = valid && c.frame <= frames_; break;
    case PlaybackAction::Play: case PlaybackAction::Pause: case PlaybackAction::Stop: break;
    default: valid = false;
  }
  if (c.action == PlaybackAction::Gain || c.action == PlaybackAction::Balance ||
      c.action == PlaybackAction::Mute || c.action == PlaybackAction::Solo) valid = valid && c.track < trackCount();
  if (valid && commands_.push(c)) return true;
  rejected_.fetch_add(1, std::memory_order_relaxed);
  return false;
}
void SessionPlayer::targets(bool immediate) noexcept {
  bool solo = false;
  for (std::size_t i = 0; i < trackCount(); ++i) solo = solo || tracks_[i].solo;
  constexpr double half_pi = 1.57079632679489661923;
  for (std::size_t i = 0; i < trackCount(); ++i) {
    auto& t = tracks_[i];
    const double gain = t.mute || (solo && !t.solo) ? 0 : std::pow(10., t.gain / 20.);
    const double left = gain * (t.balance >= 1 ? 0 : t.balance > 0 ? std::cos(t.balance * half_pi) : 1);
    const double right = gain * (t.balance <= -1 ? 0 : t.balance < 0 ? std::cos(t.balance * half_pi) : 1);
    if (left != t.left.target) t.left.set(left);
    if (right != t.right.target) t.right.set(right);
    if (immediate) { t.left.current = left; t.right.current = right; t.left.remaining = t.right.remaining = 0; }
  }
}
void SessionPlayer::transition() noexcept {
  transition_from_ = last_; transition_left_ = kRampFrames;
}
void SessionPlayer::apply(const PlaybackCommand& c) noexcept {
  switch (c.action) {
    case PlaybackAction::Play:
      if (!playing_ && position_ < frames_) { playing_ = true; transition(); } break;
    case PlaybackAction::Pause:
      if (playing_) { playing_ = false; transition(); } break;
    case PlaybackAction::Stop: playing_ = false; position_ = 0; transition(); break;
    case PlaybackAction::Seek: position_ = static_cast<std::size_t>(c.frame); transition(); break;
    case PlaybackAction::Master: master_.set(std::pow(10., c.value / 20.)); break;
    case PlaybackAction::Gain: tracks_[c.track].gain = c.value; targets(); break;
    case PlaybackAction::Balance: tracks_[c.track].balance = c.value; targets(); break;
    case PlaybackAction::Mute: tracks_[c.track].mute = c.value != 0; targets(); break;
    case PlaybackAction::Solo: tracks_[c.track].solo = c.value != 0; targets(); break;
  }
}
void SessionPlayer::render(float* stereo, std::uint32_t frames) noexcept {
  if (!stereo || !frames) return;
  PlaybackCommand command;
  // A concurrent producer cannot prolong this loop without bound.
  for (std::size_t i = 0; i < kCapacity && commands_.pop(&command); ++i) apply(command);
  double peak = 0;
  for (std::uint32_t f = 0; f < frames; ++f) {
    if (playing_ && position_ == frames_) { playing_ = false; transition(); }
    const float* disk = stream_ ? stream_->frame(position_) : nullptr;
    const bool ready = !stream_ || disk;
    const bool buffering = playing_ && !ready;
    if (buffering != buffering_) { buffering_ = buffering; transition(); }
    if (buffering_) ++buffering_frames_;
    std::array<double, 2> sample{};
    for (std::size_t i = 0; i < trackCount(); ++i) {
      const double l = tracks_[i].left.next(), r = tracks_[i].right.next();
      if (playing_ && ready) {
        sample[0] += (stream_ ? disk[i * 2] : audio_[i].samples[position_ * 2]) * l;
        sample[1] += (stream_ ? disk[i * 2 + 1] : audio_[i].samples[position_ * 2 + 1]) * r;
      }
    }
    const double master = master_.next();
    const double blend = transition_left_ ?
        static_cast<double>(kRampFrames - transition_left_ + 1) / kRampFrames : 1;
    for (std::size_t channel = 0; channel < 2; ++channel) {
      const double value = sample[channel] * master * blend + transition_from_[channel] * (1 - blend);
      peak = std::max(peak, std::abs(value));
      if (std::abs(value) > 1) ++clipped_;
      last_[channel] = std::clamp(value, -1., 1.);
      stereo[static_cast<std::size_t>(f) * 2 + channel] = static_cast<float>(last_[channel]);
    }
    if (transition_left_) --transition_left_;
    if (playing_ && ready) ++position_;
  }
  // Mark EOF even when it coincides exactly with the callback boundary.
  if (playing_ && position_ == frames_) { playing_ = false; transition(); }
  visible_buffering_frames_.store(buffering_frames_, std::memory_order_relaxed);
  visible_buffering_.store(buffering_, std::memory_order_relaxed);
  visible_position_.store(position_, std::memory_order_relaxed);
  visible_playing_.store(playing_, std::memory_order_relaxed);
  visible_clipped_.store(clipped_, std::memory_order_relaxed);
  visible_peak_.store(peak, std::memory_order_relaxed);
}
PlaybackStatus SessionPlayer::status() const noexcept {
  return {visible_position_.load(std::memory_order_relaxed), visible_clipped_.load(std::memory_order_relaxed),
          rejected_.load(std::memory_order_relaxed), visible_playing_.load(std::memory_order_relaxed),
          visible_peak_.load(std::memory_order_relaxed), visible_buffering_frames_.load(std::memory_order_relaxed),
          visible_buffering_.load(std::memory_order_relaxed), stream_ && stream_->failed()};
}
}
