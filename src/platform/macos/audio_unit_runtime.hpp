#pragma once
#include <cstdint>
#include <vector>

namespace daw {
// Must be called on the main thread. No window or audio device is created.
// This is an offline host lifecycle helper, never an audio callback operation.
void prepareAudioUnitRuntime();
void serviceAudioUnitRuntime(double seconds);
// Parses the bounded saved SWAM Cello state without creating a plugin or app.
// Validates AU identity, supported range and the stored semitone parameter.
int swamCelloStateTranspose(const std::vector<std::uint8_t>& bytes);
// Creates a new state with only the stored transpose parameter changed to zero.
std::vector<std::uint8_t> swamCelloConcertPitchState(const std::vector<std::uint8_t>& bytes);
}
