#include "core/project_io.h"

#include "core/mod_matrix.h"
#include "core/mod_matrix_parameters.h"
#include "core/sequencer.h"
#include "core/tracks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <codecvt>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <variant>
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

std::wstring utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};

    try
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.from_bytes(value);
    }
    catch (...)
    {
        return std::wstring(value.begin(), value.end());
    }
}

std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return {};

    try
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.to_bytes(value);
    }
    catch (...)
    {
        return std::string(value.begin(), value.end());
    }
}

std::string trackTypeToString(TrackType type)
{
    switch (type)
    {
    case TrackType::Synth:
        return "Synth";
    case TrackType::Sample:
        return "Sample";
    case TrackType::MidiOut:
        return "MIDI Out";
    case TrackType::VST:
        return "VST";
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

TrackType trackTypeFromString(const std::string& value)
{
    if (value == "Sample")
        return TrackType::Sample;
    if (value == "VST")
        return TrackType::VST;
    if (value == "MIDI Out" || value == "MidiOut" || value == "MIDI")
        return TrackType::MidiOut;
    return TrackType::Synth;
}

SynthWaveType synthWaveTypeFromString(const std::string& value)
{
    if (value == "Square")
        return SynthWaveType::Square;
    if (value == "Saw")
        return SynthWaveType::Saw;
    if (value == "Triangle")
        return SynthWaveType::Triangle;
    return SynthWaveType::Sine;
}

struct JsonValue
{
    using object_t = std::map<std::string, JsonValue>;
    using array_t = std::vector<JsonValue>;
    using value_t = std::variant<std::nullptr_t, bool, double, std::string, array_t, object_t>;

    value_t data;

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool() const { return std::holds_alternative<bool>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }
    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isArray() const { return std::holds_alternative<array_t>(data); }
    bool isObject() const { return std::holds_alternative<object_t>(data); }

    bool asBool(bool defaultValue = false) const
    {
        return isBool() ? std::get<bool>(data) : defaultValue;
    }

    double asNumber(double defaultValue = 0.0) const
    {
        return isNumber() ? std::get<double>(data) : defaultValue;
    }

    const std::string& asString() const
    {
        static const std::string kEmpty;
        return isString() ? std::get<std::string>(data) : kEmpty;
    }

    const array_t& asArray() const
    {
        static const array_t kEmpty;
        return isArray() ? std::get<array_t>(data) : kEmpty;
    }

    const object_t& asObject() const
    {
        static const object_t kEmpty;
        return isObject() ? std::get<object_t>(data) : kEmpty;
    }
};

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

void appendUtf8(std::string& target, unsigned int codepoint)
{
    if (codepoint <= 0x7F)
    {
        target.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        target.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
        target.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0x10FFFF)
    {
        target.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        target.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        target.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        target.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else
    {
        target.push_back('?');
    }
}

class JsonParser
{
public:
    explicit JsonParser(const std::string& input)
        : mInput(input)
    {
    }

    bool parse(JsonValue& out)
    {
        mPosition = 0;
        skipWhitespace();
        if (!parseValue(out))
            return false;
        skipWhitespace();
        return mPosition == mInput.size();
    }

private:
    const std::string& mInput;
    size_t mPosition = 0;

    char peek() const
    {
        if (mPosition >= mInput.size())
            return '\0';
        return mInput[mPosition];
    }

    char get()
    {
        if (mPosition >= mInput.size())
            return '\0';
        return mInput[mPosition++];
    }

    void skipWhitespace()
    {
        while (mPosition < mInput.size() && std::isspace(static_cast<unsigned char>(mInput[mPosition])))
        {
            ++mPosition;
        }
    }

    bool consume(char expected)
    {
        if (peek() != expected)
            return false;
        ++mPosition;
        return true;
    }

    bool parseValue(JsonValue& out)
    {
        skipWhitespace();
        char ch = peek();
        if (ch == '"')
            return parseString(out);
        if (ch == '{')
            return parseObject(out);
        if (ch == '[')
            return parseArray(out);
        if (ch == 't' || ch == 'f')
            return parseBool(out);
        if (ch == 'n')
            return parseNull(out);
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)))
            return parseNumber(out);
        return false;
    }

    bool parseString(JsonValue& out)
    {
        std::string result;
        if (!consume('"'))
            return false;
        while (true)
        {
            if (mPosition >= mInput.size())
                return false;
            char ch = get();
            if (ch == '"')
                break;
            if (ch == '\\')
            {
                if (mPosition >= mInput.size())
                    return false;
                char esc = get();
                switch (esc)
                {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u':
                {
                    unsigned int codepoint = 0;
                    if (!parseUnicodeEscape(codepoint))
                        return false;
                    appendUtf8(result, codepoint);
                    break;
                }
                default:
                    return false;
                }
            }
            else
            {
                result.push_back(ch);
            }
        }
        out.data = std::move(result);
        return true;
    }

    bool parseUnicodeEscape(unsigned int& codepoint)
    {
        if (!parseHexDigits(codepoint))
            return false;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
        {
            size_t saved = mPosition;
            if (peek() == '\\')
            {
                ++mPosition;
                if (peek() == 'u')
                {
                    ++mPosition;
                    unsigned int low = 0;
                    if (!parseHexDigits(low))
                        return false;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        return true;
                    }
                }
            }
            mPosition = saved;
            codepoint = 0xFFFD;
            return true;
        }
        if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
        {
            codepoint = 0xFFFD;
        }
        return true;
    }

    bool parseHexDigits(unsigned int& value)
    {
        if (mPosition + 4 > mInput.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            int digit = hexDigit(mInput[mPosition++]);
            if (digit < 0)
                return false;
            value = (value << 4) | static_cast<unsigned int>(digit);
        }
        return true;
    }

    bool parseObject(JsonValue& out)
    {
        if (!consume('{'))
            return false;
        JsonValue::object_t object;
        skipWhitespace();
        if (peek() == '}')
        {
            ++mPosition;
            out.data = std::move(object);
            return true;
        }
        while (true)
        {
            JsonValue keyValue;
            if (!parseString(keyValue))
                return false;
            std::string key = std::get<std::string>(keyValue.data);
            skipWhitespace();
            if (!consume(':'))
                return false;
            skipWhitespace();
            JsonValue value;
            if (!parseValue(value))
                return false;
            object.emplace(std::move(key), std::move(value));
            skipWhitespace();
            if (consume('}'))
            {
                out.data = std::move(object);
                return true;
            }
            if (!consume(','))
                return false;
            skipWhitespace();
        }
    }

    bool parseArray(JsonValue& out)
    {
        if (!consume('['))
            return false;
        JsonValue::array_t array;
        skipWhitespace();
        if (peek() == ']')
        {
            ++mPosition;
            out.data = std::move(array);
            return true;
        }
        while (true)
        {
            JsonValue element;
            if (!parseValue(element))
                return false;
            array.push_back(std::move(element));
            skipWhitespace();
            if (consume(']'))
            {
                out.data = std::move(array);
                return true;
            }
            if (!consume(','))
                return false;
            skipWhitespace();
        }
    }

    bool parseBool(JsonValue& out)
    {
        if (mInput.compare(mPosition, 4, "true") == 0)
        {
            mPosition += 4;
            out.data = true;
            return true;
        }
        if (mInput.compare(mPosition, 5, "false") == 0)
        {
            mPosition += 5;
            out.data = false;
            return true;
        }
        return false;
    }

    bool parseNull(JsonValue& out)
    {
        if (mInput.compare(mPosition, 4, "null") != 0)
            return false;
        mPosition += 4;
        out.data = nullptr;
        return true;
    }

    bool parseNumber(JsonValue& out)
    {
        size_t start = mPosition;
        if (peek() == '-')
            ++mPosition;
        if (!std::isdigit(static_cast<unsigned char>(peek())))
            return false;
        if (peek() == '0')
        {
            ++mPosition;
        }
        else
        {
            while (std::isdigit(static_cast<unsigned char>(peek())))
                ++mPosition;
        }
        if (peek() == '.')
        {
            ++mPosition;
            if (!std::isdigit(static_cast<unsigned char>(peek())))
                return false;
            while (std::isdigit(static_cast<unsigned char>(peek())))
                ++mPosition;
        }
        if (peek() == 'e' || peek() == 'E')
        {
            ++mPosition;
            if (peek() == '+' || peek() == '-')
                ++mPosition;
            if (!std::isdigit(static_cast<unsigned char>(peek())))
                return false;
            while (std::isdigit(static_cast<unsigned char>(peek())))
                ++mPosition;
        }
        std::string number = mInput.substr(start, mPosition - start);
        try
        {
            double value = std::stod(number);
            out.data = value;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
};

const JsonValue* findMember(const JsonValue::object_t& object, const std::string& key)
{
    auto it = object.find(key);
    if (it == object.end())
        return nullptr;
    return &it->second;
}

int jsonToInt(const JsonValue* value, int defaultValue)
{
    if (!value)
        return defaultValue;
    if (value->isNumber())
        return static_cast<int>(std::lround(value->asNumber()));
    if (value->isString())
    {
        try
        {
            return std::stoi(value->asString());
        }
        catch (...)
        {
        }
    }
    return defaultValue;
}

float jsonToFloat(const JsonValue* value, float defaultValue)
{
    if (!value)
        return defaultValue;
    if (value->isNumber())
        return static_cast<float>(value->asNumber());
    if (value->isString())
    {
        try
        {
            return std::stof(value->asString());
        }
        catch (...)
        {
        }
    }
    return defaultValue;
}

bool jsonToBool(const JsonValue* value, bool defaultValue)
{
    if (!value)
        return defaultValue;
    if (value->isBool())
        return value->asBool();
    if (value->isNumber())
        return value->asNumber() != 0.0;
    if (value->isString())
    {
        const std::string& text = value->asString();
        if (text == "true" || text == "1")
            return true;
        if (text == "false" || text == "0")
            return false;
    }
    return defaultValue;
}

std::string jsonToString(const JsonValue* value)
{
    if (!value)
        return {};
    if (value->isString())
        return value->asString();
    if (value->isNumber())
    {
        std::ostringstream oss;
        oss << value->asNumber();
        return oss.str();
    }
    if (value->isBool())
        return value->asBool() ? "true" : "false";
    return {};
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
    auto assignments = modMatrixGetAssignments();

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
        bool eqEnabled = trackGetEqEnabled(track.id);
        bool delayEnabled = trackGetDelayEnabled(track.id);
        float delayTimeMs = trackGetDelayTimeMs(track.id);
        float delayFeedback = trackGetDelayFeedback(track.id);
        float delayMix = trackGetDelayMix(track.id);
        bool compressorEnabled = trackGetCompressorEnabled(track.id);
        float compressorThreshold = trackGetCompressorThresholdDb(track.id);
        float compressorRatio = trackGetCompressorRatio(track.id);
        float compressorAttack = trackGetCompressorAttack(track.id);
        float compressorRelease = trackGetCompressorRelease(track.id);
        float formant = trackGetSynthFormant(track.id);
        float resonance = trackGetSynthResonance(track.id);
        float feedback = trackGetSynthFeedback(track.id);
        float pitch = trackGetSynthPitch(track.id);
        float pitchRange = trackGetSynthPitchRange(track.id);
        float synthAttack = trackGetSynthAttack(track.id);
        float synthDecay = trackGetSynthDecay(track.id);
        float synthSustain = trackGetSynthSustain(track.id);
        float synthRelease = trackGetSynthRelease(track.id);
        bool synthPhaseSync = trackGetSynthPhaseSync(track.id);
        float sampleAttack = trackGetSampleAttack(track.id);
        float sampleRelease = trackGetSampleRelease(track.id);
        int midiChannel = trackGetMidiChannel(track.id);
        int midiPort = trackGetMidiPort(track.id);
        std::wstring midiPortName = trackGetMidiPortName(track.id);
        std::string midiPortNameUtf8 = wideToUtf8(midiPortName);
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
        stream << "      \"eqEnabled\": " << (eqEnabled ? "true" : "false") << ",\n";
        stream << "      \"delayEnabled\": " << (delayEnabled ? "true" : "false") << ",\n";
        stream << "      \"delayTimeMs\": " << formatFloat(delayTimeMs) << ",\n";
        stream << "      \"delayFeedback\": " << formatFloat(delayFeedback) << ",\n";
        stream << "      \"delayMix\": " << formatFloat(delayMix) << ",\n";
        stream << "      \"compressorEnabled\": " << (compressorEnabled ? "true" : "false") << ",\n";
        stream << "      \"compressorThresholdDb\": " << formatFloat(compressorThreshold) << ",\n";
        stream << "      \"compressorRatio\": " << formatFloat(compressorRatio) << ",\n";
        stream << "      \"compressorAttack\": " << formatFloat(compressorAttack) << ",\n";
        stream << "      \"compressorRelease\": " << formatFloat(compressorRelease) << ",\n";
        stream << "      \"formant\": " << formatFloat(formant) << ",\n";
        stream << "      \"resonance\": " << formatFloat(resonance) << ",\n";
        stream << "      \"feedback\": " << formatFloat(feedback) << ",\n";
        stream << "      \"pitch\": " << formatFloat(pitch) << ",\n";
        stream << "      \"pitchRange\": " << formatFloat(pitchRange) << ",\n";
        stream << "      \"synthAttack\": " << formatFloat(synthAttack) << ",\n";
        stream << "      \"synthDecay\": " << formatFloat(synthDecay) << ",\n";
        stream << "      \"synthSustain\": " << formatFloat(synthSustain) << ",\n";
        stream << "      \"synthRelease\": " << formatFloat(synthRelease) << ",\n";
        stream << "      \"phaseSync\": " << (synthPhaseSync ? "true" : "false") << ",\n";
        stream << "      \"sampleAttack\": " << formatFloat(sampleAttack) << ",\n";
        stream << "      \"sampleRelease\": " << formatFloat(sampleRelease) << ",\n";
        stream << "      \"lfos\": [\n";
        for (size_t lfoIndex = 0; lfoIndex < track.lfoSettings.size(); ++lfoIndex)
        {
            float rate = trackGetLfoRate(track.id, static_cast<int>(lfoIndex));
            LfoShape shape = trackGetLfoShape(track.id, static_cast<int>(lfoIndex));
            float deform = trackGetLfoDeform(track.id, static_cast<int>(lfoIndex));

            stream << "        {\n";
            stream << "          \"index\": " << lfoIndex << ",\n";
            stream << "          \"rateHz\": " << formatFloat(rate) << ",\n";
            stream << "          \"shape\": \"" << lfoShapeToString(shape) << "\",\n";
            stream << "          \"deform\": " << formatFloat(deform) << "\n";
            stream << "        }" << (lfoIndex + 1 < track.lfoSettings.size() ? ",\n" : "\n");
        }
        stream << "      ],\n";
        stream << "      \"midiChannel\": " << midiChannel << ",\n";
        stream << "      \"midiPort\": " << midiPort << ",\n";
        stream << "      \"midiPortName\": \"" << escapeJsonString(midiPortNameUtf8) << "\",\n";
        stream << "      \"hasSample\": " << (hasSample ? "true" : "false") << ",\n";
        stream << "      \"stepCount\": " << stepCount << ",\n";
        stream << "      \"steps\": [\n";

        for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
        {
            bool enabled = trackGetStepState(track.id, stepIndex);
            auto notes = trackGetStepNotes(track.id, stepIndex);
            float velocity = trackGetStepVelocity(track.id, stepIndex);
            std::vector<float> noteVelocities;
            noteVelocities.reserve(notes.size());
            std::vector<bool> noteSustain;
            noteSustain.reserve(notes.size());
            for (int note : notes)
            {
                noteVelocities.push_back(trackGetStepNoteVelocity(track.id, stepIndex, note));
                noteSustain.push_back(trackGetStepNoteSustain(track.id, stepIndex, note));
            }
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
            stream << "          \"noteVelocities\": [";
            for (size_t noteIndex = 0; noteIndex < noteVelocities.size(); ++noteIndex)
            {
                if (noteIndex > 0)
                {
                    stream << ", ";
                }
                stream << formatFloat(noteVelocities[noteIndex]);
            }
            stream << "],\n";
            stream << "          \"noteSustain\": [";
            for (size_t sustainIndex = 0; sustainIndex < noteSustain.size(); ++sustainIndex)
            {
                if (sustainIndex > 0)
                {
                    stream << ", ";
                }
                stream << (noteSustain[sustainIndex] ? "true" : "false");
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

    stream << "  ],\n";
    stream << "  \"modMatrix\": [\n";
    for (size_t i = 0; i < assignments.size(); ++i)
    {
        const auto& assignment = assignments[i];
        stream << "    {\n";
        stream << "      \"id\": " << assignment.id << ",\n";
        stream << "      \"source\": " << assignment.sourceIndex << ",\n";
        stream << "      \"trackId\": " << assignment.trackId << ",\n";
        stream << "      \"parameter\": " << assignment.parameterIndex << ",\n";
        stream << "      \"amount\": " << formatFloat(assignment.normalizedAmount) << "\n";
        stream << "    }";
        if (i + 1 < assignments.size())
            stream << ",";
        stream << "\n";
    }
    stream << "  ]\n";
    stream << "}\n";

    stream.flush();
    return stream.good();
}

bool loadProjectFromFile(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string contents = buffer.str();

    JsonParser parser(contents);
    JsonValue root;
    if (!parser.parse(root) || !root.isObject())
    {
        return false;
    }

    const auto& rootObject = root.asObject();
    const JsonValue* tracksValue = findMember(rootObject, "tracks");
    if (!tracksValue || !tracksValue->isArray())
    {
        return false;
    }

    int bpmValue = jsonToInt(findMember(rootObject, "bpm"), 120);
    int version = jsonToInt(findMember(rootObject, "version"), 1);
    (void)version; // Future compatibility.

    const auto& tracksArray = tracksValue->asArray();

    initTracks();
    modMatrixClearAssignments();

    std::vector<int> trackIds;
    auto currentTracks = getTracks();
    if (!tracksArray.empty())
    {
        if (currentTracks.empty())
        {
            trackIds.push_back(addTrack().id);
        }
        else
        {
            trackIds.push_back(currentTracks.front().id);
        }

        for (size_t i = 1; i < tracksArray.size(); ++i)
        {
            trackIds.push_back(addTrack().id);
        }
    }

    for (size_t i = 0; i < tracksArray.size() && i < trackIds.size(); ++i)
    {
        const JsonValue& trackValue = tracksArray[i];
        if (!trackValue.isObject())
            continue;

        int trackId = trackIds[i];
        const auto& trackObject = trackValue.asObject();

        trackSetName(trackId, jsonToString(findMember(trackObject, "name")));
        trackSetType(trackId, trackTypeFromString(jsonToString(findMember(trackObject, "type"))));
        trackSetSynthWaveType(trackId, synthWaveTypeFromString(jsonToString(findMember(trackObject, "waveType"))));
        trackSetVolume(trackId, jsonToFloat(findMember(trackObject, "volume"), trackGetVolume(trackId)));
        trackSetPan(trackId, jsonToFloat(findMember(trackObject, "pan"), trackGetPan(trackId)));
        trackSetEqLowGain(trackId, jsonToFloat(findMember(trackObject, "eqLow"), trackGetEqLowGain(trackId)));
        trackSetEqMidGain(trackId, jsonToFloat(findMember(trackObject, "eqMid"), trackGetEqMidGain(trackId)));
        trackSetEqHighGain(trackId, jsonToFloat(findMember(trackObject, "eqHigh"), trackGetEqHighGain(trackId)));
        trackSetEqEnabled(trackId, jsonToBool(findMember(trackObject, "eqEnabled"), trackGetEqEnabled(trackId)));
        trackSetSynthFormant(trackId, jsonToFloat(findMember(trackObject, "formant"), trackGetSynthFormant(trackId)));
        trackSetSynthResonance(trackId,
                               jsonToFloat(findMember(trackObject, "resonance"), trackGetSynthResonance(trackId)));
        trackSetSynthFeedback(trackId, jsonToFloat(findMember(trackObject, "feedback"), trackGetSynthFeedback(trackId)));
        trackSetSynthPitch(trackId, jsonToFloat(findMember(trackObject, "pitch"), trackGetSynthPitch(trackId)));
        trackSetSynthPitchRange(trackId, jsonToFloat(findMember(trackObject, "pitchRange"), trackGetSynthPitchRange(trackId)));
        trackSetSynthAttack(trackId, jsonToFloat(findMember(trackObject, "synthAttack"), trackGetSynthAttack(trackId)));
        trackSetSynthDecay(trackId, jsonToFloat(findMember(trackObject, "synthDecay"), trackGetSynthDecay(trackId)));
        trackSetSynthSustain(trackId, jsonToFloat(findMember(trackObject, "synthSustain"), trackGetSynthSustain(trackId)));
        trackSetSynthRelease(trackId, jsonToFloat(findMember(trackObject, "synthRelease"), trackGetSynthRelease(trackId)));
        trackSetSynthPhaseSync(trackId, jsonToBool(findMember(trackObject, "phaseSync"), trackGetSynthPhaseSync(trackId)));
        trackSetSampleAttack(trackId, jsonToFloat(findMember(trackObject, "sampleAttack"), trackGetSampleAttack(trackId)));
        trackSetSampleRelease(trackId, jsonToFloat(findMember(trackObject, "sampleRelease"), trackGetSampleRelease(trackId)));
        const JsonValue* lfosValue = findMember(trackObject, "lfos");
        if (lfosValue && lfosValue->isArray())
        {
            for (const auto& lfoValue : lfosValue->asArray())
            {
                if (!lfoValue.isObject())
                    continue;

                const auto& lfoObject = lfoValue.asObject();
                int index = jsonToInt(findMember(lfoObject, "index"), -1);
                if (index < 0)
                    continue;

                float rateHz = jsonToFloat(findMember(lfoObject, "rateHz"), trackGetLfoRate(trackId, index));
                trackSetLfoRate(trackId, index, rateHz);
                LfoShape shape = lfoShapeFromString(jsonToString(findMember(lfoObject, "shape")));
                trackSetLfoShape(trackId, index, shape);
                float deform = jsonToFloat(findMember(lfoObject, "deform"), trackGetLfoDeform(trackId, index));
                trackSetLfoDeform(trackId, index, deform);
            }
        }
        trackSetDelayEnabled(trackId, jsonToBool(findMember(trackObject, "delayEnabled"), trackGetDelayEnabled(trackId)));
        trackSetDelayTimeMs(trackId, jsonToFloat(findMember(trackObject, "delayTimeMs"), trackGetDelayTimeMs(trackId)));
        trackSetDelayFeedback(trackId, jsonToFloat(findMember(trackObject, "delayFeedback"), trackGetDelayFeedback(trackId)));
        trackSetDelayMix(trackId, jsonToFloat(findMember(trackObject, "delayMix"), trackGetDelayMix(trackId)));
        trackSetCompressorEnabled(trackId,
                                  jsonToBool(findMember(trackObject, "compressorEnabled"),
                                             trackGetCompressorEnabled(trackId)));
        trackSetCompressorThresholdDb(trackId,
                                      jsonToFloat(findMember(trackObject, "compressorThresholdDb"),
                                                  trackGetCompressorThresholdDb(trackId)));
        trackSetCompressorRatio(trackId,
                                 jsonToFloat(findMember(trackObject, "compressorRatio"),
                                             trackGetCompressorRatio(trackId)));
        trackSetCompressorAttack(trackId,
                                  jsonToFloat(findMember(trackObject, "compressorAttack"),
                                              trackGetCompressorAttack(trackId)));
        trackSetCompressorRelease(trackId,
                                   jsonToFloat(findMember(trackObject, "compressorRelease"),
                                               trackGetCompressorRelease(trackId)));
        trackSetMidiChannel(trackId, jsonToInt(findMember(trackObject, "midiChannel"), trackGetMidiChannel(trackId)));
        int midiPort = jsonToInt(findMember(trackObject, "midiPort"), trackGetMidiPort(trackId));
        std::wstring midiPortName = utf8ToWide(jsonToString(findMember(trackObject, "midiPortName")));
        trackSetMidiPort(trackId, midiPort, midiPortName);

        int stepCount = jsonToInt(findMember(trackObject, "stepCount"), trackGetStepCount(trackId));
        trackSetStepCount(trackId, stepCount);
        stepCount = trackGetStepCount(trackId);

        for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
        {
            trackSetStepState(trackId, stepIndex, false);
            trackSetStepVelocity(trackId, stepIndex, kTrackStepVelocityMax);
            trackSetStepPan(trackId, stepIndex, 0.0f);
            trackSetStepPitchOffset(trackId, stepIndex, 0.0f);
        }

        const JsonValue* stepsValue = findMember(trackObject, "steps");
        if (stepsValue && stepsValue->isArray())
        {
            for (const auto& stepValue : stepsValue->asArray())
            {
                if (!stepValue.isObject())
                    continue;

                const auto& stepObject = stepValue.asObject();
                int stepIndex = jsonToInt(findMember(stepObject, "index"), -1);
                if (stepIndex < 0 || stepIndex >= stepCount)
                    continue;

                bool enabled = jsonToBool(findMember(stepObject, "enabled"), false);
                if (!enabled)
                {
                    trackSetStepState(trackId, stepIndex, false);
                    continue;
                }

                std::vector<int> notes;
                const JsonValue* notesValue = findMember(stepObject, "notes");
                if (notesValue && notesValue->isArray())
                {
                    for (const auto& noteValue : notesValue->asArray())
                    {
                        if (noteValue.isNumber())
                        {
                            notes.push_back(static_cast<int>(std::lround(noteValue.asNumber())));
                        }
                        else if (noteValue.isString())
                        {
                            try
                            {
                                notes.push_back(std::stoi(noteValue.asString()));
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                }

                if (!notes.empty())
                {
                    trackSetStepNote(trackId, stepIndex, notes.front());
                    for (size_t noteIndex = 1; noteIndex < notes.size(); ++noteIndex)
                    {
                        trackToggleStepNote(trackId, stepIndex, notes[noteIndex]);
                    }
                }
                else
                {
                    trackSetStepState(trackId, stepIndex, true);
                }

                trackSetStepVelocity(trackId, stepIndex,
                                     jsonToFloat(findMember(stepObject, "velocity"), trackGetStepVelocity(trackId, stepIndex)));
                const JsonValue* noteVelocityArray = findMember(stepObject, "noteVelocities");
                if (noteVelocityArray && noteVelocityArray->isArray())
                {
                    const auto& velocities = noteVelocityArray->asArray();
                    size_t count = std::min(notes.size(), velocities.size());
                    for (size_t noteIndex = 0; noteIndex < count; ++noteIndex)
                    {
                        float velocityValue = jsonToFloat(&velocities[noteIndex],
                                                          trackGetStepNoteVelocity(trackId, stepIndex, notes[noteIndex]));
                        trackSetStepNoteVelocity(trackId, stepIndex, notes[noteIndex], velocityValue);
                    }
                }
                const JsonValue* sustainArray = findMember(stepObject, "noteSustain");
                if (sustainArray && sustainArray->isArray())
                {
                    const auto& sustainValues = sustainArray->asArray();
                    size_t count = std::min(notes.size(), sustainValues.size());
                    for (size_t noteIndex = 0; noteIndex < count; ++noteIndex)
                    {
                        bool sustainValue = jsonToBool(&sustainValues[noteIndex],
                                                       trackGetStepNoteSustain(trackId, stepIndex, notes[noteIndex]));
                        trackSetStepNoteSustain(trackId, stepIndex, notes[noteIndex], sustainValue);
                    }
                }
                trackSetStepPan(trackId, stepIndex,
                                jsonToFloat(findMember(stepObject, "pan"), trackGetStepPan(trackId, stepIndex)));
                trackSetStepPitchOffset(trackId, stepIndex,
                                        jsonToFloat(findMember(stepObject, "pitchOffset"), trackGetStepPitchOffset(trackId, stepIndex)));
            }
        }
    }

    const JsonValue* modMatrixValue = findMember(rootObject, "modMatrix");
    if (modMatrixValue && modMatrixValue->isArray())
    {
        std::vector<ModMatrixAssignment> assignments;
        const auto& modArray = modMatrixValue->asArray();
        assignments.reserve(modArray.size());
        for (const auto& entry : modArray)
        {
            if (!entry.isObject())
                continue;

            const auto& entryObject = entry.asObject();
            ModMatrixAssignment assignment;
            assignment.id = jsonToInt(findMember(entryObject, "id"), 0);
            assignment.sourceIndex = jsonToInt(findMember(entryObject, "source"), 0);
            assignment.trackId = jsonToInt(findMember(entryObject, "trackId"), 0);
            assignment.parameterIndex = jsonToInt(findMember(entryObject, "parameter"), 0);
            assignment.normalizedAmount = modMatrixClampNormalized(
                jsonToFloat(findMember(entryObject, "amount"), assignment.normalizedAmount));
            assignments.push_back(assignment);
        }
        modMatrixSetAssignments(assignments);
    }

    int clampedBpm = std::clamp(bpmValue, 40, 240);
    sequencerBPM.store(clampedBpm, std::memory_order_relaxed);

    int activeTrackId = 0;
    if (!trackIds.empty())
    {
        activeTrackId = trackIds.front();
    }
    else
    {
        auto refreshed = getTracks();
        if (!refreshed.empty())
            activeTrackId = refreshed.front().id;
    }
    setActiveSequencerTrackId(activeTrackId);
    requestSequencerReset();

    return true;
}

