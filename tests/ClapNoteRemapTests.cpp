// ClapNoteRemap tests: a shift per key for CLAP note events. Retune's plugin tests cover a single
// shift for every key in depth.

#include <cmath>
#include <cstring>
#include <vector>

#include "Check.h"
#include "uw/clap/ClapNoteRemap.h"

using namespace uw;

namespace {

struct Event {
    std::uint16_t type;
    int           key;
    std::int32_t  noteId;
    double        value;   // a tuning expression's value
};

struct Collector {
    std::vector<Event>   events;
    clap_output_events_t out{this, &Collector::push};

    static bool push(const clap_output_events_t* list, const clap_event_header_t* h) {
        auto* self = static_cast<Collector*>(list->ctx);
        if (h->type == CLAP_EVENT_NOTE_EXPRESSION) {
            const auto* x = reinterpret_cast<const clap_event_note_expression_t*>(h);
            self->events.push_back({h->type, x->key, x->note_id, x->value});
        } else {
            const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
            self->events.push_back({h->type, n->key, n->note_id, 0.0});
        }
        return true;
    }
};

clap_event_note_t note(std::uint16_t type, int key, std::int32_t noteId) {
    clap_event_note_t ev{};
    ev.header   = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    ev.note_id  = noteId;
    ev.channel  = 0;
    ev.key      = static_cast<std::int16_t>(key);
    ev.velocity = 0.8;
    return ev;
}

clap_event_note_expression_t tuning(int key, std::int32_t noteId, double value) {
    clap_event_note_expression_t ev{};
    ev.header        = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
    ev.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
    ev.note_id       = noteId;
    ev.channel       = 0;
    ev.key           = static_cast<std::int16_t>(key);
    ev.value         = value;
    return ev;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void testPerKeyShifts() {
    ClapNoteRemap r;
    int shifts[128] = {};
    shifts[60] = 512 + 128;   // up 1.25 semitones: key 61, tuning +0.25
    shifts[62] = 512 + 384;   // up 1.75: key 64 (rounded), tuning -0.25
    shifts[64] = -512;        // down 1, no tuning
    r.setShifts(shifts);

    Collector c;
    auto on = note(CLAP_EVENT_NOTE_ON, 60, 1);
    r.process(&on.header, &c.out);
    CHECK(c.events.size() == 2 && c.events[0].key == 61 && c.events[1].type == CLAP_EVENT_NOTE_EXPRESSION &&
          c.events[1].key == 61 && c.events[1].noteId == 1 && near(c.events[1].value, 0.25));

    c.events.clear();
    on = note(CLAP_EVENT_NOTE_ON, 62, 2);
    r.process(&on.header, &c.out);
    CHECK(c.events.size() == 2 && c.events[0].key == 64 && near(c.events[1].value, -0.25));

    c.events.clear();
    on = note(CLAP_EVENT_NOTE_ON, 64, 3);
    r.process(&on.header, &c.out);
    CHECK(c.events.size() == 1 && c.events[0].key == 63);
}

void testLatchAndHostTuning() {
    ClapNoteRemap r;
    int shifts[128] = {};
    shifts[60] = 512 + 128;
    r.setShifts(shifts);
    Collector c;
    auto on = note(CLAP_EVENT_NOTE_ON, 60, 7);
    r.process(&on.header, &c.out);

    // New shifts don't move the sounding note.
    shifts[60] = 0;
    r.setShifts(shifts);
    c.events.clear();
    auto t = tuning(60, 7, 0.5);
    r.process(&t.header, &c.out);
    CHECK(c.events.size() == 1 && c.events[0].key == 61 && near(c.events[0].value, 0.75));

    c.events.clear();
    auto off = note(CLAP_EVENT_NOTE_OFF, 60, 7);
    r.process(&off.header, &c.out);
    CHECK(c.events.size() == 1 && c.events[0].type == CLAP_EVENT_NOTE_OFF && c.events[0].key == 61);

    c.events.clear();
    on = note(CLAP_EVENT_NOTE_ON, 60, 8);
    r.process(&on.header, &c.out);
    CHECK(c.events.size() == 1 && c.events[0].key == 60);
}

void testWildcardTuning() {
    ClapNoteRemap r;
    Collector c;
    r.setShift(512 + 128);   // every key shares +0.25
    auto t = tuning(-1, -1, 1.0);
    r.process(&t.header, &c.out);
    CHECK(c.events.size() == 1 && near(c.events[0].value, 1.25));

    int shifts[128] = {};
    shifts[60] = 128;        // keys differ: a wildcard can't carry one key's fraction
    r.setShifts(shifts);
    c.events.clear();
    r.process(&t.header, &c.out);
    CHECK(c.events.size() == 1 && near(c.events[0].value, 1.0));
}

void testOutOfRangeDropped() {
    ClapNoteRemap r;
    int shifts[128] = {};
    shifts[125] = 512 * 5;
    r.setShifts(shifts);
    Collector c;
    auto on = note(CLAP_EVENT_NOTE_ON, 125, 4);
    r.process(&on.header, &c.out);
    auto off = note(CLAP_EVENT_NOTE_OFF, 125, 4);
    r.process(&off.header, &c.out);
    CHECK(c.events.empty());
}

void testSharedOutputKey() {
    ClapNoteRemap r;
    int shifts[128] = {};
    shifts[61] = -512;   // C#4 lands on C4
    r.setShifts(shifts);
    Collector c;
    auto on = note(CLAP_EVENT_NOTE_ON, 60, 1);
    r.process(&on.header, &c.out);
    c.events.clear();
    on = note(CLAP_EVENT_NOTE_ON, 61, 2);
    r.process(&on.header, &c.out);
    CHECK(c.events.size() == 2 && c.events[0].type == CLAP_EVENT_NOTE_OFF && c.events[0].key == 60 &&
          c.events[0].noteId == 1 && c.events[1].type == CLAP_EVENT_NOTE_ON && c.events[1].noteId == 2);

    c.events.clear();
    auto off = note(CLAP_EVENT_NOTE_OFF, 60, 1);
    r.process(&off.header, &c.out);
    CHECK(c.events.empty());
    off = note(CLAP_EVENT_NOTE_OFF, 61, 2);
    r.process(&off.header, &c.out);
    CHECK(c.events.size() == 1 && c.events[0].key == 60 && c.events[0].noteId == 2);
}

} // namespace

int main() {
    testPerKeyShifts();
    testLatchAndHostTuning();
    testWildcardTuning();
    testOutOfRangeDropped();
    testSharedOutputKey();
    return uw::test::finish("clap-note-remap");
}
