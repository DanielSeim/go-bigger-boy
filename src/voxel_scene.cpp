#include "gbb/voxel_scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>

namespace gbb {
namespace {

constexpr int tile_size = 8;

unsigned bit_count(const std::uint8_t value) {
    unsigned count = 0;
    auto remaining = value;
    while (remaining != 0) {
        count += remaining & 1U;
        remaining = static_cast<std::uint8_t>(remaining >> 1U);
    }
    return count;
}

std::size_t opaque_pixels(const SceneVisibleTileCell& cell) {
    return std::accumulate(
        cell.opaque_mask.begin(), cell.opaque_mask.end(), std::size_t{0},
        [](const std::size_t total, const std::uint8_t row) {
            return total + bit_count(row);
        });
}

bool compatible_edge(const SceneVisibleTileCell& left,
                     const SceneVisibleTileCell& right) {
    const auto same_tile_context =
        left.tile_id == right.tile_id &&
        (left.attributes & 0x78U) == (right.attributes & 0x78U) &&
        left.tile_bank == right.tile_bank && left.palette == right.palette;
    if (same_tile_context) return true;

    // A tile reused with a different flip is still a structural hint, but a
    // different bank or palette is kept separate because CGB games often use
    // those for unrelated materials.
    if (left.tile_id == right.tile_id &&
        (left.attributes & 0x18U) == (right.attributes & 0x18U))
        return true;

    // Do not join unrelated tile IDs merely because their silhouettes happen
    // to have similar density. Shared tile graphics are the only generic
    // provenance signal strong enough for an automatic accepted object;
    // profiles can add stronger game-specific templates later.
    return false;
}

struct DisjointSet {
    explicit DisjointSet(const std::size_t size) : parent(size), rank(size, 0) {
        std::iota(parent.begin(), parent.end(), std::size_t{0});
    }

    std::size_t find(std::size_t value) {
        while (parent[value] != value) {
            parent[value] = parent[parent[value]];
            value = parent[value];
        }
        return value;
    }

    void join(std::size_t left, std::size_t right) {
        left = find(left);
        right = find(right);
        if (left == right) return;
        if (rank[left] < rank[right]) std::swap(left, right);
        parent[right] = left;
        if (rank[left] == rank[right]) ++rank[left];
    }

    std::vector<std::size_t> parent;
    std::vector<unsigned> rank;
};

struct CandidateFeatures {
    float compactness{};
    float repetition{};
    float boundary{};
    float ground_contact{};
    float aspect{};
};

CandidateFeatures features_for(
    const std::vector<std::size_t>& component,
    const std::vector<SceneVisibleTileCell>& cells) {
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    bool first = true;
    std::map<std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>, std::size_t>
        signatures;
    std::map<std::pair<int, int>, bool> occupied;
    for (const auto index : component) {
        const auto& cell = cells[index];
        const auto x = static_cast<int>(cell.screen_x);
        const auto y = static_cast<int>(cell.screen_y);
        if (first) {
            min_x = max_x = x;
            min_y = max_y = y;
            first = false;
        } else {
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
        occupied[{x, y}] = true;
        ++signatures[{cell.tile_id, static_cast<std::uint8_t>(cell.attributes & 0x78U),
                      cell.palette}];
    }
    const auto width = max_x - min_x + tile_size;
    const auto height = max_y - min_y + tile_size;
    const auto box_cells = std::max(1, (width / tile_size) * (height / tile_size));
    const auto area = static_cast<float>(component.size());
    const auto repeated = std::accumulate(
        signatures.begin(), signatures.end(), std::size_t{0},
        [](const std::size_t total, const auto& entry) {
            return total + (entry.second > 1 ? entry.second - 1 : 0);
        });
    std::size_t perimeter = 0;
    for (const auto index : component) {
        const auto x = static_cast<int>(cells[index].screen_x);
        const auto y = static_cast<int>(cells[index].screen_y);
        for (const auto [dx, dy] :
             std::array<std::pair<int, int>, 4>{{{-tile_size, 0},
                                                  {tile_size, 0},
                                                  {0, -tile_size},
                                                  {0, tile_size}}}) {
            if (occupied.find({x + dx, y + dy}) == occupied.end()) ++perimeter;
        }
    }
    const auto ground_row = max_y;
    const auto ground_cells = std::count_if(
        component.begin(), component.end(), [&](const std::size_t index) {
            return static_cast<int>(cells[index].screen_y) >= ground_row - tile_size;
        });
    const auto aspect = static_cast<float>(width) /
                        static_cast<float>(std::max(1, height));
    return {
        std::min(1.0F, area / static_cast<float>(box_cells)),
        std::min(1.0F, static_cast<float>(repeated) /
                           std::max(1.0F, area - 1.0F)),
        std::min(1.0F, static_cast<float>(perimeter) /
                           std::max(1.0F, area * 2.0F)),
        std::min(1.0F, static_cast<float>(ground_cells) /
                           std::max(1.0F, area / 3.0F)),
        std::exp(-std::abs(std::log(std::max(0.25F, aspect)))),
    };
}

float candidate_confidence(const std::vector<std::size_t>& component,
                           const std::vector<SceneVisibleTileCell>& cells) {
    const auto features = features_for(component, cells);
    const auto area = std::min(1.0F, static_cast<float>(component.size()) / 8.0F);
    return std::min(1.0F,
                    0.20F * area + 0.23F * features.compactness +
                        0.17F * features.repetition + 0.16F * features.boundary +
                        0.14F * features.ground_contact + 0.10F * features.aspect);
}

void add_sprite_objects(const SceneSnapshot& snapshot, VoxelScene& scene) {
    const auto sprite_height = (snapshot.lcdc & 0x04U) != 0 ? 16 : 8;
    const auto sprite_pixel_opaque = [&](const SceneSprite& sprite, const int x,
                                         const int y) {
        if (snapshot.tile_size_bytes == 0 || snapshot.tile_data.empty()) return false;
        const auto source_x = (sprite.attributes & 0x20U) != 0 ? 7 - x : x;
        const auto source_y = (sprite.attributes & 0x40U) != 0
                                  ? sprite_height - 1 - y
                                  : y;
        const auto tile = sprite_height == 16
                              ? static_cast<unsigned>(sprite.tile & 0xFEU)
                              : static_cast<unsigned>(sprite.tile);
        const auto tile_index = tile + static_cast<unsigned>(source_y / 8);
        const auto bank = snapshot.cgb_mode && (sprite.attributes & 0x08U) != 0
                              ? 1U
                              : 0U;
        const auto offset = static_cast<std::size_t>(bank) *
                                snapshot.tile_bank_stride +
                            static_cast<std::size_t>(tile_index) *
                                snapshot.tile_size_bytes +
                            static_cast<std::size_t>(source_y % 8) * 2U;
        if (offset + 1 >= snapshot.tile_data.size()) return false;
        const auto bit = static_cast<unsigned>(7 - source_x);
        return (((snapshot.tile_data[offset] >> bit) & 1U) |
                (((snapshot.tile_data[offset + 1] >> bit) & 1U) << 1U)) != 0;
    };

    for (std::size_t index = 0; index < snapshot.sprites.size(); ++index) {
        const auto& sprite = snapshot.sprites[index];
        if (!sprite.visible) continue;
        VoxelObject object;
        object.id = "sprite-" + std::to_string(index);
        object.kind = VoxelObjectKind::sprite;
        object.confidence = 1.0F;
        object.min_x = sprite.screen_x;
        object.min_y = sprite.screen_y;
        object.max_x = static_cast<std::int16_t>(sprite.screen_x + 8);
        object.max_y = static_cast<std::int16_t>(sprite.screen_y + sprite_height);
        object.anchor_y = object.max_y;
        const auto object_index = scene.objects.size();
        scene.objects.push_back(std::move(object));
        for (int local_y = 0; local_y < sprite_height; ++local_y) {
            for (int local_x = 0; local_x < 8; ++local_x) {
                if (!sprite_pixel_opaque(sprite, local_x, local_y)) continue;
                const auto x = static_cast<int>(sprite.screen_x) + local_x;
                const auto y = static_cast<int>(sprite.screen_y) + local_y;
                if (x < 0 || y < 0 || x >= static_cast<int>(snapshot.width) ||
                    y >= static_cast<int>(snapshot.height)) continue;
                scene.object_owner[static_cast<std::size_t>(y) * snapshot.width +
                                   static_cast<std::size_t>(x)] =
                    static_cast<std::int32_t>(object_index);
            }
        }
    }
}

} // namespace

VoxelScene build_voxel_scene(const SceneSnapshot& snapshot,
                             const VoxelSceneBuildOptions& options) {
    VoxelScene scene;
    scene.width = snapshot.width;
    scene.height = snapshot.height;
    scene.object_owner.assign(scene.width * scene.height, -1);

    if (options.include_sprites) add_sprite_objects(snapshot, scene);
    if (!options.detect_background_objects) return scene;

    std::vector<SceneVisibleTileCell> cells;
    std::vector<std::size_t> source_indices;
    cells.reserve(snapshot.visible_tile_cells.size());
    source_indices.reserve(snapshot.visible_tile_cells.size());
    for (std::size_t source_index = 0;
         source_index < snapshot.visible_tile_cells.size(); ++source_index) {
        const auto& cell = snapshot.visible_tile_cells[source_index];
        if (cell.source != SceneTileSource::background || opaque_pixels(cell) == 0)
            continue;
        cells.push_back(cell);
        source_indices.push_back(source_index);
    }
    if (cells.size() < options.minimum_background_cells) return scene;

    std::map<std::pair<int, int>, std::size_t> positions;
    for (std::size_t index = 0; index < cells.size(); ++index)
        positions[{cells[index].screen_x, cells[index].screen_y}] = index;
    std::vector<bool> claimed(cells.size(), false);

    const auto make_object = [&](const std::vector<std::size_t>& component,
                                 const float confidence,
                                 const std::string& id) {
        VoxelObject object;
        object.id = id;
        object.confidence = confidence;
        object.source_cells.reserve(component.size());
        bool first = true;
        for (const auto cell_index : component) {
            const auto& cell = cells[cell_index];
            object.source_cells.push_back(source_indices[cell_index]);
            object.map_cells.emplace_back(cell.map_x, cell.map_y);
            const auto cell_right = static_cast<int>(cell.screen_x) +
                                    std::max<int>(1, cell.visible_width);
            const auto cell_bottom = static_cast<int>(cell.screen_y) +
                                     std::max<int>(1, cell.visible_height);
            if (first) {
                object.min_x = cell.screen_x;
                object.min_y = cell.screen_y;
                object.max_x = static_cast<std::int16_t>(cell_right);
                object.max_y = static_cast<std::int16_t>(cell_bottom);
                first = false;
            } else {
                object.min_x = std::min(object.min_x, cell.screen_x);
                object.min_y = std::min(object.min_y, cell.screen_y);
                object.max_x = std::max(object.max_x,
                                        static_cast<std::int16_t>(cell_right));
                object.max_y = std::max(object.max_y,
                                        static_cast<std::int16_t>(cell_bottom));
            }
        }
        object.anchor_y = object.max_y;
        return object;
    };
    const auto paint_object = [&](const std::vector<std::size_t>& component,
                                  const std::size_t object_index) {
        for (const auto cell_index : component) {
            const auto& cell = cells[cell_index];
            for (int row = 0; row < cell.visible_height; ++row) {
                for (int column = 0; column < cell.visible_width; ++column) {
                    const auto x = static_cast<int>(cell.screen_x) + column;
                    const auto y = static_cast<int>(cell.screen_y) + row;
                    if (x < 0 || y < 0 || x >= static_cast<int>(scene.width) ||
                        y >= static_cast<int>(scene.height)) continue;
                    const auto source_x = std::clamp(column, 0, 7);
                    const auto source_y = std::clamp(row, 0, 7);
                    if ((cell.opaque_mask[static_cast<std::size_t>(source_y)] &
                         (1U << source_x)) == 0)
                        continue;
                    scene.object_owner[static_cast<std::size_t>(y) * scene.width +
                                       static_cast<std::size_t>(x)] =
                        static_cast<std::int32_t>(object_index);
                }
            }
        }
    };
    const auto accept_object = [&](const std::vector<std::size_t>& component,
                                   VoxelObject object) {
        scene.candidates.push_back(object);
        const auto object_index = scene.objects.size();
        scene.objects.push_back(std::move(object));
        paint_object(component, object_index);
    };

    // Authored templates are checked before generic graph components. This
    // lets a profile keep a roof, wall, door and window together even when
    // they use unrelated tile IDs. A match must be fully visible and cannot
    // reuse a cell already claimed by another template.
    if (options.background_templates != nullptr) {
        std::size_t match_number = 0;
        for (const auto& [position, top_index] : positions) {
            const auto top_x = position.first;
            const auto top_y = position.second;
            for (const auto& object_template : *options.background_templates) {
                if (object_template.width == 0 || object_template.height == 0 ||
                    object_template.tile_ids.size() !=
                        static_cast<std::size_t>(object_template.width) *
                            object_template.height)
                    continue;
                std::vector<std::size_t> matched;
                matched.reserve(object_template.tile_ids.size());
                bool matches = true;
                for (std::uint32_t row = 0; row < object_template.height && matches;
                     ++row) {
                    for (std::uint32_t column = 0;
                         column < object_template.width; ++column) {
                        const auto found = positions.find({
                            top_x + static_cast<int>(column) * tile_size,
                            top_y + static_cast<int>(row) * tile_size});
                        if (found == positions.end() || claimed[found->second] ||
                            cells[found->second].visible_width != 8 ||
                            cells[found->second].visible_height != 8) {
                            matches = false;
                            break;
                        }
                        const auto wanted = object_template.tile_ids[
                            static_cast<std::size_t>(row) * object_template.width +
                            column];
                        if (wanted >= 0 && cells[found->second].tile_id != wanted) {
                            matches = false;
                            break;
                        }
                        matched.push_back(found->second);
                    }
                }
                if (!matches) continue;
                for (const auto cell_index : matched) claimed[cell_index] = true;
                const auto match_id = match_number++;
                const auto id = "template-" +
                                (object_template.id.empty()
                                     ? std::to_string(match_id)
                                     : object_template.id) +
                                "-" + std::to_string(match_id);
                accept_object(matched,
                              make_object(matched, 1.0F, id));
            }
        }
    }

    DisjointSet sets(cells.size());
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (claimed[index]) continue;
        const auto x = static_cast<int>(cells[index].screen_x);
        const auto y = static_cast<int>(cells[index].screen_y);
        for (const auto [dx, dy] :
             std::array<std::pair<int, int>, 2>{{{tile_size, 0}, {0, tile_size}}}) {
            const auto neighbour = positions.find({x + dx, y + dy});
            if (neighbour != positions.end() && !claimed[neighbour->second] &&
                compatible_edge(cells[index], cells[neighbour->second]))
                sets.join(index, neighbour->second);
        }
    }

    std::map<std::size_t, std::vector<std::size_t>> components;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (!claimed[index]) components[sets.find(index)].push_back(index);
    }

    for (const auto& [_, component] : components) {
        if (component.size() < options.minimum_background_cells ||
            static_cast<float>(component.size()) /
                    static_cast<float>(cells.size()) >
                options.maximum_background_fraction)
            continue;
        const auto confidence = candidate_confidence(component, cells);
        const auto object = make_object(
            component, confidence,
            "background-object-" + std::to_string(scene.objects.size()));
        if (confidence >= options.accepted_confidence)
            accept_object(component, object);
        else
            scene.candidates.push_back(object);
    }
    return scene;
}

} // namespace gbb
