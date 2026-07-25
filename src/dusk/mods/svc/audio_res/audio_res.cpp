#include "audio_res.hpp"

#include "helpers/cast.hpp"
#include "mods/svc/audio_res.h"

#include "../registry.hpp"
#include "../slot_map.hpp"
#include "aurora/lib/logging.hpp"
#include "dusk/mods/loader/loader.hpp"

namespace dusk::mods::svc {

namespace audio_res {
namespace {

using namespace dusk::helpers::cast;

aurora::Module Log("dusk::mods::svc::audio_res");

SlotMap<RuntimeWaveReplacementSlot> s_waveReplacements;

constexpr ContainerLoadFunction s_containerLoadFunctions[] = {
    load_wav,
#if DUSK_OPUS
    load_opus,
#endif
};

bool validate_raw_size(LoadedMod const& mod, std::string const& path, uintptr_t actual_size, AudioRawWave const& raw, u32& sample_count) {
    u32 samples_per_block;
    u32 bytes_per_block;
    switch (raw.format) {
    case Adpcm4:
        samples_per_block = 16;
        bytes_per_block = 9;
        break;
    case Pcm16:
        samples_per_block = 1;
        bytes_per_block = 2;
        break;
    default:
        Log.error("[{}] unimplemented wave format (ADPCM2/PCM8): '{}'", mod.metadata.id, path);
        return false;
    }

    if (actual_size % bytes_per_block != 0) {
        Log.error("[{}] raw file is not divisible by format block size: '{}'", mod.metadata.id, path);
        return false;
    }

    sample_count = (actual_size / bytes_per_block) * samples_per_block;
    return true;
}

ModResult load_raw(
    LoadedMod const& mod, RuntimeWaveReplacementSlot& slot, AudioRawWave const& raw) {
    auto file_contents = mod.bundle->readFile(slot.bundle_path);
    u32 sample_count = 0;

    if (!validate_raw_size(mod, slot.bundle_path, file_contents.size(), raw, sample_count)) {
        Log.error(
            "[{}] replace_wave: file '{}' is a raw .abf file, but raw_wave data was not specified",
            mod.metadata.id, slot.bundle_path);

        return MOD_INVALID_ARGUMENT;
    }

    if (slot.format == Pcm16) {
        SampleDataPcm16 pcm16;
        pcm16.data.resize(sample_count);
        memcpy(pcm16.data.data(), file_contents.data(), file_contents.size());
        slot.data = std::make_shared<SampleDataPcm16>(std::move(pcm16));
    } else {
        slot.data = std::make_shared<SampleDataU8>(std::move(file_contents));
    }

    slot.sample_count = sample_count;
    slot.sample_rate = raw.sample_rate;
    slot.format = raw.format;
    slot.sample_value_penult = raw.sample_value_penult;
    slot.sample_value_last = raw.sample_value_last;
    return MOD_OK;
}

/**
 * Load an audio file stored in some sort of container format.
 * File type is determined by looking at contents, not extension.
 */
ModResult load_container(LoadedMod const& mod, RuntimeWaveReplacementSlot& slot) {
    auto file_contents = mod.bundle->readFile(slot.bundle_path);

    for (auto const loader : s_containerLoadFunctions) {
        auto const result = loader(mod, slot, file_contents);
        if (result == MOD_OK) {
            return MOD_OK;
        }

        if (result != MOD_UNSUPPORTED) {
            return result;
        }
    }

    return MOD_UNSUPPORTED;
}

JASWaveInfo wave_info_from_slot(RuntimeWaveReplacementSlot const& slot) {
    JASWaveInfo info;
    info.mWaveFormat = static_cast<u8>(slot.format);
    info.mBaseKey = slot.base_key;
    info.mLoopFlag = slot.loop ? 0xFF : 0;
    info.mSampleRate = slot.sample_rate;
    info.mOffsetStart = 0; // Unused but just init it ig.
    info.mOffsetLength = bounded_cast(slot.data->size());
    info.mLoopStartSample = slot.loop_start_sample;
    info.mLoopEndSample = bounded_cast(slot.loop_end_sample);
    info.mSampleCount = bounded_cast(slot.sample_count);
    info.mpLast = slot.sample_value_last;
    info.mpPenult = slot.sample_value_penult;
    // mpLoaded is initialized to point to a 1, so we're good there.
    return info;
}

ModResult insert_replace_wave(
    ModContext* ctx,
    AudioWaveBank bank,
    u16 wave_id,
    char const* file_name,
    AudioWaveInfo const* wave_info,
    AudioWaveHandle* out_handle) {
    if (out_handle != nullptr) {
        *out_handle = 0;
    }

    auto mod = mod_from_context(ctx);
    if (mod == nullptr || file_name == nullptr || !is_safe_resource_path(file_name)) {
        return MOD_INVALID_ARGUMENT;
    }

    RuntimeWaveReplacementSlot slot{};
    slot.bundle_path = file_name;
    slot.bank = bank;
    slot.wave_id = wave_id;

    ModResult result;
    if (wave_info && wave_info->raw_wave) {
        result = load_raw(*mod, slot, *wave_info->raw_wave);
    } else {
        result = load_container(*mod, slot);
    }
    if (result != MOD_OK) {
        return result;
    }

    if (wave_info != nullptr) {
        slot.base_key = wave_info->base_key;
        slot.loop = wave_info->loop;
        slot.loop_start_sample = wave_info->loop_start_sample;
        slot.loop_end_sample = wave_info->loop_end_sample;
    } else {
        slot.base_key = AUDIO_RES_DEFAULT_KEY;
        slot.loop = false;
        slot.loop_start_sample = 0;
        slot.loop_end_sample = slot.sample_count;
    }

    // TODO: Handle conflicts

    audio_res::AudioWaveReplacementValue value;
    value.data = slot.data;
    value.wave_info = wave_info_from_slot(slot);
    audio_res::s_replacements.emplace(
        audio_res::AudioWaveKey{.bank = bank, .wave_id = wave_id},
        std::make_unique<AudioWaveReplacementValue>(std::move(value)));

    const auto handle = s_waveReplacements.emplace(*mod, std::move(slot));
    if (out_handle) {
        *out_handle = handle;
    }

    return MOD_OK;
}

constexpr AudioResService s_audioResService{
    .header = SERVICE_HEADER(AudioResService, AUDIO_RES_SERVICE_MAJOR, AUDIO_RES_SERVICE_MINOR),
    .replace_wave = &audio_res::insert_replace_wave};

}

absl::flat_hash_map<AudioWaveKey, std::unique_ptr<AudioWaveReplacementValue>> s_replacements;

AudioWaveReplacementValue::~AudioWaveReplacementValue() = default;
const JASWaveInfo* AudioWaveReplacementValue::getWaveInfo() const {
    return &wave_info;
}

void const* AudioWaveReplacementValue::getAramBaseAddress() const {
    return data->get_data().data();
}

intptr_t AudioWaveReplacementValue::getWavePtr() const {
    return 0;
}

void SampleDataPcm16::be_swap() {
    for (auto& sample : data) {
        ::be_swap(sample);
    }
}

}  // namespace audio_res

constinit const ServiceModule g_audioResModule{
    .id = AUDIO_RES_SERVICE_ID,
    .majorVersion = AUDIO_RES_SERVICE_MAJOR,
    .minorVersion = AUDIO_RES_SERVICE_MINOR,
    .service = &audio_res::s_audioResService,
    // .modDetached = overlay_remove_mod,
    // .lifecycleApplied = overlay_sync_files,
    // .frameEnd =
    //     [] {
    //         if (consume_overlays_dirty()) {
    //             overlay_sync_files();
    //         }
    // },
};

}
