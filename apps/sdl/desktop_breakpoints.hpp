#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace gbb::sdl {

// Keeps desktop debugger breakpoints independent from SDL window state. A
// breakpoint is checked against the CPU's pre-instruction PC, so the paused
// machine is ready to inspect before the instruction at that address runs.
class DesktopBreakpoints {
  public:
    [[nodiscard]] bool toggle(const std::uint16_t address) {
        const auto found = std::lower_bound(addresses_.begin(), addresses_.end(),
                                            address);
        if (found != addresses_.end() && *found == address) {
            addresses_.erase(found);
            if (resume_address_ == address) resume_address_.reset();
            return false;
        }
        addresses_.insert(found, address);
        return true;
    }

    [[nodiscard]] bool contains(const std::uint16_t address) const noexcept {
        return std::binary_search(addresses_.begin(), addresses_.end(), address);
    }

    void clear() noexcept {
        addresses_.clear();
        resume_address_.reset();
        last_hit_.reset();
    }

    [[nodiscard]] bool empty() const noexcept { return addresses_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return addresses_.size(); }
    [[nodiscard]] const std::vector<std::uint16_t>& addresses() const noexcept {
        return addresses_;
    }
    [[nodiscard]] std::optional<std::uint16_t> last_hit() const noexcept {
        return last_hit_;
    }

    // When resuming from a breakpoint, execute the instruction currently
    // under the PC once before allowing that same breakpoint to fire again.
    void resume_after_hit() noexcept {
        if (last_hit_ && contains(*last_hit_)) resume_address_ = last_hit_;
        else resume_address_.reset();
        last_hit_.reset();
    }

    [[nodiscard]] std::optional<std::uint16_t> check(
        const std::uint16_t address) noexcept {
        if (resume_address_) {
            if (*resume_address_ == address) {
                resume_address_.reset();
                return std::nullopt;
            }
            resume_address_.reset();
        }
        if (!contains(address)) return std::nullopt;
        last_hit_ = address;
        return address;
    }

  private:
    std::vector<std::uint16_t> addresses_;
    std::optional<std::uint16_t> resume_address_;
    std::optional<std::uint16_t> last_hit_;
};

}  // namespace gbb::sdl
