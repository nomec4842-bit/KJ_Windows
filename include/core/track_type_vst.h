#pragma once

#include "core/tracks.h"

#include <memory>

std::shared_ptr<kj::VST3Host> trackGetVstHost(int trackId);
std::shared_ptr<kj::VST3Host> trackEnsureVstHost(int trackId);

