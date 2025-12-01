#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

namespace kj {

class VstParameterQueue {
public:
    void push_gui_change(Steinberg::Vst::ParamID id, double normalized_value);
    void apply_to_audio_processor(Steinberg::Vst::ProcessData& data);

private:
    std::atomic<bool> is_dirty_{false};
    std::vector<Steinberg::Vst::ParameterChange> change_buffer_;
    std::mutex access_mutex_;
    Steinberg::Vst::ParameterChanges parameter_changes_{};
};

} // namespace kj

