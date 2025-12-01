#include "hosting/VstParameterQueue.h"

namespace kj {

void VstParameterQueue::push_gui_change(Steinberg::Vst::ParamID id, double normalized_value)
{
    std::lock_guard<std::mutex> lock(access_mutex_);

    ParameterChange change{};
    change.id = id;
    change.value = normalized_value;
    change.sampleOffset = 0;

    change_buffer_.push_back(change);
    is_dirty_.store(true, std::memory_order_release);
}

void VstParameterQueue::apply_to_audio_processor(Steinberg::Vst::ProcessData& data)
{
    data.inputParameterChanges = nullptr;

    if (!is_dirty_.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(access_mutex_);

    parameter_changes_.clearQueue();
    parameter_changes_.setMaxParameters(static_cast<Steinberg::int32>(change_buffer_.size()));

    for (const auto& change : change_buffer_)
    {
        Steinberg::int32 index = 0;
        if (auto* queue = parameter_changes_.addParameterData(change.id, index))
        {
            queue->addPoint(change.sampleOffset, change.value, index);
        }
    }

    change_buffer_.clear();
    is_dirty_.store(false, std::memory_order_release);

    if (parameter_changes_.getParameterCount() > 0)
        data.inputParameterChanges = &parameter_changes_;
}

} // namespace kj

