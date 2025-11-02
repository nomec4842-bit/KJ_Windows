#pragma once

#include "core/effects/effect_plugin.h"

#include <cstddef>
#include <string>

#if defined(_WIN32)
#    include <windows.h>
#else
using HMODULE = void*;
#endif

class DllEffectHost
{
public:
    explicit DllEffectHost(std::wstring path);
    ~DllEffectHost();

    DllEffectHost(DllEffectHost&& other) noexcept;
    DllEffectHost& operator=(DllEffectHost&& other) noexcept;

    DllEffectHost(const DllEffectHost&) = delete;
    DllEffectHost& operator=(const DllEffectHost&) = delete;

    [[nodiscard]] bool isLoaded() const noexcept;
    [[nodiscard]] const EffectDescriptor* descriptor() const noexcept;

    void* createInstance(double sampleRate) const;
    void destroyInstance(void* instance) const;
    void setParameter(void* instance, const char* parameterId, float value) const;
    void process(void* instance, float* left, float* right, std::size_t frameCount) const;
    void reset(void* instance) const;

private:
    void load();
    void unload() noexcept;
    const EffectDescriptor& ensureDescriptor() const;

    std::wstring m_path;
    HMODULE m_module{nullptr};
    const EffectDescriptor* m_descriptor{nullptr};
};
