// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include "gameboy/boot_rom.hpp"
#include "gameboy/sgb_sound.hpp"
#include "../../firmware/sgb/prototype_image.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <string_view>
#include <stdexcept>
#include <utility>

int main(int argc,char** argv) {
    try {
        if(argc!=3 && argc!=4) throw std::runtime_error("usage: sgb_original_title_probe sgb|sgb2 GAME [MASTER_CLOCK_LIMIT]");
        const std::string_view model=argv[1];
        if(model!="sgb" && model!="sgb2") throw std::runtime_error("model must be sgb or sgb2");
        std::uint64_t limit=120'000'000;
        if(argc==4) {
            const std::string_view value=argv[3];
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),limit);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !limit || limit>200'000'000)
                throw std::runtime_error("master clock limit must be 1..200000000");
        }
        std::ifstream input(argv[2],std::ios::binary|std::ios::ate);
        if(!input || input.tellg()<0x150 || input.tellg()>16*1024*1024)
            throw std::runtime_error("game must be a readable ROM of 336 bytes..16 MiB");
        gameboy::SgbHostConfig c;
        c.game_rom.resize(static_cast<std::size_t>(input.tellg()));input.seekg(0);
        if(!input.read(reinterpret_cast<char*>(c.game_rom.data()),c.game_rom.size()))
            throw std::runtime_error("could not read complete game image");
        c.model=model=="sgb"?gameboy::HardwareModel::sgb:gameboy::HardwareModel::sgb2;
        c.gb_boot_rom=model=="sgb"?gameboy::sgb_boot_rom():gameboy::sgb2_boot_rom();
        c.program_rom=gameboy::firmware::sgb_prototype_rom();
        gameboy::SgbHost h(std::move(c));gameboy::SgbHost::StereoSample sample;
        std::string_view outcome="clock_limit";
        while(h.cpu().timing().clocks()<limit) {
            if(h.cpu().debug_wram_byte(0x20)==0xff) {outcome="prototype_halt";break;}
            if(!h.step()) {outcome="host_step_failure";break;}
            while(h.pop_sample(sample)) {}
        }
        if(outcome=="clock_limit" && h.cpu().debug_wram_byte(0x20)==0xff) outcome="prototype_halt";
        gameboy::SgbSoundTransfer::Result transfer;
        if(h.cpu().debug_wram_byte(0x25)) {
            gameboy::SgbSoundTransfer::Payload payload{};
            for(unsigned i=0;i<payload.size();++i) payload[i]=h.cpu().debug_wram_byte(0x1000+i);
            transfer=gameboy::SgbSoundTransfer::parse(payload);
        }
        const auto ram=[&](unsigned address){return unsigned(h.cpu().debug_wram_byte(address));};
        std::cout<<"{\"schema\":\"gbb-sgb-original-title-probe-v1\",\"model\":\""<<model
                 <<"\",\"qualification\":false,\"outcome\":\""<<outcome
                 <<"\",\"master_clocks\":"<<h.cpu().timing().clocks()<<",\"clock_limit\":"<<limit
                 <<",\"gb_frames\":"<<h.icd().completed_frames()<<",\"host_status\":"<<unsigned(h.status())
                 <<",\"firmware_state\":"<<ram(0x20)<<",\"unsupported_header\":"<<ram(0x21)
                 <<",\"transfer_error\":"<<ram(0x27)<<",\"last_header\":"<<ram(0x100)
                 <<",\"validated_jump\":"<<(ram(0x44)|(ram(0x45)<<8))
                 <<",\"captures\":"<<ram(0x25)<<",\"handoffs\":"<<ram(0x26)
                 <<",\"adoptions\":"<<ram(0x2a)<<",\"mailbox_version\":"<<ram(0x2b)
                 <<",\"external_owner\":"<<ram(0x24)<<",\"packets\":"<<h.icd().packets_delivered()
                 <<",\"sounds\":"<<h.icd().sound_packets_delivered()
                 <<",\"cpu_pc\":"<<h.cpu().registers().pc
                 <<",\"fault_pc\":"<<(outcome=="host_step_failure"?h.fault().pc:0)
                 <<",\"captured_list_error\":\""<<(ram(0x25)?gameboy::SgbSoundTransfer::error_name(transfer.error):"not_captured")
                 <<"\",\"captured_jump\":"<<transfer.jump_address<<",\"captured_writes\":[";
        for(std::size_t i=0;i<transfer.writes.size();++i) {
            if(i) std::cout<<',';
            const auto& write=transfer.writes[i];
            std::cout<<"{\"destination\":"<<write.destination<<",\"size\":"<<write.size<<'}';
        }
        std::cout<<"]}\n";
        return 0; // Inventory outcome, never a title-compatibility pass.
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 2;}
}
