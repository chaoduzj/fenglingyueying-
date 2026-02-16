// trainer.h
#pragma once

#include "TrainerBase.h"

class Trainer : public TrainerBase
{
public:
    Trainer() : TrainerBase(L"Inotia4.exe") {} // x64
    ~Trainer() override = default;

    // Constants
    static inline const wchar_t *moduleName = L"Inotia4_port-Native.dll";

    inline bool freezeHealth()
    {
        std::vector<unsigned int> offsets = {0x003E4D38, 0x1F0};
        return createPointerToggle(moduleName, "FreezeHealth", offsets, nullptr, (int *)nullptr);
    }

    inline bool freezeMana()
    {
        std::vector<unsigned int> offsets = {0x003E4D38, 0x1F4};
        return createPointerToggle(moduleName, "FreezeMana", offsets, nullptr, (int *)nullptr);
    }

    inline bool setExp(int newVal)
    {
        std::vector<unsigned int> offsets = {0x003E4D38, 0x318};
        uintptr_t targetAddress = resolveModuleDynamicAddress(moduleName, offsets);

        // The pattern for: imul eax,edx,9E3779B9
        const std::vector<std::string> pat = {"69", "C2", "B9", "79", "37", "9E"};
        size_t patternOffset = 0;
        size_t overwriteLen = 6;
        size_t codeSize = 0x100;

        auto buildFunc = [targetAddress, newVal](uintptr_t codeCaveAddr, uintptr_t hookAddr, const std::vector<BYTE> &originalBytes) -> std::vector<BYTE>
        {
            std::vector<BYTE> code;
            code.reserve(0x100);

            // --- HACK LOGIC ---

            // 1. Load the Target Address into RAX (using 64-bit immediate move)
            //    Opcode: 48 B8 + 8 bytes of address
            code.push_back(0x48);
            code.push_back(0xB8);
            const BYTE *pAddr = reinterpret_cast<const BYTE *>(&targetAddress);
            code.insert(code.end(), pAddr, pAddr + 8);

            // 2. Compare RCX (Current Address) with RAX (Target Address)
            //    Opcode: 48 39 C1 (CMP RCX, RAX)
            code.push_back(0x48);
            code.push_back(0x39);
            code.push_back(0xC1);

            // 3. JNE (Jump if Not Equal) to Original Code
            //    Opcode: 75 <offset>
            //    We skip the next instruction (MOV EDX, hackValue).
            //    MOV EDX, imm32 is 5 bytes (B8 + 4 bytes).
            code.push_back(0x75);
            code.push_back(0x05); // Jump 5 bytes forward

            // 4. Move Hack Value into EDX
            //    Opcode: BA + 4 bytes of value (MOV EDX, imm32)
            code.push_back(0xBA); // 0xBA is MOV EDX, imm32
            const BYTE *pVal = reinterpret_cast<const BYTE *>(&newVal);
            code.insert(code.end(), pVal, pVal + 4);

            // --- ORIGINAL CODE RESTORATION ---

            // 5. Execute the overwritten original instruction
            //    imul eax,edx,9E3779B9
            code.insert(code.end(), originalBytes.begin(), originalBytes.end());

            // 6. Jump back to the game (JMP rel32)
            code.push_back(0xE9);
            uintptr_t returnAddr = hookAddr + 6;                   // overwriteLen
            uintptr_t currentPos = codeCaveAddr + code.size() + 4; // +4 for the displacement bytes
            int32_t relJump = static_cast<int32_t>(returnAddr - currentPos);
            const BYTE *pJump = reinterpret_cast<const BYTE *>(&relJump);
            code.insert(code.end(), pJump, pJump + 4);

            return code;
        };

        return createNamedHook(moduleName, "SetExp", pat, patternOffset, overwriteLen, codeSize, buildFunc);
    }

    inline bool setCoins(int newVal)
    {
        std::vector<unsigned int> offsets = {0x411618};
        return WriteToDynamicAddress(moduleName, offsets, newVal);
    }

    inline bool setStatPoints(int newVal)
    {
        std::vector<unsigned int> offsets = {0x003E4D38, 0x340};
        uintptr_t targetAddress = resolveModuleDynamicAddress(moduleName, offsets);

        // The pattern for: imul eax,edx,9E3779B9
        const std::vector<std::string> pat = {"69", "C2", "B9", "79", "37", "9E"};
        size_t patternOffset = 0;
        size_t overwriteLen = 6;
        size_t codeSize = 0x100;

        auto buildFunc = [targetAddress, newVal](uintptr_t codeCaveAddr, uintptr_t hookAddr, const std::vector<BYTE> &originalBytes) -> std::vector<BYTE>
        {
            std::vector<BYTE> code;
            code.reserve(0x100);

            code.push_back(0x48);
            code.push_back(0xB8);
            const BYTE *pAddr = reinterpret_cast<const BYTE *>(&targetAddress);
            code.insert(code.end(), pAddr, pAddr + 8);

            code.push_back(0x48);
            code.push_back(0x39);
            code.push_back(0xC1);

            code.push_back(0x75);
            code.push_back(0x05);

            code.push_back(0xBA);
            const BYTE *pVal = reinterpret_cast<const BYTE *>(&newVal);
            code.insert(code.end(), pVal, pVal + 4);

            code.insert(code.end(), originalBytes.begin(), originalBytes.end());

            code.push_back(0xE9);
            uintptr_t returnAddr = hookAddr + 6;
            uintptr_t currentPos = codeCaveAddr + code.size() + 4;
            int32_t relJump = static_cast<int32_t>(returnAddr - currentPos);
            const BYTE *pJump = reinterpret_cast<const BYTE *>(&relJump);
            code.insert(code.end(), pJump, pJump + 4);

            return code;
        };

        return createNamedHook(moduleName, "SetStatPoints", pat, patternOffset, overwriteLen, codeSize, buildFunc);
    }

    inline bool setSkillPoints(int newVal)
    {
        std::vector<unsigned int> offsets = {0x003E4D38, 0x330};
        uintptr_t targetAddress = resolveModuleDynamicAddress(moduleName, offsets);

        // The pattern for: imul eax,edx,9E3779B9
        const std::vector<std::string> pat = {"69", "C2", "B9", "79", "37", "9E"};
        size_t patternOffset = 0;
        size_t overwriteLen = 6;
        size_t codeSize = 0x100;

        auto buildFunc = [targetAddress, newVal](uintptr_t codeCaveAddr, uintptr_t hookAddr, const std::vector<BYTE> &originalBytes) -> std::vector<BYTE>
        {
            std::vector<BYTE> code;
            code.reserve(0x100);

            code.push_back(0x48);
            code.push_back(0xB8);
            const BYTE *pAddr = reinterpret_cast<const BYTE *>(&targetAddress);
            code.insert(code.end(), pAddr, pAddr + 8);

            code.push_back(0x48);
            code.push_back(0x39);
            code.push_back(0xC1);

            code.push_back(0x75);
            code.push_back(0x05);

            code.push_back(0xBA);
            const BYTE *pVal = reinterpret_cast<const BYTE *>(&newVal);
            code.insert(code.end(), pVal, pVal + 4);

            code.insert(code.end(), originalBytes.begin(), originalBytes.end());

            code.push_back(0xE9);
            uintptr_t returnAddr = hookAddr + 6;
            uintptr_t currentPos = codeCaveAddr + code.size() + 4;
            int32_t relJump = static_cast<int32_t>(returnAddr - currentPos);
            const BYTE *pJump = reinterpret_cast<const BYTE *>(&relJump);
            code.insert(code.end(), pJump, pJump + 4);

            return code;
        };

        return createNamedHook(moduleName, "SetSkillPoints", pat, patternOffset, overwriteLen, codeSize, buildFunc);
    }

    inline unsigned int getCurMouseItem()
    {
        std::vector<unsigned int> offsets = {0x38A470};
        return ReadFromDynamicAddress<unsigned int>(moduleName, offsets);
    }

    inline unsigned int getSlot1Item()
    {
        std::vector<unsigned int> offsets = {0x411620};
        return ReadFromDynamicAddress<unsigned int>(moduleName, offsets);
    }

    inline bool setSlot1Item(int itemAddress)
    {
        std::vector<unsigned int> offsets = {0x411620};
        return WriteToDynamicAddress(moduleName, offsets, itemAddress);
    }

    inline bool unlockAllMercenarySlots()
    {
        std::vector<unsigned int> offsets = {0x003E4F40, 0x8};
        uintptr_t startAddr = resolveModuleDynamicAddress(moduleName, offsets);

        if (startAddr == 0)
        {
            std::cerr << "[!] Could not resolve start address." << std::endl;
            return false;
        }

        // Each unlocked slot has pattern "92 255 255 255 255"
        BYTE pattern[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
        size_t patternLen = sizeof(pattern);
        std::vector<BYTE> buffer(4096);
        SIZE_T bytesRead;

        bool allPatched = true;

        if (ReadProcessMemory(hProcess, (LPCVOID)startAddr, buffer.data(), buffer.size(), &bytesRead))
        {
            for (size_t i = 1; i <= bytesRead - patternLen; i++)
            {
                if (memcmp(&buffer[i], pattern, patternLen) == 0)
                {
                    uintptr_t targetAddr = startAddr + i - 1;
                    BYTE val = 92;

                    if (!WriteProcessMemory(hProcess, (LPVOID)targetAddr, &val, sizeof(BYTE), nullptr))
                    {
                        allPatched = false;
                    }
                }
            }
        }
        else
        {
            std::cerr << "[!] RPM failed. Error: " << GetLastError() << std::endl;
            return false;
        }

        return allPatched;
    }
};
