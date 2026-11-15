// Run from an x86 MSVC developer prompt:
// cl /nologo /EHsc /std:c++17 tests\StorageExpansionTest.cpp /Fe:StorageExpansionTest.exe
// Memory writes and detour installation are recorded, never applied to a game.
#include <cassert>
#include <map>
#include "../ezorsia/HpMpAlert.cpp"

namespace {
std::map<DWORD, unsigned char> patches;
unsigned int writeCount = 0;
unsigned int nativePackets = 0;

void __fastcall NativeProcessPacket(void*, void*, CInPacket*) {
    ++nativePackets;
}

void AssertLimit(unsigned char limit) {
    for (DWORD address : {0x0046C5F7, 0x00470935, 0x004B16D7, 0x004BA582, 0x004BA597}) {
        assert(patches.at(address) == limit);
    }
}
}

void Memory::WriteByte(DWORD address, unsigned char value) {
    patches[address] = value;
    ++writeCount;
}

void Memory::WriteInt(DWORD, unsigned int) {
    assert(false && "Storage configuration must not write HP/MP status-bar fields");
}

bool Memory::SetHook(bool, void**, void*) {
    return true;
}

int main() {
    HookHpMpAlertRecv(true);
    AssertLimit(48);
    assert(patches.at(0x0047AB5D) == 124);
    s_ProcessPacket = NativeProcessPacket;

    unsigned char data[] = {0, 0, 0, 0, 0x01, 0x10, 124};
    CInPacket packet{};
    packet.Data = data;
    packet.Size = sizeof(data);
    packet.RawSeq = 0xFFFF;
    packet.DataLen = sizeof(data) - 4;
    packet.Offset = 4;

    for (unsigned char limit : {4, 48, 96, 124}) {
        data[6] = limit;
        ProcessPacket_Hook(nullptr, nullptr, &packet);
        AssertLimit(limit);
        assert(patches.at(0x0047AB5D) == 124);
        assert(nativePackets == 0);
    }

    const unsigned int previousWrites = writeCount;
    for (unsigned char invalid : {0, 3, 125, 128, 255}) {
        data[6] = invalid;
        ProcessPacket_Hook(nullptr, nullptr, &packet);
        AssertLimit(124);
        assert(writeCount == previousWrites);
    }
    data[6] = 124;
    ProcessPacket_Hook(nullptr, nullptr, &packet);
    assert(writeCount == previousWrites);

    packet.Size = 6; // A high sequence number must not inflate this 16-bit length.
    data[6] = 48; // Outside the declared packet: this value must not be applied.
    ProcessPacket_Hook(nullptr, nullptr, &packet);
    assert(nativePackets == 0);
    assert(writeCount == previousWrites);

    packet.Size = sizeof(data);
    data[4] = 0x02; // Unrelated opcodes must still reach the original dispatcher.
    ProcessPacket_Hook(nullptr, nullptr, &packet);
    assert(nativePackets == 1);

    // A partial opcode must not be mistaken for the custom configuration opcode.
    packet.Size = 5;
    data[4] = 0x01;
    ProcessPacket_Hook(nullptr, nullptr, &packet);
    assert(nativePackets == 2);
    assert(writeCount == previousWrites);

    HookHpMpAlertRecv(false);
    AssertLimit(48);
    assert(patches.at(0x0047AB5D) == 48);
    std::cout << "Storage expansion client checks passed\n";
}
