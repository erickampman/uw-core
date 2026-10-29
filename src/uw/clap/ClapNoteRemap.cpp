#include "uw/clap/ClapNoteRemap.h"

#include <algorithm>

namespace uw {

namespace {

void push(const clap_output_events_t* out, const clap_event_header_t* h) {
    if (out != nullptr && out->try_push != nullptr) out->try_push(out, h);
}

bool validKey(int key) { return key >= 0 && key <= 127; }

// Adds a fraction (1/512 semitone) to a tuning expression, within its ±120 semitone range.
void addTuning(clap_event_note_expression_t& m, int fraction79) {
    if (m.expression_id == CLAP_NOTE_EXPRESSION_TUNING)
        m.value = std::clamp(m.value + fraction79 / 512.0, -120.0, 120.0);
}

// num / den rounded to nearest, halves away from zero (den > 0).
int roundDiv(int num, int den) {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

} // namespace

void ClapNoteRemap::setShifts(const int (&shift79)[128]) noexcept {
    for (int k = 0; k < 128; ++k) {
        keyShift_[k]   = roundDiv(shift79[k], 512);
        fraction79_[k] = shift79[k] - keyShift_[k] * 512;
    }
    uniformFraction79_ = fraction79_[0];
    for (int f : fraction79_)
        if (f != uniformFraction79_) uniformFraction79_ = 0;
}

void ClapNoteRemap::setShift(int shift79) noexcept {
    int all[128];
    for (int& s : all) s = shift79;
    setShifts(all);
}

void ClapNoteRemap::reset() noexcept {
    for (auto& channel : latch_)
        for (auto& e : channel) e = Entry{};
    for (auto& channel : owner_)
        for (auto& o : channel) o = -1;
}

void ClapNoteRemap::process(const clap_event_header_t* h, const clap_output_events_t* out) noexcept {
    switch (h->type) {
        case CLAP_EVENT_NOTE_ON:
            noteOn(*reinterpret_cast<const clap_event_note_t*>(h), out);
            break;
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
            noteEnd(*reinterpret_cast<const clap_event_note_t*>(h), out);
            break;
        case CLAP_EVENT_NOTE_EXPRESSION:
            expression(*reinterpret_cast<const clap_event_note_expression_t*>(h), out);
            break;
        default:
            push(out, h);
            break;
    }
}

template <typename F>
int ClapNoteRemap::forEachMatch(int channel, int key, std::int32_t noteId, F&& f) noexcept {
    if (channel > 15 || key > 127) return 0;
    const int c0 = channel < 0 ? 0 : channel, c1 = channel < 0 ? 15 : channel;
    const int k0 = key < 0 ? 0 : key, k1 = key < 0 ? 127 : key;
    int n = 0;
    for (int c = c0; c <= c1; ++c)
        for (int k = k0; k <= k1; ++k) {
            Entry& e = latch_[c][k];
            if (e.state == Latch::None) continue;
            // With a note_id, the latched note must have the same one; a note the host gave no
            // id to still matches by its key.
            if (noteId >= 0 && e.noteId != noteId && (key < 0 || e.noteId >= 0)) continue;
            f(c, e);
            ++n;
        }
    return n;
}

void ClapNoteRemap::noteOn(const clap_event_note_t& ev, const clap_output_events_t* out) noexcept {
    if (ev.channel < 0 || ev.channel > 15 || !validKey(ev.key)) {
        push(out, &ev.header);   // not a playable note: leave it to the receiver
        return;
    }
    Entry& e = latch_[ev.channel][ev.key];
    const int outKey = ev.key + keyShift_[ev.key];
    const int fraction79 = fraction79_[ev.key];
    const bool inRange = validKey(outKey);
    if (e.state == Latch::Sounding && (!inRange || e.outKey != outKey)) {
        // Retriggered while sounding, and the new note won't land on the old key: end the old
        // one first so it can't hang.
        clap_event_note_t off = ev;
        off.header.type = CLAP_EVENT_NOTE_OFF;
        off.note_id     = e.noteId;
        off.key         = e.outKey;
        off.velocity    = 0;
        push(out, &off.header);
    }
    if (!inRange) {
        e = {Latch::Dropped, 0, 0, ev.note_id};
        return;
    }
    // Another key sounding on this output key: end its note, and drop what follows it. (An owner
    // whose note has since ended, or moved, is stale and ignored.)
    std::int8_t& owner = owner_[ev.channel][outKey];
    if (owner >= 0 && owner != ev.key) {
        Entry& o = latch_[ev.channel][owner];
        if (o.state == Latch::Sounding && o.outKey == outKey) {
            clap_event_note_t off = ev;
            off.header.type = CLAP_EVENT_NOTE_OFF;
            off.note_id     = o.noteId;
            off.key         = o.outKey;
            off.velocity    = 0;
            push(out, &off.header);
            o = {Latch::Dropped, 0, 0, o.noteId};
        }
    }
    owner = static_cast<std::int8_t>(ev.key);
    e = {Latch::Sounding, static_cast<std::uint8_t>(outKey), static_cast<std::int16_t>(fraction79),
         ev.note_id};

    clap_event_note_t on = ev;
    on.key = static_cast<std::int16_t>(outKey);
    push(out, &on.header);
    if (fraction79 != 0) {
        clap_event_note_expression_t tuning{};
        tuning.header = {sizeof(tuning), ev.header.time, CLAP_CORE_EVENT_SPACE_ID,
                         CLAP_EVENT_NOTE_EXPRESSION, ev.header.flags};
        tuning.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
        tuning.note_id       = ev.note_id;
        tuning.port_index    = ev.port_index;
        tuning.channel       = ev.channel;
        tuning.key           = on.key;
        tuning.value         = fraction79 / 512.0;
        push(out, &tuning.header);
    }
}

void ClapNoteRemap::noteEnd(const clap_event_note_t& ev, const clap_output_events_t* out) noexcept {
    if (ev.key < 0 && ev.note_id < 0) {
        forEachMatch(ev.channel, -1, -1, [](int, Entry& e) { e = Entry{}; });
        push(out, &ev.header);
        return;
    }
    const int n = forEachMatch(ev.channel, ev.key, ev.note_id, [&](int channel, Entry& e) {
        if (e.state == Latch::Sounding) {
            clap_event_note_t m = ev;
            m.channel = static_cast<std::int16_t>(channel);
            m.key     = e.outKey;
            push(out, &m.header);
        }
        e = Entry{};
    });
    if (n != 0) return;
    // No Note On seen (it came before a reset, or before this started): guess the key the
    // current shift would have given it.
    clap_event_note_t m = ev;
    if (ev.key >= 0) {
        if (ev.key > 127 || !validKey(ev.key + keyShift_[ev.key])) return;
        m.key = static_cast<std::int16_t>(ev.key + keyShift_[ev.key]);
    }
    push(out, &m.header);
}

void ClapNoteRemap::expression(const clap_event_note_expression_t& ev,
                               const clap_output_events_t* out) noexcept {
    if (ev.key >= 0 || ev.note_id >= 0) {
        const int n = forEachMatch(ev.channel, ev.key, ev.note_id, [&](int channel, Entry& e) {
            if (e.state != Latch::Sounding) return;
            clap_event_note_expression_t m = ev;
            m.channel = static_cast<std::int16_t>(channel);
            m.key     = e.outKey;
            addTuning(m, e.fraction79);
            push(out, &m.header);
        });
        if (n != 0) return;
    }
    // Wildcard, or no latched note: the current shift.
    clap_event_note_expression_t m = ev;
    if (ev.key >= 0) {
        if (ev.key > 127 || !validKey(ev.key + keyShift_[ev.key])) return;
        m.key = static_cast<std::int16_t>(ev.key + keyShift_[ev.key]);
        addTuning(m, fraction79_[ev.key]);
    } else {
        addTuning(m, uniformFraction79_);   // every key: right only if they all share it
    }
    push(out, &m.header);
}

} // namespace uw
