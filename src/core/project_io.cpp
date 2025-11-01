#include "core/project_io.h"

#include "core/sequencer.h"
#include "core/tracks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

TrackType trackTypeFromString(const std::string& value)
{
    if (value == "Sample")
        return TrackType::Sample;
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
        bool delayEnabled = trackGetDelayEnabled(track.id);
        float delayTimeMs = trackGetDelayTimeMs(track.id);
        float delayFeedback = trackGetDelayFeedback(track.id);
        float delayMix = trackGetDelayMix(track.id);
        float formant = trackGetSynthFormant(track.id);
        float feedback = trackGetSynthFeedback(track.id);
        float pitch = trackGetSynthPitch(track.id);
        float pitchRange = trackGetSynthPitchRange(track.id);
        float synthAttack = trackGetSynthAttack(track.id);
        float synthDecay = trackGetSynthDecay(track.id);
        float synthSustain = trackGetSynthSustain(track.id);
        float synthRelease = trackGetSynthRelease(track.id);
        float sampleAttack = trackGetSampleAttack(track.id);
        float sampleRelease = trackGetSampleRelease(track.id);
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
        stream << "      \"delayEnabled\": " << (delayEnabled ? "true" : "false") << ",\n";
        stream << "      \"delayTimeMs\": " << formatFloat(delayTimeMs) << ",\n";
        stream << "      \"delayFeedback\": " << formatFloat(delayFeedback) << ",\n";
        stream << "      \"delayMix\": " << formatFloat(delayMix) << ",\n";
        stream << "      \"formant\": " << formatFloat(formant) << ",\n";
        stream << "      \"feedback\": " << formatFloat(feedback) << ",\n";
        stream << "      \"pitch\": " << formatFloat(pitch) << ",\n";
        stream << "      \"pitchRange\": " << formatFloat(pitchRange) << ",\n";
        stream << "      \"synthAttack\": " << formatFloat(synthAttack) << ",\n";
        stream << "      \"synthDecay\": " << formatFloat(synthDecay) << ",\n";
        stream << "      \"synthSustain\": " << formatFloat(synthSustain) << ",\n";
        stream << "      \"synthRelease\": " << formatFloat(synthRelease) << ",\n";
        stream << "      \"sampleAttack\": " << formatFloat(sampleAttack) << ",\n";
        stream << "      \"sampleRelease\": " << formatFloat(sampleRelease) << ",\n";
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
        trackSetSynthFormant(trackId, jsonToFloat(findMember(trackObject, "formant"), trackGetSynthFormant(trackId)));
        trackSetSynthFeedback(trackId, jsonToFloat(findMember(trackObject, "feedback"), trackGetSynthFeedback(trackId)));
        trackSetSynthPitch(trackId, jsonToFloat(findMember(trackObject, "pitch"), trackGetSynthPitch(trackId)));
        trackSetSynthPitchRange(trackId, jsonToFloat(findMember(trackObject, "pitchRange"), trackGetSynthPitchRange(trackId)));
        trackSetSynthAttack(trackId, jsonToFloat(findMember(trackObject, "synthAttack"), trackGetSynthAttack(trackId)));
        trackSetSynthDecay(trackId, jsonToFloat(findMember(trackObject, "synthDecay"), trackGetSynthDecay(trackId)));
        trackSetSynthSustain(trackId, jsonToFloat(findMember(trackObject, "synthSustain"), trackGetSynthSustain(trackId)));
        trackSetSynthRelease(trackId, jsonToFloat(findMember(trackObject, "synthRelease"), trackGetSynthRelease(trackId)));
        trackSetSampleAttack(trackId, jsonToFloat(findMember(trackObject, "sampleAttack"), trackGetSampleAttack(trackId)));
        trackSetSampleRelease(trackId, jsonToFloat(findMember(trackObject, "sampleRelease"), trackGetSampleRelease(trackId)));
        trackSetDelayEnabled(trackId, jsonToBool(findMember(trackObject, "delayEnabled"), trackGetDelayEnabled(trackId)));
        trackSetDelayTimeMs(trackId, jsonToFloat(findMember(trackObject, "delayTimeMs"), trackGetDelayTimeMs(trackId)));
        trackSetDelayFeedback(trackId, jsonToFloat(findMember(trackObject, "delayFeedback"), trackGetDelayFeedback(trackId)));
        trackSetDelayMix(trackId, jsonToFloat(findMember(trackObject, "delayMix"), trackGetDelayMix(trackId)));

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
                trackSetStepPan(trackId, stepIndex,
                                jsonToFloat(findMember(stepObject, "pan"), trackGetStepPan(trackId, stepIndex)));
                trackSetStepPitchOffset(trackId, stepIndex,
                                        jsonToFloat(findMember(stepObject, "pitchOffset"), trackGetStepPitchOffset(trackId, stepIndex)));
            }
        }
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

