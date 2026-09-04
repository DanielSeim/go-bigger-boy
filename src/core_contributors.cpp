#include "gbb/core_contributors.hpp"
#include "gbb/gameboy_core_factory.hpp"

namespace gbb {

std::vector<CoreFactory> built_in_core_factories() {
    return {gameboy_core_factory()};
}

} // namespace gbb
