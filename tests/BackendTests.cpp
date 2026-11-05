// uw::audio and uw::midi on this machine, without sound: whether a JACK server
// is reachable (never starting one), and opening the MIDI input's clients.

#include <cstdio>
#include <unistd.h>

#include "Check.h"
#include "uw/audio/JackBackend.h"
#include "uw/midi/AlsaSeqInput.h"

int main() {
    std::printf("backends: JACK server %s\n", uw::JackBackend::serverAvailable() ? "reachable" : "not running");
    if (access("/dev/snd/seq", F_OK) != 0) {   // no snd-seq module (CI runners): skip, don't fail
        std::printf("backends: skipped, no ALSA sequencer (/dev/snd/seq)\n");
        return 77;                              // SKIP_RETURN_CODE in CMakeLists.txt
    }
    {
        const auto input = uw::AlsaSeqInput::create("uw-core tests");
        CHECK(input != nullptr);   // needs /dev/snd/seq (the snd-seq module)
    }
    return uw::test::finish("backends");
}
