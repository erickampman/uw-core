#pragma once

#include <cstdint>
#include <string>

namespace uw {

// Static description of one plugin parameter. Values are in the parameter's
// PLAIN range (minValue..maxValue), not normalized; the VST3 seam converts
// to/from VST3's normalized [0,1] at the boundary (StageHand's docs/architecture.md 3a).
struct ParameterInfo {
    std::uint32_t id     = 0;         // stable id
    void*         cookie = nullptr;   // plugin fast-lookup handle (CLAP); may be null
    std::string   name;
    double        minValue      = 0.0;
    double        maxValue      = 1.0;
    double        defaultValue  = 0.0;
    bool          isAutomatable = true;
    bool          isStepped     = false;
    bool          isHidden      = false;   // not for the user: a host's own controls leave it out
    bool          isReadOnly    = false;   // shown, never set by the host
    std::string   module;                  // where it belongs, as a path ("Osc 1/Filter"); may be empty
};

} // namespace uw
