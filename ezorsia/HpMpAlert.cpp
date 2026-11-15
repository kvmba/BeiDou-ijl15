#include "stdafx.h"
#include "HpMpAlert.h"
#include <cstddef>
namespace {
constexpr DWORD kSaveGlobalAddr = 0x0049C8E7;
constexpr DWORD kUIStatusBarPtr = 0x00BEBF9C;
constexpr DWORD kHpAlertOffset = 0x80;
constexpr DWORD kMpAlertOffset = 0x84;
constexpr DWORD kClientSocketPtr = 0x00BE7914;
constexpr DWORD kProcessPacketAddr = 0x004965F1;
constexpr WORD kOpcodeSetHpMpAlert = 0x1000;
constexpr WORD kOpcodeStorageSlotLimit = 0x1001;
constexpr unsigned char kMaxStorageSlots = 124;
static unsigned char s_storageSlotLimit = 0;

static void ApplyStorageSlotLimit(unsigned char limit) {
    if (limit < 4 || limit > kMaxStorageSlots || limit == s_storageSlotLimit) {
        return;
    }
    // GMS083 comparison immediates; keep the original instructions and branches.
    const DWORD addresses[] = {
        0x0046C5F7, // CCashShop::OnIncTrunkCount
        0x00470935, // CCashShop::OnBuySlotInc
        0x004B16D7, // CCSWnd_Inventory::EnableExTrunkButton
        0x004BA582, // Unknown capacity must remain unpurchasable.
        0x004BA597  // CCSWnd_List::ResetSlotIncComm
    };
    for (DWORD address : addresses) {
        Memory::WriteByte(address, limit);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    s_storageSlotLimit = limit;
}

struct COutPacket {
    int Loopback;
    union {
        unsigned char* Data;
        void* Unk;
        unsigned short* Header;
    };
    unsigned long Size;
    unsigned int Offset;
    int EncryptedByShanda;
};
struct CInPacket {
    int Loopback;
    int State;
    void* Data;
    unsigned short Size;
    unsigned short RawSeq;
    unsigned short DataLen;
    unsigned short Unknown;
    unsigned int Offset;
};
static_assert(sizeof(CInPacket) == 0x18 && offsetof(CInPacket, Size) == 0x0C
    && offsetof(CInPacket, Offset) == 0x14, "CInPacket must match the GMS083 x86 layout");
using SendPacket_t = void(__fastcall*)(void* pThis, void* edx, COutPacket* packet);
static SendPacket_t g_SendPacket = reinterpret_cast<SendPacket_t>(0x0049637B);
static bool TryReadDword(DWORD address, DWORD& out) {
    __try {
        out = *reinterpret_cast<DWORD*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}
static unsigned char ClampAlert(int value) {
    if (value < 0) return 0;
    if (value > 20) return 20;
    return static_cast<unsigned char>(value);
}
static void SendHpMpAlertFromStatusBar() {
    DWORD statusBar = 0;
    if (!TryReadDword(kUIStatusBarPtr, statusBar) || statusBar == 0) {
        return;
    }
    DWORD hpRaw = 0;
    DWORD mpRaw = 0;
    if (!TryReadDword(statusBar + kHpAlertOffset, hpRaw)) {
        return;
    }
    if (!TryReadDword(statusBar + kMpAlertOffset, mpRaw)) {
        return;
    }
    const unsigned char hpAlert = ClampAlert(static_cast<int>(hpRaw));
    const unsigned char mpAlert = ClampAlert(static_cast<int>(mpRaw));
    DWORD socketPtr = 0;
    if (!TryReadDword(kClientSocketPtr, socketPtr) || socketPtr == 0) {
        return;
    }
    unsigned char payload[4] = {
        static_cast<unsigned char>(kOpcodeSetHpMpAlert & 0xFF),
        static_cast<unsigned char>((kOpcodeSetHpMpAlert >> 8) & 0xFF),
        hpAlert,
        mpAlert
    };
    COutPacket packet{};
    packet.Loopback = 0;
    packet.Data = payload;
    packet.Size = sizeof(payload);
    packet.Offset = 0;
    packet.EncryptedByShanda = 0;
    g_SendPacket(reinterpret_cast<void*>(socketPtr), nullptr, &packet);
}
static void ApplyHpMpAlertToStatusBar(unsigned char hpAlert, unsigned char mpAlert) {
    DWORD statusBar = 0;
    if (!TryReadDword(kUIStatusBarPtr, statusBar) || statusBar == 0) {
        return;
    }
    Memory::WriteInt(statusBar + kHpAlertOffset, hpAlert);
    Memory::WriteInt(statusBar + kMpAlertOffset, mpAlert);
}
static void HandleHpMpAlertPacket(CInPacket* packet) {
    if (packet == nullptr) {
        return;
    }
    __try {
        if (packet->Data == nullptr || packet->Size < 8) {
            return;
        }
        const unsigned char* data = reinterpret_cast<const unsigned char*>(packet->Data);
        const unsigned short opcode = *reinterpret_cast<const unsigned short*>(data + 4);
        if (opcode != kOpcodeSetHpMpAlert) {
            return;
        }
        // First two bytes are HP/MP alert thresholds; append more settings after if needed.
        const unsigned char hpAlert = ClampAlert(static_cast<int>(data[6]));
        const unsigned char mpAlert = ClampAlert(static_cast<int>(data[7]));
        ApplyHpMpAlertToStatusBar(hpAlert, mpAlert);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}
using SaveGlobal_t = void(__fastcall*)(void* pThis, void* edx);
static SaveGlobal_t s_SaveGlobal = reinterpret_cast<SaveGlobal_t>(kSaveGlobalAddr);
static void __fastcall SaveGlobal_Hook(void* pThis, void* edx) {
    s_SaveGlobal(pThis, edx);
    SendHpMpAlertFromStatusBar();
}
using ProcessPacket_t = void(__fastcall*)(void* pThis, void* edx, CInPacket* packet);
static ProcessPacket_t s_ProcessPacket = reinterpret_cast<ProcessPacket_t>(kProcessPacketAddr);
static void __fastcall ProcessPacket_Hook(void* pThis, void* edx, CInPacket* packet) {
    if (packet->Data != nullptr && packet->Size >= 6) {
        const auto* data = static_cast<const unsigned char*>(packet->Data);
        if (*reinterpret_cast<const WORD*>(data + 4) == kOpcodeStorageSlotLimit) {
            if (packet->Size >= 7) {
                ApplyStorageSlotLimit(data[6]);
            }
            return; // Do not pass our configuration opcode to the native dispatcher.
        }
    }
    HandleHpMpAlertPacket(packet);
    s_ProcessPacket(pThis, edx, packet);
}
} // namespace
void HookSaveGlobal(bool enable) {
    Memory::SetHook(enable, reinterpret_cast<void**>(&s_SaveGlobal), SaveGlobal_Hook);
}
void HookHpMpAlertRecv(bool enable) {
    ApplyStorageSlotLimit(48);
    // Validate successful responses against the supported ceiling, not a lowered
    // purchase limit. The original new-count > old-count check stays intact.
    Memory::WriteByte(0x0047AB5D, enable ? kMaxStorageSlots : 48);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Memory::SetHook(enable, reinterpret_cast<void**>(&s_ProcessPacket), ProcessPacket_Hook);
}
