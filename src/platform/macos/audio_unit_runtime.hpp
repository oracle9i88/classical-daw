#pragma once

namespace daw {
// Must be called on the main thread. No window or audio device is created.
// This is an offline host lifecycle helper, never an audio callback operation.
void prepareAudioUnitRuntime();
void serviceAudioUnitRuntime(double seconds);
}
