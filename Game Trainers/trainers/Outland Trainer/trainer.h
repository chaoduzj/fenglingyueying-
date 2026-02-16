// trainer.h
#pragma once

#include "TrainerBase.h"

class Trainer : public TrainerBase
{
public:
    Trainer() : TrainerBase(L"Outland.exe") {} // x86
    ~Trainer() override = default;

    // Constants
    static inline const wchar_t *moduleName = L"Outland.exe";

    inline bool setHealth(float newVal)
    {
        std::vector<unsigned int> offsets = {0x005024F0, 0x5C, 0x14, 0x0, 0x14, 0x18, 0x3C};
        return createPointerToggle(moduleName, "SetHealth", offsets, newVal);
    }

    inline bool setMaxHealth(float newVal)
    {
        std::vector<unsigned int> offsets = {0x005024F0, 0x5C, 0x14, 0x0, 0x14, 0x18, 0x40};
        return createPointerToggle(moduleName, "SetMaxHealth", offsets, newVal);
    }

    inline bool setCoins(int newVal)
    {
        std::vector<unsigned int> offsets = {0x004C72A0, 0x4C, 0x44, 0x0, 0x4, 0x14, 0x0, 0xF8};
        return createPointerToggle(moduleName, "SetCoins", offsets, newVal);
    }
};
