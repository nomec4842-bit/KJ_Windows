#include "core/project_io.h"

#include "core/sequencer.h"
#include "core/tracks.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace
{

std::string escapeJsonString(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                std::ostringstream oss;
                oss << "\\u" << std::hex << std::uppercase << std::setw(4)
                    << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(ch));
                escaped += oss.str();
            }
            else
            {
                escaped += ch;
            }
            break;
        }
    }
    return escaped;
}

std::string trackTypeToString(TrackType type)
{
    switch (type)
    {
    case TrackType::Synth:
        return "Synth";
    case TrackType::Sample:
        return "Sample";
    }
    return "Unknown";
}

std::string synthWaveTypeToString(SynthWaveType type)
{
    switch (type)
    {
    case SynthWaveType::Sine:
        return "Sine";
    case SynthWaveType::Square:
        return "Square";
    case SynthWaveType::Saw:
        return "Saw";
    case SynthWaveType::Triangle:
        return "Triangle";
    }
    return "Wave";
}

std::string formatFloat(float value)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << value;
    return oss.str();
}

} // namespace

bool saveProjectToFile(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    std::filesystem::path targetPath = path;
    if (!targetPath.has_extension() || targetPath.extension() != ".jik")
    {
        targetPath.replace_extension(".jik");
    }

    std::ofstream stream(targetPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return false;
    }

    int bpm = sequencerBPM.load(std::memory_order_relaxed);
    auto tracks = getTracks();

    stream << "{\n";
    stream << "  \"version\": 1,\n";
    stream << "  \"bpm\": " << bpm << ",\n";
    stream << "  \"tracks\": [\n";

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const Track& track = tracks[i];
        TrackType type = trackGetType(track.id);
        SynthWaveType waveType = trackGetSynthWaveType(track.id);
        float volume = trackGetVolume(track.id);
        float pan = trackGetPan(track.id);
        float lowGain = trackGetEqLowGain(track.id);
        float midGain = trackGetEqMidGain(track.id);
        float highGain = trackGetEqHighGain(track.id);
        float formant = trackGetSynthFormant(track.id);
        float feedback = trackGetSynthFeedback(track.id);
        float pitch = trackGetSynthPitch(track.id);
        float pitchRange = trackGetSynthPitchRange(track.id);
        bool hasSample = trackGetSampleBuffer(track.id) != nullptr;
        int stepCount = trackGetStepCount(track.id);

        stream << "    {\n";
        stream << "      \"id\": " << track.id << ",\n";
        stream << "      \"name\": \"" << escapeJsonString(track.name) << "\",\n";
        stream << "      \"type\": \"" << trackTypeToString(type) << "\",\n";
        stream << "      \"waveType\": \"" << synthWaveTypeToString(waveType) << "\",\n";
        stream << "      \"volume\": " << formatFloat(volume) << ",\n";
        stream << "      \"pan\": " << formatFloat(pan) << ",\n";
        stream << "      \"eqLow\": " << formatFloat(lowGain) << ",\n";
        stream << "      \"eqMid\": " << formatFloat(midGain) << ",\n";
        stream << "      \"eqHigh\": " << formatFloat(highGain) << ",\n";
        stream << "      \"formant\": " << formatFloat(formant) << ",\n";
        stream << "      \"feedback\": " << formatFloat(feedback) << ",\n";
        stream << "      \"pitch\": " << formatFloat(pitch) << ",\n";
        stream << "      \"pitchRange\": " << formatFloat(pitchRange) << ",\n";
        stream << "      \"hasSample\": " << (hasSample ? "true" : "false") << ",\n";
        stream << "      \"stepCount\": " << stepCount << ",\n";
        stream << "      \"steps\": [\n";

        for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
        {
            bool enabled = trackGetStepState(track.id, stepIndex);
            auto notes = trackGetStepNotes(track.id, stepIndex);
            float velocity = trackGetStepVelocity(track.id, stepIndex);
            float stepPan = trackGetStepPan(track.id, stepIndex);
            float pitchOffset = trackGetStepPitchOffset(track.id, stepIndex);

            stream << "        {\n";
            stream << "          \"index\": " << stepIndex << ",\n";
            stream << "          \"enabled\": " << (enabled ? "true" : "false") << ",\n";
            stream << "          \"notes\": [";
            for (size_t noteIndex = 0; noteIndex < notes.size(); ++noteIndex)
            {
                if (noteIndex > 0)
                {
                    stream << ", ";
                }
                stream << notes[noteIndex];
            }
            stream << "],\n";
            stream << "          \"velocity\": " << formatFloat(velocity) << ",\n";
            stream << "          \"pan\": " << formatFloat(stepPan) << ",\n";
            stream << "          \"pitchOffset\": " << formatFloat(pitchOffset) << "\n";
            stream << "        }";
            if (stepIndex + 1 < stepCount)
            {
                stream << ",";
            }
            stream << "\n";
        }

        stream << "      ]\n";
        stream << "    }";
        if (i + 1 < tracks.size())
        {
            stream << ",";
        }
        stream << "\n";
    }

    stream << "  ]\n";
    stream << "}\n";

    stream.flush();
    return stream.good();
}

