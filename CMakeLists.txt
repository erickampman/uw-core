cmake_minimum_required(VERSION 3.21)

project(uw-core VERSION 0.1.0 LANGUAGES CXX)

# uw-core: code shared by StageHand, Umpire and (uw::ump only) the plugins.
# Each user adds it as a submodule and calls add_subdirectory(); it builds only
# static libraries. Layers (README.md):
#   uw::ump        UMP messages and helpers; no dependencies, C++17, PIC
#   uw::base       clock, SPSC queue, denormals, audio buffers, events
#   uw::plugin     the plugin seam (interfaces only)
#   uw::clap-host  the plugin seam's CLAP implementation
#   uw::audio      JACK (Linux) / CoreAudio (macOS)
#   uw::midi       ALSA sequencer UMP (Linux) / CoreMIDI (macOS) input and output

option(UW_CORE_UMP_ONLY "Build only uw::ump (for plugins: no CLAP, JACK or ALSA needed)" OFF)
option(UW_CORE_BACKENDS "Build uw::audio and uw::midi (Linux: needs JACK and ALSA)" ON)
option(UW_CORE_TESTS "Build uw-core's tests" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Symbols stay hidden: linked into a plugin (a shared object), uw-core's code
# mustn't bind to another plugin's copy in the same host.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(uw-warnings INTERFACE)
target_compile_options(uw-warnings INTERFACE -Wall -Wextra -Wpedantic)

# --- uw::ump: no dependencies, usable from C++17 plugins ------------------------
add_library(uw-ump STATIC src/uw/ump/Ump.cpp)
add_library(uw::ump ALIAS uw-ump)
target_include_directories(uw-ump PUBLIC src)
target_compile_features(uw-ump PUBLIC cxx_std_17)
set_target_properties(uw-ump PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(uw-ump PRIVATE uw-warnings)

if(NOT UW_CORE_UMP_ONLY)
    find_package(Threads REQUIRED)

    # --- uw::base ----------------------------------------------------------------
    add_library(uw-base STATIC src/uw/base/Clock.cpp)
    add_library(uw::base ALIAS uw-base)
    target_compile_features(uw-base PUBLIC cxx_std_20)
    target_link_libraries(uw-base PUBLIC uw-ump PRIVATE uw-warnings)

    # --- uw::plugin and uw::clap-host --------------------------------------------
    add_library(uw-plugin INTERFACE)
    add_library(uw::plugin ALIAS uw-plugin)
    target_link_libraries(uw-plugin INTERFACE uw-base)

    # The CLAP SDK (git submodule, pinned to 1.2.10, as StageHand's). Its
    # headers are third-party: no warnings from them in our code.
    add_library(uw-clap-sdk INTERFACE)
    target_include_directories(uw-clap-sdk SYSTEM INTERFACE external/clap/include)

    add_library(uw-clap-host STATIC
        src/uw/clap/ClapModule.cpp
        src/uw/clap/ClapPlugin.cpp
        src/uw/clap/ClapPluginFactory.cpp)
    add_library(uw::clap-host ALIAS uw-clap-host)
    target_link_libraries(uw-clap-host
        PUBLIC  uw-plugin uw-clap-sdk Threads::Threads ${CMAKE_DL_LIBS}
        PRIVATE uw-warnings)
    if(APPLE)
        target_link_libraries(uw-clap-host PUBLIC "-framework CoreFoundation")   # CFBundle loads .clap bundles
    endif()
endif()

if(NOT UW_CORE_UMP_ONLY AND UW_CORE_BACKENDS)
    # --- uw::audio and uw::midi: the platform backends ----------------------------
    add_library(uw-audio STATIC)
    add_library(uw::audio ALIAS uw-audio)
    add_library(uw-midi STATIC)
    add_library(uw::midi ALIAS uw-midi)
    if(APPLE)
        target_sources(uw-audio PRIVATE src/uw/audio/CoreAudioBackend.cpp)
        target_link_libraries(uw-audio PUBLIC "-framework CoreAudio" "-framework CoreFoundation")
        target_sources(uw-midi PRIVATE src/uw/midi/CoreMidiInput.cpp src/uw/midi/CoreMidiOutput.cpp)
        target_link_libraries(uw-midi PUBLIC "-framework CoreMIDI" "-framework CoreFoundation")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(UW_JACK REQUIRED IMPORTED_TARGET jack)
        pkg_check_modules(UW_ALSA REQUIRED IMPORTED_TARGET alsa>=1.2.10)   # UMP sequencer API
        target_sources(uw-audio PRIVATE src/uw/audio/JackBackend.cpp)
        target_link_libraries(uw-audio PUBLIC PkgConfig::UW_JACK)
        target_sources(uw-midi PRIVATE src/uw/midi/AlsaSeqInput.cpp src/uw/midi/AlsaSeqOutput.cpp)
        target_link_libraries(uw-midi PUBLIC PkgConfig::UW_ALSA)
    endif()
    foreach(backend uw-audio uw-midi)
        target_link_libraries(${backend} PUBLIC uw-base Threads::Threads PRIVATE uw-warnings)
    endforeach()
endif()

# --- Tests (tests/Check.h; one executable per suite, no third-party framework) ---
if(UW_CORE_TESTS)
    enable_testing()
    function(uw_add_test NAME SOURCE LIBRARY)
        add_executable(${NAME} ${SOURCE})
        target_include_directories(${NAME} PRIVATE tests)
        target_link_libraries(${NAME} PRIVATE ${LIBRARY} uw-warnings)
        add_test(NAME ${NAME} COMMAND ${NAME})
    endfunction()
    uw_add_test(test-ump tests/UmpTests.cpp uw-ump)
    if(NOT UW_CORE_UMP_ONLY)
        uw_add_test(test-spsc      tests/SpscQueueTests.cpp uw-base)
        uw_add_test(test-clock     tests/ClockTests.cpp     uw-base)
        uw_add_test(test-denormals tests/DenormalTests.cpp  uw-base)
        uw_add_test(test-clap-host tests/ClapHostTests.cpp uw-clap-host)
        if(UW_CORE_BACKENDS AND NOT APPLE)
            uw_add_test(test-backends tests/BackendTests.cpp "uw-audio;uw-midi")
            uw_add_test(test-alsa-output tests/AlsaSeqOutputTests.cpp uw-midi)
        endif()
        if(UW_CORE_BACKENDS AND APPLE)
            uw_add_test(test-coremidi-output tests/CoreMidiOutputTests.cpp uw-midi)
        endif()
        target_link_libraries(test-spsc PRIVATE Threads::Threads)
        target_link_libraries(test-clock PRIVATE Threads::Threads)
    endif()
endif()
