// trainer.h
#pragma once

#include "TrainerBase.h"

class Trainer : public TrainerBase
{
public:
    Trainer() : TrainerBase(L"mio.exe") {} // x64
    ~Trainer() override = default;

    static inline const wchar_t *moduleName = L"mio.exe";

    // Infinite Health: force the damage branch at mio.exe+0x9FA61C.
    inline bool infiniteHealth()
    {
        const std::vector<std::string> pat = {"0F", "85", "??", "??", "??", "??", "41", "F6", "46", "1C", "14", "0F", "84", "A5", "00", "00"};
        size_t patchOffset = 0;
        const std::vector<BYTE> newBytes = {0x90, 0xE9};
        return createBytePatch(moduleName, "InfiniteHealth", pat, patchOffset, newBytes);
    }

    // Unlimited Modifier Allocation: force GetUsedAllocation() (mio.exe+0x9E1C80) to return 0,
    // so the Allocation matrix (max = level*10+25, e.g. 75) is never full
    inline bool unlimitedModifierAllocation()
    {
        const std::vector<std::string> pat = {"41", "57", "41", "56", "41", "54", "56", "57", "53", "48", "83", "EC", "48", "48", "8B", "05", "??", "??", "??", "??", "48", "31", "E0", "48", "89", "44", "24", "40", "48", "B8", "07", "00", "00", "00", "01", "00", "00", "00"};
        size_t patchOffset = 0;
        const std::vector<BYTE> newBytes = {0x31, 0xC0, 0xC3};  // xor eax,eax ; ret  (used = 0)
        return createBytePatch(moduleName, "UnlimitedModifierAllocation", pat, patchOffset, newBytes);
    }

    // Infinite Jumps: keep the jump-used flag at [rcx+0x8D0] cleared.
    inline bool infiniteJumps()
    {
        const std::vector<std::string> pat = {"80", "B9", "D0", "08", "00", "00", "00", "75", "12", "80", "BE", "08", "14", "00", "00", "00"};
        size_t patternOffset = 0;
        size_t overwriteLen = 7;
        size_t codeSize = 0x100;

        auto buildFunc = [overwriteLen, codeSize](uintptr_t codeCaveAddr, uintptr_t hookAddr, const std::vector<BYTE> &originalBytes) -> std::vector<BYTE>
        {
            std::vector<BYTE> code(codeSize, 0x90);
            size_t wPos = 0;

            code[wPos++] = 0xC6;
            code[wPos++] = 0x81;
            code[wPos++] = 0xD0;
            code[wPos++] = 0x08;
            code[wPos++] = 0x00;
            code[wPos++] = 0x00;
            code[wPos++] = 0x00;

            std::memcpy(&code[wPos], originalBytes.data(), originalBytes.size());
            wPos += originalBytes.size();

            code[wPos++] = 0xE9;
            uintptr_t returnAddr = hookAddr + overwriteLen;
            uintptr_t nextInstrAddr = codeCaveAddr + wPos + 4;
            int32_t rel = static_cast<int32_t>(returnAddr - nextInstrAddr);
            std::memcpy(&code[wPos], &rel, 4);
            wPos += 4;

            return code;
        };

        return createNamedHook(moduleName, "InfiniteJumps", pat, patternOffset, overwriteLen, codeSize, buildFunc);
    }

    // Infinite Stamina: pin the static float at mio.exe+0x11119A8 to 100.0.
    inline bool infiniteStamina()
    {
        std::vector<unsigned int> offsets = {0x11119A8};
        return createPointerToggle(moduleName, "InfiniteStamina", offsets, 100.0f);
    }

    // Movement Speed Multiplier: both player position integrators do `pos += xmm6` where xmm6 is
    // this frame's move delta (velocity*dt). We hook each and insert `mulss xmm6,[mult]` first -
    // mulss scales only the LOW float (X), so horizontal speed is multiplied while fall/jump/
    // climb stay normal. The multiplier is a float embedded in each code cave; mult=1.0 = normal.
    inline bool movementSpeedMultiplier(float mult)
    {
        auto buildFunc = [mult](uintptr_t cave, uintptr_t hook, const std::vector<BYTE> &orig) -> std::vector<BYTE>
        {
            std::vector<BYTE> code = {0xF3, 0x0F, 0x59, 0x35, 0, 0, 0, 0};    // mulss xmm6,[rip+disp]
            code.insert(code.end(), orig.begin(), orig.end());                // original movsd + addps
            size_t jmpPos = code.size();
            code.insert(code.end(), {0xE9, 0, 0, 0, 0});                      // jmp back
            size_t floatPos = code.size();
            BYTE fb[4];
            std::memcpy(fb, &mult, sizeof(float));
            code.insert(code.end(), fb, fb + 4);                             // embedded multiplier
            int32_t disp = static_cast<int32_t>(floatPos - 8);               // rip (after mulss) = cave+8
            std::memcpy(&code[4], &disp, 4);
            int32_t rel = static_cast<int32_t>((hook + orig.size()) - (cave + jmpPos + 5));
            std::memcpy(&code[jmpPos + 1], &rel, 4);
            return code;
        };
        // integrator A: movsd xmm0,[rbx+18]; addps xmm0,xmm6; movlps [rbx+18],xmm0; ... (extended for uniqueness)
        const std::vector<std::string> pat1 = {"F2", "0F", "10", "43", "18", "0F", "58", "C6", "0F", "13", "43", "18", "0F", "57", "C0", "F3", "0F", "58", "43", "20", "F3", "0F", "11", "43", "20", "48", "8B", "9C", "24", "F8", "04", "00", "00"};
        // integrator B: movsd xmm0,[rsi]; addps xmm6,xmm0; movlps [rsi],xmm6
        const std::vector<std::string> pat2 = {"F2", "0F", "10", "06", "0F", "58", "F0", "0F", "13", "36"};
        bool ok1 = createNamedHook(moduleName, "MovementSpeedMultiplierH1", pat1, 0, 8, 0x40, buildFunc);
        bool ok2 = createNamedHook(moduleName, "MovementSpeedMultiplierH2", pat2, 0, 7, 0x40, buildFunc);
        return ok1 && ok2;
    }

    inline bool setNacreDroplets(int newVal)
    {
        std::vector<unsigned int> offsets = {0x1116BF8, 0x754};
        return WriteToDynamicAddress<int>(moduleName, offsets, newVal);
    }

    inline bool setCrystallizedNacre(int newVal)
    {
        std::vector<unsigned int> offsets = {0x1116BF8, 0xDD24};
        return WriteToDynamicAddress<int>(moduleName, offsets, newVal);
    }

    inline bool setOldCore(int newVal)
    {
        std::vector<unsigned int> offsets = {0x1116BF8, 0x15F14};
        return WriteToDynamicAddress<int>(moduleName, offsets, newVal);
    }
};
