// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/snes_brr.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("ASSET");
        std::array<unsigned char, 192> asset{};
        std::ifstream input(argv[1], std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(asset.data()), asset.size()) ||
            input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("asset length");
        std::cout << "[";
        for (unsigned source = 0; source < 2; ++source) {
            const unsigned base = 64 + source * 64, count = asset[16 + source];
            const unsigned loop = asset[10 + source*4] | (asset[11 + source*4] << 8);
            if (count < 1 || count > 4 || loop < 0x5000 + base ||
                (loop - 0x5000 - base) % 9 || (loop - 0x5000 - base) / 9 >= count)
                throw std::runtime_error("sample bound");
            gameboy::SnesBrrDecoder decoder;
            std::cout << (source ? ",[" : "[");
            bool first = true;
            for (unsigned traversal = 0; traversal < 2; ++traversal) {
                const unsigned start = traversal ? (loop - 0x5000 - base) / 9 : 0;
                for (unsigned block = start; block < count; ++block) {
                    gameboy::SnesBrrDecoder::EncodedBlock bytes{};
                    std::copy_n(asset.begin() + base + block*9, 9, bytes.begin());
                    for (auto value : decoder.decode(bytes).samples) {
                        std::cout << (first ? "" : ",") << value;
                        first = false;
                    }
                }
            }
            std::cout << "]";
        }
        std::cout << "]\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
