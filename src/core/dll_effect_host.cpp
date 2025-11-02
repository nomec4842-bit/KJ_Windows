#include "core/effects/dll_effect_host.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace
{
std::string narrowPath(const std::wstring& path)
{
    std::string result;
    result.reserve(path.size());
    for (wchar_t ch : path)
    {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

[[noreturn]] void throwError(const std::wstring& path, const char* message)
{
    throw std::runtime_error(std::string(message) + ": " + narrowPath(path));
}
}

DllEffectHost::DllEffectHost(std::wstring path)
    : m_path(std::move(path))
{
    load();
}

DllEffectHost::~DllEffectHost()
{
    unload();
}

DllEffectHost::DllEffectHost(DllEffectHost&& other) noexcept
    : m_path(std::move(other.m_path))
    , m_module(other.m_module)
    , m_descriptor(other.m_descriptor)
{
    other.m_module = nullptr;
    other.m_descriptor = nullptr;
}

DllEffectHost& DllEffectHost::operator=(DllEffectHost&& other) noexcept
{
    if (this != &other)
    {
        unload();
        m_path = std::move(other.m_path);
        m_module = other.m_module;
        m_descriptor = other.m_descriptor;
        other.m_module = nullptr;
        other.m_descriptor = nullptr;
    }
    return *this;
}

bool DllEffectHost::isLoaded() const noexcept
{
    return m_descriptor != nullptr;
}

const EffectDescriptor* DllEffectHost::descriptor() const noexcept
{
    return m_descriptor;
}

void* DllEffectHost::createInstance(double sampleRate) const
{
    const auto& desc = ensureDescriptor();
    if (!desc.createInstance)
    {
        throw std::runtime_error("DLL effect host missing createInstance callback");
    }
    return desc.createInstance(sampleRate);
}

void DllEffectHost::destroyInstance(void* instance) const
{
    const auto& desc = ensureDescriptor();
    if (!desc.destroyInstance)
    {
        throw std::runtime_error("DLL effect host missing destroyInstance callback");
    }
    desc.destroyInstance(instance);
}

void DllEffectHost::setParameter(void* instance, const char* parameterId, float value) const
{
    const auto& desc = ensureDescriptor();
    if (!desc.setParameter)
    {
        throw std::runtime_error("DLL effect host missing setParameter callback");
    }
    desc.setParameter(instance, parameterId, value);
}

void DllEffectHost::process(void* instance, float* left, float* right, std::size_t frameCount) const
{
    const auto& desc = ensureDescriptor();
    if (!desc.process)
    {
        throw std::runtime_error("DLL effect host missing process callback");
    }
    desc.process(instance, left, right, frameCount);
}

void DllEffectHost::reset(void* instance) const
{
    const auto& desc = ensureDescriptor();
    if (!desc.reset)
    {
        throw std::runtime_error("DLL effect host missing reset callback");
    }
    desc.reset(instance);
}

void DllEffectHost::load()
{
#if defined(_WIN32)
    unload();

    m_module = ::LoadLibraryW(m_path.c_str());
    if (!m_module)
    {
        throwError(m_path, "Failed to load plugin DLL");
    }

    auto getDescriptor = reinterpret_cast<GetEffectDescriptorFn>(::GetProcAddress(m_module, "getEffectDescriptor"));
    if (!getDescriptor)
    {
        unload();
        throwError(m_path, "Missing getEffectDescriptor symbol");
    }

    m_descriptor = getDescriptor();
    if (!m_descriptor)
    {
        unload();
        throwError(m_path, "getEffectDescriptor returned null");
    }
#else
    throwError(m_path, "DLL hosting is only supported on Windows builds");
#endif
}

void DllEffectHost::unload() noexcept
{
#if defined(_WIN32)
    if (m_module)
    {
        ::FreeLibrary(m_module);
    }
#endif
    m_module = nullptr;
    m_descriptor = nullptr;
}

const EffectDescriptor& DllEffectHost::ensureDescriptor() const
{
    if (!m_descriptor)
    {
        throwError(m_path, "Effect descriptor not available");
    }
    return *m_descriptor;
}
