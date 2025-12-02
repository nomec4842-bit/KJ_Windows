#pragma once

#include <mutex>

namespace kj {

using once_flag = std::once_flag;
using std::call_once;

} // namespace kj

