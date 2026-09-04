#pragma once

#include <stdexcept>

namespace gameboy {

class SaveStateError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace gameboy
