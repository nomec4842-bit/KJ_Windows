#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

namespace kj {

class VstParameterQueue {
public:
    void push_gui_change(Steinberg::Vst::ParamID id, double normalized_value);
    void apply_to_audio_processor(Steinberg::Vst::ProcessData& data);

private:
    struct ParameterChange
    {
        Steinberg::Vst::ParamID id{};
        Steinberg::Vst::ParamValue value{};
        Steinberg::int32 sampleOffset{0};
    };

    std::atomic<bool> is_dirty_{false};
    std::vector<ParameterChange> change_buffer_;
    std::mutex access_mutex_;
    Steinberg::Vst::ParameterChanges parameter_changes_{};
};

} // namespace kj

