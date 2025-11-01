#pragma once

#include <cstddef>

struct EffectParameterInfo
{
    const char* id;
    const char* name;
    float minValue;
    float maxValue;
    float defaultValue;
};

struct EffectDescriptor
{
    const char* identifier;
    const char* displayName;
    std::size_t parameterCount;
    const EffectParameterInfo* parameters;
    void* (*createInstance)(double sampleRate);
    void (*destroyInstance)(void* instance);
    void (*setParameter)(void* instance, const char* parameterId, float value);
    void (*process)(void* instance, float* left, float* right, std::size_t frameCount);
    void (*reset)(void* instance);
};

using GetEffectDescriptorFn = const EffectDescriptor* (*)();

extern "C" const EffectDescriptor* getEffectDescriptor();
