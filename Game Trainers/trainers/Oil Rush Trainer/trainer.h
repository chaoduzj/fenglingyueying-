// trainer.h
#pragma once

#include "TrainerBase.h"

class Trainer : public TrainerBase
{
public:
    Trainer() : TrainerBase(L"OilRush.exe") {} // x86
    ~Trainer() override = default;

    // Constants
    static inline const wchar_t *moduleName = L"Unigine_x86.dll";

    uintptr_t internal_OilFreeze_Address = 0;
    uintptr_t internal_UpgradeFreeze_Address = 0;

    // Internal state trackers
    int currentOilTarget = -1;
    int currentAbilityTarget = -1;

    // --- CLEAN UI WRAPPERS ---
    inline bool toggleOil(bool active, int value)
    {
        currentOilTarget = active ? value : -1;
        return setMasterResources(currentOilTarget, currentAbilityTarget);
    }

    inline bool toggleAbility(bool active, int value)
    {
        currentAbilityTarget = active ? value : -1;
        return setMasterResources(currentOilTarget, currentAbilityTarget);
    }

private:
    inline bool setMasterResources(int targetOil, int targetAbilityPoints)
    {
        // 1. If both are -1, safely kill the hook and exit.
        if (targetOil == -1 && targetAbilityPoints == -1)
        {
            disableNamedHook("MasterResourceHook");
            internal_OilFreeze_Address = 0;
            internal_UpgradeFreeze_Address = 0;
            return true;
        }

        // 2. If the hook is active, just update the live memory targets!
        auto it = hooks.find("MasterResourceHook");
        if (it != hooks.end() && it->second.active)
        {
            if (internal_OilFreeze_Address != 0)
                WriteProcessMemory(hProcess, (LPVOID)internal_OilFreeze_Address, &targetOil, sizeof(int), nullptr);

            if (internal_UpgradeFreeze_Address != 0)
                WriteProcessMemory(hProcess, (LPVOID)internal_UpgradeFreeze_Address, &targetAbilityPoints, sizeof(int), nullptr);

            return true;
        }

        // 3. Create the Master Hook.
        uintptr_t modBase = 0;
        size_t modSize = 0;
        if (!getModuleInfo(moduleName, modBase, modSize))
            return false;

        uintptr_t staticPtrBase = modBase + 0x00671B28;
        size_t overwriteLen = 6;
        size_t codeSize = 0x300;

        auto buildFunc = [this, staticPtrBase, targetOil, targetAbilityPoints](uintptr_t codeCaveAddr, uintptr_t hookAddr, const std::vector<BYTE> &originalBytes) -> std::vector<BYTE>
        {
            std::vector<BYTE> code;
            code.reserve(0x300);

            std::vector<size_t> oil_jump_offsets;
            std::vector<size_t> cleanup_jump_offsets;

            auto emit_je = [&](std::vector<size_t> &jump_list)
            {
                code.push_back(0x0F);
                code.push_back(0x84);
                jump_list.push_back(code.size());
                code.insert(code.end(), 4, 0x00);
            };
            auto emit_jne = [&](std::vector<size_t> &jump_list)
            {
                code.push_back(0x0F);
                code.push_back(0x85);
                jump_list.push_back(code.size());
                code.insert(code.end(), 4, 0x00);
            };

            // Read Master Value (EAX) & Save ebx
            code.push_back(0x8B);
            code.push_back(0x46);
            code.push_back(0x10);
            code.push_back(0x53);

            // ==========================================
            // BLOCK 1: OIL CHECK
            // ==========================================
            code.push_back(0x8B);
            code.push_back(0x1D);
            const BYTE *pStatic = reinterpret_cast<const BYTE *>(&staticPtrBase);
            code.insert(code.end(), pStatic, pStatic + 4);
            code.push_back(0x85);
            code.push_back(0xDB);
            emit_je(oil_jump_offsets);

            std::vector<uint32_t> oil_offsets = {0x0D8, 0x00C, 0x120, 0x77C, 0x6A0, 0x064};
            for (uint32_t off : oil_offsets)
            {
                if (off < 0x80)
                {
                    code.push_back(0x8B);
                    code.push_back(0x5B);
                    code.push_back(static_cast<BYTE>(off));
                }
                else
                {
                    code.push_back(0x8B);
                    code.push_back(0x9B);
                    code.insert(code.end(), reinterpret_cast<BYTE *>(&off), reinterpret_cast<BYTE *>(&off) + 4);
                }
                code.push_back(0x85);
                code.push_back(0xDB);
                emit_je(oil_jump_offsets);
            }
            uint32_t oilFinalOff = 0xCCC;
            code.push_back(0x8B);
            code.push_back(0x9B);
            code.insert(code.end(), reinterpret_cast<BYTE *>(&oilFinalOff), reinterpret_cast<BYTE *>(&oilFinalOff) + 4);

            code.push_back(0x39);
            code.push_back(0xD8);
            emit_jne(oil_jump_offsets);
            code.push_back(0x83);
            code.push_back(0x7E);
            code.push_back(0xE0);
            code.push_back(0x00);
            emit_jne(oil_jump_offsets);

            // --- FIXED HACK OIL ---
            // mov ebx, ds:[OilTarget]
            code.push_back(0x8B);
            code.push_back(0x1D);
            size_t oil_var_inst_offset = code.size();
            code.insert(code.end(), 4, 0x00);

            // cmp ebx, -1 (Safeguard: If -1, jump to Upgrade Check, leaving EAX perfectly intact!)
            code.push_back(0x83);
            code.push_back(0xFB);
            code.push_back(0xFF);
            emit_je(oil_jump_offsets);

            // mov [esi+10], ebx (Force Hack)
            code.push_back(0x89);
            code.push_back(0x5E);
            code.push_back(0x10);
            // mov eax, ebx (Update EAX so the UI mirrors the hack)
            code.push_back(0x8B);
            code.push_back(0xC3);

            code.push_back(0xE9);
            cleanup_jump_offsets.push_back(code.size());
            code.insert(code.end(), 4, 0x00);

            // ==========================================
            // BLOCK 2: UPGRADE POINTS CHECK
            // ==========================================
            size_t check_upgrades_pos = code.size();

            code.push_back(0x8B);
            code.push_back(0x1D);
            code.insert(code.end(), pStatic, pStatic + 4);
            code.push_back(0x85);
            code.push_back(0xDB);
            emit_je(cleanup_jump_offsets);

            std::vector<uint32_t> up_offsets = {0x844, 0x0A4};
            for (uint32_t off : up_offsets)
            {
                code.push_back(0x8B);
                code.push_back(0x9B);
                code.insert(code.end(), reinterpret_cast<BYTE *>(&off), reinterpret_cast<BYTE *>(&off) + 4);
                code.push_back(0x85);
                code.push_back(0xDB);
                emit_je(cleanup_jump_offsets);
            }
            uint32_t upFinalOff = 0x620;
            code.push_back(0x8B);
            code.push_back(0x9B);
            code.insert(code.end(), reinterpret_cast<BYTE *>(&upFinalOff), reinterpret_cast<BYTE *>(&upFinalOff) + 4);

            code.push_back(0x39);
            code.push_back(0xD8);
            emit_jne(cleanup_jump_offsets);
            code.push_back(0x83);
            code.push_back(0x7E);
            code.push_back(0xE0);
            code.push_back(0x00);
            emit_jne(cleanup_jump_offsets);

            // --- FIXED HACK UPGRADE ---
            // mov ebx, ds:[UpTarget]
            code.push_back(0x8B);
            code.push_back(0x1D);
            size_t up_var_inst_offset = code.size();
            code.insert(code.end(), 4, 0x00);

            // cmp ebx, -1 (Safeguard)
            code.push_back(0x83);
            code.push_back(0xFB);
            code.push_back(0xFF);
            emit_je(cleanup_jump_offsets);

            // mov [esi+10], ebx
            code.push_back(0x89);
            code.push_back(0x5E);
            code.push_back(0x10);
            // mov eax, ebx
            code.push_back(0x8B);
            code.push_back(0xC3);

            // ==========================================
            // CLEANUP & DATA SECTION
            // ==========================================
            size_t cleanup_pos = code.size();
            code.push_back(0x5B); // pop ebx
            code.push_back(0x89);
            code.push_back(0x47);
            code.push_back(0x10);

            code.push_back(0xE9);
            size_t jmp_offset = code.size();
            code.insert(code.end(), 4, 0x00);

            size_t oil_data_pos = code.size();
            const BYTE *pOilVal = reinterpret_cast<const BYTE *>(&targetOil);
            code.insert(code.end(), pOilVal, pOilVal + 4);

            size_t up_data_pos = code.size();
            const BYTE *pUpVal = reinterpret_cast<const BYTE *>(&targetAbilityPoints);
            code.insert(code.end(), pUpVal, pUpVal + 4);

            this->internal_OilFreeze_Address = static_cast<uint32_t>(codeCaveAddr + oil_data_pos);
            this->internal_UpgradeFreeze_Address = static_cast<uint32_t>(codeCaveAddr + up_data_pos);

            std::memcpy(&code[oil_var_inst_offset], &this->internal_OilFreeze_Address, 4);
            std::memcpy(&code[up_var_inst_offset], &this->internal_UpgradeFreeze_Address, 4);

            uintptr_t currentPos = codeCaveAddr + jmp_offset + 4;
            int32_t relJump = static_cast<int32_t>((hookAddr + 6) - currentPos);
            std::memcpy(&code[jmp_offset], &relJump, 4);

            for (size_t off : oil_jump_offsets)
            {
                int32_t rel = static_cast<int32_t>(check_upgrades_pos - (off + 4));
                std::memcpy(&code[off], &rel, 4);
            }

            for (size_t off : cleanup_jump_offsets)
            {
                int32_t rel = static_cast<int32_t>(cleanup_pos - (off + 4));
                std::memcpy(&code[off], &rel, 4);
            }

            return code;
        };

        return createNamedHookByOffset(moduleName, "MasterResourceHook", 0x90336, overwriteLen, codeSize, buildFunc);
    }
};
