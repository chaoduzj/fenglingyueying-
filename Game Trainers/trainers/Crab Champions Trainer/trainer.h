#pragma once

#include "UEBase.h"

#include <string>

struct ToggleFloatArgument
{
    int isEnabled;
    float value;
};

struct ToggleIntArgument
{
    int isEnabled;
    int value;
};

struct WeaponAbilityMeleeRankArgument
{
    int itemId;
    int rank;
    int applyToAll;
};

struct WeaponAbilityMeleeListBuffer
{
    char data[8192];
};

struct RankListBuffer
{
    char data[1024];
};

class Trainer : public UEBase
{
public:
    Trainer() : UEBase(L"CrabChampions-Win64-Shipping.exe") {}
    ~Trainer() override = default;

    bool setCrystals(int value)
    {
        return invokeMethod("SetCrystals", value);
    }

    bool setKeys(int value)
    {
        return invokeMethod("SetKeys", value);
    }

    bool setHealth(bool isEnabled, int value)
    {
        return invokeMethod("SetHealth", ToggleIntArgument{isEnabled ? 1 : 0, value});
    }

    bool setMaxHealth(int value)
    {
        return invokeMethod("SetMaxHealth", value);
    }

    bool setGodMode(bool isEnabled)
    {
        return invokeMethod("SetGodMode", isEnabled ? 1 : 0);
    }

    bool setMovementSpeedMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetMovementSpeedMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setWeaponDamageMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetWeaponDamageMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setAbilityDamageMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetAbilityDamageMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setMeleeDamageMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetMeleeDamageMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setCriticalHitChanceMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetCriticalHitChanceMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setCriticalHitDamageMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetCriticalHitDamageMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setFiringRateMultiplier(bool isEnabled, float value)
    {
        return invokeMethod("SetFiringRateMultiplier", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setReloadTime(bool isEnabled, float value)
    {
        return invokeMethod("SetReloadTime", ToggleFloatArgument{isEnabled ? 1 : 0, value});
    }

    bool setAmmo(bool isEnabled, int value)
    {
        return invokeMethod("SetAmmo", ToggleIntArgument{isEnabled ? 1 : 0, value});
    }

    bool setNoAbilityMeleeCooldown(bool isEnabled)
    {
        return invokeMethod("SetNoAbilityMeleeCooldown", isEnabled ? 1 : 0);
    }

    std::string getWeaponAbilityMeleeList()
    {
        WeaponAbilityMeleeListBuffer buffer{};
        if (!invokeMethodReadBack("GetWeaponAbilityMeleeList", buffer))
            return {};
        return buffer.data;
    }

    std::string getRankList()
    {
        RankListBuffer buffer{};
        if (!invokeMethodReadBack("GetRankList", buffer))
            return {};
        return buffer.data;
    }

    bool setWeaponAbilityMeleeRank(int itemId, int rank, bool applyToAll)
    {
        return invokeMethod("SetWeaponAbilityMeleeRank", WeaponAbilityMeleeRankArgument{itemId, rank, applyToAll ? 1 : 0});
    }

    bool unlockAllWeapons()
    {
        return invokeMethod("UnlockAllWeapons", 0);
    }

    bool unlockAllMelee()
    {
        return invokeMethod("UnlockAllMelee", 0);
    }

    bool unlockAllAbilities()
    {
        return invokeMethod("UnlockAllAbilities", 0);
    }

    bool unlockAllCosmetics()
    {
        return invokeMethod("UnlockAllCosmetics", 0);
    }
};
