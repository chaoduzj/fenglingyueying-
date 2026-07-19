#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <MinHook.h>

#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/CrabChampions_classes.hpp"

namespace
{
    // Bridge request ABI
    enum class RequestType
    {
        None,
        SetCrystals,
        SetKeys,
        SetHealth,
        SetMaxHealth,
        SetGodMode,
        UnlockAllWeapons,
        UnlockAllMelee,
        UnlockAllAbilities,
        UnlockAllCosmetics,
        SetMovementSpeedMultiplier,
        SetWeaponDamageMultiplier,
        SetAbilityDamageMultiplier,
        SetMeleeDamageMultiplier,
        SetCriticalHitChanceMultiplier,
        SetCriticalHitDamageMultiplier,
        SetFiringRateMultiplier,
        SetReloadTime,
        SetAmmo,
        SetNoAbilityMeleeCooldown,
        GetWeaponAbilityMeleeList,
        GetRankList,
        SetWeaponAbilityMeleeRank,
    };

    enum class RequestState
    {
        Idle,
        Pending,
        Processing,
        Complete,
    };

    struct Request
    {
        RequestType type = RequestType::None;
        int value = 0;
        int secondaryValue = 0;
        float floatValue = 0.0f;
        bool isEnabled = false;
        bool applyToAll = false;
        char *output = nullptr;
        std::size_t outputCapacity = 0;
        bool result = false;
        RequestState state = RequestState::Idle;
    };

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

    struct MovementLayerState
    {
        bool isEnabled = false;
        float multiplier = 1.0f;
        SDK::ACrabPlayerC *characterTarget = nullptr;
        SDK::UCharacterMovementComponent *movementTarget = nullptr;
        float gameBaseWalkSpeed = 0.0f;
        float gameWalkSpeed = 0.0f;
        float gameCrouchedSpeed = 0.0f;
        float lastAppliedBaseWalkSpeed = 0.0f;
        float lastAppliedWalkSpeed = 0.0f;
        float lastAppliedCrouchedSpeed = 0.0f;
        bool hasAppliedValue = false;
    };

    struct GodModeState
    {
        bool isEnabled = false;
        bool isClearing = false;
        int clearStablePasses = 0;
    };

    struct FireRateLayerState
    {
        float gameFireRate = 0.0f;
        float gameFireInterval = 0.0f;
        float gameBurstInterval = 0.0f;
        float lastAppliedFireRate = 0.0f;
        float lastAppliedFireInterval = 0.0f;
        float lastAppliedBurstInterval = 0.0f;
        bool hasAppliedValue = false;
    };

    struct ValueOverrideState
    {
        float gameValue = 0.0f;
        float lastAppliedValue = 0.0f;
        bool hasAppliedValue = false;
    };

    struct AmmoOverrideState
    {
        int gameClipSize = 0;
        int lastAppliedClipSize = 0;
        bool hasAppliedValue = false;
    };

    struct AmmoRefreshState
    {
        SDK::UCrabWeaponDA *weaponData = nullptr;
        int clipSize = 0;
    };

    struct PlayerContext
    {
        SDK::UCrabGI *gameInstance = nullptr;
        SDK::APlayerController *controller = nullptr;
        SDK::ACrabPS *playerState = nullptr;
        SDK::ACrabPlayerC *character = nullptr;
        SDK::UCrabHC *healthComponent = nullptr;
    };

    using ProcessEventFn = void(__fastcall *)(const SDK::UObject *, SDK::UFunction *, void *);
    using PlayerStatGetterFn = float(__fastcall *)(SDK::ACrabPS *);
    using ControllerStatGetterFn = float(__fastcall *)(SDK::APlayerController *);
    using FinalizeDamageInfoFn = void(__fastcall *)(
        SDK::ACrabPS *,
        SDK::FCrabDamageInfo *);
    using SetInvulnerableFn = void(__fastcall *)(SDK::ACrabC *, bool);
    using MeleeInputFn = void(__fastcall *)(SDK::ACrabPlayerC *);

    // Game-specific native offsets
    // Crab Champions Steam build 20903826 (UE 4.27). These native routines
    // supply final stat values, finalize outgoing damage, and control the
    // remaining features which cannot be implemented through reflected APIs.
    constexpr std::uintptr_t kWeaponDamageGetterRva = 0x00D25C30;
    constexpr std::uintptr_t kAbilityDamageGetterRva = 0x00D225A0;
    constexpr std::uintptr_t kMeleeDamageGetterRva = 0x00D14F30;
    constexpr std::uintptr_t kCriticalHitChanceGetterRva = 0x00D22F20;
    constexpr std::uintptr_t kCriticalHitDamageGetterRva = 0x00D23030;
    constexpr std::uintptr_t kFinalizeDamageInfoRva = 0x00D21480;
    constexpr std::uintptr_t kAbilityDamageStatsReturnRva = 0x00D5D7AC;
    constexpr std::uintptr_t kSetInvulnerableRva = 0x00CBD7A0;
    constexpr std::uintptr_t kMeleeInputRva = 0x00D15980;
    // ACrabWeapon::GetFireRate reads the first private runtime value, which is
    // why changing only it updates the Attributes panel. Native firing waits
    // on the two following reciprocal intervals: the normal shot interval and
    // the intra-burst interval. Perks and ranks are already folded into all
    // three values before they reach the live weapon actor.
    constexpr std::size_t kCurrentWeaponFireRateOffset = 0x02C0;
    constexpr std::size_t kCurrentWeaponFireIntervalOffset = 0x02C4;
    constexpr std::size_t kCurrentWeaponBurstIntervalOffset = 0x02C8;
    // Native melee input checks this private ACrabPlayerC float before starting
    // an attack, then repopulates it from the data asset and perk modifiers.
    constexpr std::size_t kCurrentMeleeCooldownOffset = 0x0A28;
    constexpr ULONGLONG kMeleeReleaseGapMilliseconds = 120;
    constexpr int kGodModeClearStablePasses = 30;

    // Hook and request state
    std::atomic_bool g_hookReady = false;
    ProcessEventFn g_originalProcessEvent = nullptr;
    PlayerStatGetterFn g_originalWeaponDamageGetter = nullptr;
    PlayerStatGetterFn g_originalAbilityDamageGetter = nullptr;
    ControllerStatGetterFn g_originalMeleeDamageGetter = nullptr;
    PlayerStatGetterFn g_originalCriticalHitChanceGetter = nullptr;
    PlayerStatGetterFn g_originalCriticalHitDamageGetter = nullptr;
    FinalizeDamageInfoFn g_originalFinalizeDamageInfo = nullptr;
    SetInvulnerableFn g_setInvulnerable = nullptr;
    MeleeInputFn g_originalMeleeInput = nullptr;
    std::atomic_bool g_isWeaponDamageEnabled = false;
    std::atomic<float> g_weaponDamageMultiplier = 1.0f;
    std::atomic_bool g_isAbilityDamageEnabled = false;
    std::atomic<float> g_abilityDamageMultiplier = 1.0f;
    std::atomic_bool g_isMeleeDamageEnabled = false;
    std::atomic<float> g_meleeDamageMultiplier = 1.0f;
    std::atomic_bool g_isCriticalHitChanceEnabled = false;
    std::atomic<float> g_criticalHitChanceMultiplier = 1.0f;
    std::atomic_bool g_isCriticalHitDamageEnabled = false;
    std::atomic<float> g_criticalHitDamageMultiplier = 1.0f;
    std::mutex g_requestMutex;
    std::condition_variable g_requestCondition;
    Request g_request;
    thread_local bool g_isProcessingRequest = false;

    // Active session cheats
    std::atomic_bool g_isHealthLockEnabled = false;
    std::atomic<int> g_lockedHealth = 0;
    std::atomic<ULONGLONG> g_lastSessionMaintenance = 0;
    std::atomic_bool g_areCosmeticsUnlocked = false;
    std::recursive_mutex g_sessionCheatMutex;
    GodModeState g_godModeState;
    MovementLayerState g_movementSpeedLayer;
    bool g_isFiringRateEnabled = false;
    float g_firingRateMultiplier = 1.0f;
    std::unordered_map<SDK::ACrabWeapon *, FireRateLayerState> g_fireRateLayers;
    bool g_isReloadTimeEnabled = false;
    float g_reloadTimeValue = 0.0f;
    std::unordered_map<SDK::UCrabWeaponDA *, ValueOverrideState> g_reloadTimeOverrides;
    bool g_isAmmoEnabled = false;
    int g_ammoValue = 0;
    std::unordered_map<SDK::UCrabWeaponDA *, AmmoOverrideState> g_ammoOverrides;
    std::unordered_map<SDK::ACrabWeapon *, AmmoRefreshState> g_ammoRefreshStates;
    bool g_isNoAbilityMeleeCooldownEnabled = false;
    std::unordered_map<SDK::UCrabAbilityDA *, ValueOverrideState> g_abilityCooldownOverrides;
    SDK::ACrabPlayerC *g_lastMeleeInputCharacter = nullptr;
    bool g_meleeInputLatched = false;
    ULONGLONG g_lastMeleeInputAt = 0;

    // Unreal context and common validation
    SDK::UWorld *GetWorld()
    {
        const std::uintptr_t imageBase =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!imageBase)
            return nullptr;

        return *reinterpret_cast<SDK::UWorld **>(imageBase + SDK::Offsets::GWorld);
    }

    PlayerContext ResolvePlayerContext()
    {
        PlayerContext context{};
        SDK::UWorld *world = GetWorld();
        if (!world || !world->OwningGameInstance)
            return {};

        SDK::UGameInstance *baseGameInstance = world->OwningGameInstance;
        if (baseGameInstance->IsA(SDK::UCrabGI::StaticClass()))
            context.gameInstance = static_cast<SDK::UCrabGI *>(baseGameInstance);

        if (baseGameInstance->LocalPlayers.Num() <= 0 || !baseGameInstance->LocalPlayers[0])
            return {};

        context.controller = baseGameInstance->LocalPlayers[0]->PlayerController;
        if (!context.controller || !context.controller->PlayerState)
            return {};

        if (!context.controller->PlayerState->IsA(SDK::ACrabPS::StaticClass()))
            return {};

        context.playerState = static_cast<SDK::ACrabPS *>(context.controller->PlayerState);

        SDK::APawn *pawn = context.controller->AcknowledgedPawn;
        if (pawn && pawn->IsA(SDK::ACrabPlayerC::StaticClass()))
        {
            context.character = static_cast<SDK::ACrabPlayerC *>(pawn);
            context.healthComponent = context.character->HC;
        }

        return context;
    }

    bool NearlyEqual(float left, float right)
    {
        const float scale = (std::max)(1.0f, (std::max)(std::fabs(left), std::fabs(right)));
        return std::fabs(left - right) <= 0.0001f * scale;
    }

    bool IsValidMultiplier(float value, bool allowZero = false)
    {
        return std::isfinite(value) && (allowZero ? value >= 0.0f : value > 0.0f);
    }

    // Layered session modifiers
    bool ApplyGodMode(const PlayerContext &context)
    {
        if (!context.character || !g_setInvulnerable)
            return false;

        SDK::ACrabPlayerC *character = context.character;
        g_godModeState.isClearing = false;
        g_godModeState.clearStablePasses = 0;
        g_setInvulnerable(character, true);
        return true;
    }

    void RestoreGodMode(const PlayerContext &context)
    {
        g_godModeState.isEnabled = false;
        g_godModeState.isClearing = context.character && g_setInvulnerable;
        g_godModeState.clearStablePasses = 0;
        if (g_godModeState.isClearing)
            g_setInvulnerable(context.character, false);
    }

    void MaintainGodModeClear(const PlayerContext &context)
    {
        if (!g_godModeState.isClearing || !context.character || !g_setInvulnerable)
            return;

        if (context.character->bIsInvulnerable)
        {
            // A replicated update can arrive after the toggle request. Clear
            // it through the native setter and restart the stability window.
            g_setInvulnerable(context.character, false);
            g_godModeState.clearStablePasses = 0;
            return;
        }

        ++g_godModeState.clearStablePasses;
        if (g_godModeState.clearStablePasses >= kGodModeClearStablePasses)
        {
            g_godModeState.isClearing = false;
            g_godModeState.clearStablePasses = 0;
        }
    }

    bool ApplyMovementSpeedLayer(const PlayerContext &context)
    {
        if (!context.character || !context.character->CharacterMovement)
            return false;

        auto *character = context.character;
        auto *movement = context.character->CharacterMovement;
        if (g_movementSpeedLayer.characterTarget != character ||
            g_movementSpeedLayer.movementTarget != movement ||
            !g_movementSpeedLayer.hasAppliedValue)
        {
            g_movementSpeedLayer.characterTarget = character;
            g_movementSpeedLayer.movementTarget = movement;
            g_movementSpeedLayer.gameBaseWalkSpeed = character->BaseWalkSpeed;
            g_movementSpeedLayer.gameWalkSpeed = movement->MaxWalkSpeed;
            g_movementSpeedLayer.gameCrouchedSpeed = movement->MaxWalkSpeedCrouched;
        }
        else
        {
            const bool baseSpeedWasLayered = NearlyEqual(
                character->BaseWalkSpeed,
                g_movementSpeedLayer.lastAppliedBaseWalkSpeed);
            if (!baseSpeedWasLayered)
                g_movementSpeedLayer.gameBaseWalkSpeed = character->BaseWalkSpeed;

            // BaseWalkSpeed is the game's authoritative grounded-speed input.
            // MaxWalkSpeed is also maintained for immediate and airborne
            // movement. If the game recalculates the component while the base
            // field is layered, divide our layer back out before composing it
            // again so native perks and ranks remain additive.
            if (!NearlyEqual(movement->MaxWalkSpeed, g_movementSpeedLayer.lastAppliedWalkSpeed))
            {
                float gameWalkSpeed = movement->MaxWalkSpeed;
                if (baseSpeedWasLayered)
                    gameWalkSpeed /= g_movementSpeedLayer.multiplier;
                g_movementSpeedLayer.gameWalkSpeed = gameWalkSpeed;
            }
            if (!NearlyEqual(movement->MaxWalkSpeedCrouched, g_movementSpeedLayer.lastAppliedCrouchedSpeed))
            {
                float gameCrouchedSpeed = movement->MaxWalkSpeedCrouched;
                if (baseSpeedWasLayered)
                    gameCrouchedSpeed /= g_movementSpeedLayer.multiplier;
                g_movementSpeedLayer.gameCrouchedSpeed = gameCrouchedSpeed;
            }
        }

        const float baseWalkSpeed =
            g_movementSpeedLayer.gameBaseWalkSpeed * g_movementSpeedLayer.multiplier;
        const float walkSpeed =
            g_movementSpeedLayer.gameWalkSpeed * g_movementSpeedLayer.multiplier;
        const float crouchedSpeed =
            g_movementSpeedLayer.gameCrouchedSpeed * g_movementSpeedLayer.multiplier;
        character->BaseWalkSpeed = baseWalkSpeed;
        movement->MaxWalkSpeed = walkSpeed;
        movement->MaxWalkSpeedCrouched = crouchedSpeed;
        g_movementSpeedLayer.lastAppliedBaseWalkSpeed = baseWalkSpeed;
        g_movementSpeedLayer.lastAppliedWalkSpeed = walkSpeed;
        g_movementSpeedLayer.lastAppliedCrouchedSpeed = crouchedSpeed;
        g_movementSpeedLayer.hasAppliedValue = true;
        return true;
    }

    void RestoreMovementSpeedLayer(const PlayerContext &context)
    {
        if (context.character && context.character->CharacterMovement &&
            context.character == g_movementSpeedLayer.characterTarget &&
            context.character->CharacterMovement == g_movementSpeedLayer.movementTarget &&
            g_movementSpeedLayer.hasAppliedValue)
        {
            auto *movement = context.character->CharacterMovement;
            if (NearlyEqual(
                    context.character->BaseWalkSpeed,
                    g_movementSpeedLayer.lastAppliedBaseWalkSpeed))
            {
                context.character->BaseWalkSpeed = g_movementSpeedLayer.gameBaseWalkSpeed;
            }
            if (NearlyEqual(movement->MaxWalkSpeed, g_movementSpeedLayer.lastAppliedWalkSpeed))
                movement->MaxWalkSpeed = g_movementSpeedLayer.gameWalkSpeed;
            if (NearlyEqual(movement->MaxWalkSpeedCrouched, g_movementSpeedLayer.lastAppliedCrouchedSpeed))
                movement->MaxWalkSpeedCrouched = g_movementSpeedLayer.gameCrouchedSpeed;
        }
        g_movementSpeedLayer = {};
    }

    float &GetWeaponRuntimeFloat(
        SDK::ACrabWeapon *weapon,
        std::size_t offset)
    {
        return *reinterpret_cast<float *>(
            reinterpret_cast<std::uint8_t *>(weapon) + offset);
    }

    void ApplyFiringRateLayer(SDK::ACrabWeapon *weapon)
    {
        if (!weapon || !g_isFiringRateEnabled)
            return;

        float &currentFireRate =
            GetWeaponRuntimeFloat(weapon, kCurrentWeaponFireRateOffset);
        float &currentFireInterval =
            GetWeaponRuntimeFloat(weapon, kCurrentWeaponFireIntervalOffset);
        float &currentBurstInterval =
            GetWeaponRuntimeFloat(weapon, kCurrentWeaponBurstIntervalOffset);
        FireRateLayerState &state = g_fireRateLayers[weapon];
        if (!state.hasAppliedValue)
        {
            state.gameFireRate = currentFireRate;
            state.gameFireInterval = currentFireInterval;
            state.gameBurstInterval = currentBurstInterval;
        }
        else
        {
            // A perk, rank, or other native recalculation can replace any of
            // these results. Treat each replacement as the new game-owned
            // baseline so this remains multiplicative on top of game buffs.
            if (!NearlyEqual(currentFireRate, state.lastAppliedFireRate))
                state.gameFireRate = currentFireRate;
            if (!NearlyEqual(currentFireInterval, state.lastAppliedFireInterval))
                state.gameFireInterval = currentFireInterval;
            if (!NearlyEqual(currentBurstInterval, state.lastAppliedBurstInterval))
                state.gameBurstInterval = currentBurstInterval;
        }

        const float fireRate = state.gameFireRate * g_firingRateMultiplier;
        const float fireInterval = state.gameFireInterval / g_firingRateMultiplier;
        const float burstInterval = state.gameBurstInterval / g_firingRateMultiplier;
        currentFireRate = fireRate;
        currentFireInterval = fireInterval;
        currentBurstInterval = burstInterval;
        state.lastAppliedFireRate = fireRate;
        state.lastAppliedFireInterval = fireInterval;
        state.lastAppliedBurstInterval = burstInterval;
        state.hasAppliedValue = true;
    }

    void RestoreFiringRateLayers(const PlayerContext &context)
    {
        // Weapon actors can be destroyed when swapping weapons or maps. Only
        // dereference actors which are still owned by the current character;
        // stale entries can simply be discarded with their dead actors.
        if (context.character)
        {
            for (SDK::ACrabWeapon *weapon : context.character->Weapons)
            {
                const auto stateIt = g_fireRateLayers.find(weapon);
                if (!weapon || stateIt == g_fireRateLayers.end() ||
                    !stateIt->second.hasAppliedValue)
                {
                    continue;
                }

                float &currentFireRate =
                    GetWeaponRuntimeFloat(weapon, kCurrentWeaponFireRateOffset);
                float &currentFireInterval =
                    GetWeaponRuntimeFloat(weapon, kCurrentWeaponFireIntervalOffset);
                float &currentBurstInterval =
                    GetWeaponRuntimeFloat(weapon, kCurrentWeaponBurstIntervalOffset);
                if (NearlyEqual(currentFireRate, stateIt->second.lastAppliedFireRate))
                    currentFireRate = stateIt->second.gameFireRate;
                if (NearlyEqual(
                        currentFireInterval,
                        stateIt->second.lastAppliedFireInterval))
                {
                    currentFireInterval = stateIt->second.gameFireInterval;
                }
                if (NearlyEqual(
                        currentBurstInterval,
                        stateIt->second.lastAppliedBurstInterval))
                {
                    currentBurstInterval = stateIt->second.gameBurstInterval;
                }
            }
        }
        g_fireRateLayers.clear();
    }

    void ApplyReloadTimeOverride(SDK::UCrabWeaponDA *weaponData)
    {
        if (!weaponData || !g_isReloadTimeEnabled)
            return;

        ValueOverrideState &state = g_reloadTimeOverrides[weaponData];
        if (!state.hasAppliedValue)
            state.gameValue = weaponData->ReloadDuration;
        else if (!NearlyEqual(weaponData->ReloadDuration, state.lastAppliedValue))
            state.gameValue = weaponData->ReloadDuration;

        weaponData->ReloadDuration = g_reloadTimeValue;
        state.lastAppliedValue = g_reloadTimeValue;
        state.hasAppliedValue = true;
    }

    void RestoreReloadTimeOverrides()
    {
        for (auto &[weaponData, state] : g_reloadTimeOverrides)
        {
            if (weaponData && state.hasAppliedValue &&
                NearlyEqual(weaponData->ReloadDuration, state.lastAppliedValue))
            {
                weaponData->ReloadDuration = state.gameValue;
            }
        }
        g_reloadTimeOverrides.clear();
    }

    void ApplyAmmoOverride(SDK::UCrabWeaponDA *weaponData)
    {
        if (!weaponData || !g_isAmmoEnabled)
            return;

        AmmoOverrideState &state = g_ammoOverrides[weaponData];
        if (!state.hasAppliedValue)
            state.gameClipSize = weaponData->BaseClipSize;
        else if (weaponData->BaseClipSize != state.lastAppliedClipSize)
            state.gameClipSize = weaponData->BaseClipSize;

        weaponData->BaseClipSize = g_ammoValue;
        state.lastAppliedClipSize = g_ammoValue;
        state.hasAppliedValue = true;
    }

    void RestoreAmmoOverrides()
    {
        for (auto &[weaponData, state] : g_ammoOverrides)
        {
            if (weaponData && state.hasAppliedValue &&
                weaponData->BaseClipSize == state.lastAppliedClipSize)
            {
                weaponData->BaseClipSize = state.gameClipSize;
            }
        }
        g_ammoOverrides.clear();
    }

    void RefreshWeaponAmmo(const PlayerContext &context)
    {
        if (!context.character)
            return;

        for (SDK::ACrabWeapon *weapon : context.character->Weapons)
        {
            if (!weapon)
                continue;
            weapon->OnRep_WeaponInfo();
            weapon->OnRep_TimesFired();
        }
    }

    bool ApplyWeaponCheats(const PlayerContext &context)
    {
        bool foundWeapon = false;
        auto applyWeaponData = [&](SDK::UCrabWeaponDA *weaponData)
        {
            if (!weaponData)
                return;
            foundWeapon = true;
            ApplyReloadTimeOverride(weaponData);
            ApplyAmmoOverride(weaponData);
        };

        if (context.playerState)
            applyWeaponData(context.playerState->WeaponDA);

        if (context.character)
        {
            for (SDK::ACrabWeapon *weapon : context.character->Weapons)
            {
                if (!weapon)
                    continue;
                ApplyFiringRateLayer(weapon);
                applyWeaponData(weapon->WeaponDA);

                // TimesFired is the replicated shots-consumed counter. Holding
                // it at zero keeps the configured clip full without relying on
                // an unknown native/private ammo offset. Refresh once when the
                // actor or requested clip changes so the HUD sees the new clip
                // immediately, even when TimesFired was already zero.
                if (g_isAmmoEnabled)
                {
                    const auto refreshIt = g_ammoRefreshStates.find(weapon);
                    const bool clipChanged =
                        refreshIt == g_ammoRefreshStates.end() ||
                        refreshIt->second.weaponData != weapon->WeaponDA ||
                        refreshIt->second.clipSize != g_ammoValue;
                    const bool consumedAmmo = weapon->TimesFired != 0;
                    if (consumedAmmo)
                        weapon->TimesFired = 0;

                    if (clipChanged)
                        weapon->OnRep_WeaponInfo();
                    if (clipChanged || consumedAmmo)
                        weapon->OnRep_TimesFired();

                    g_ammoRefreshStates[weapon] = {weapon->WeaponDA, g_ammoValue};
                }
            }

            // Drop state for weapon actors which were replaced. Their memory
            // is no longer safe to touch and the new actor gets a fresh native
            // baseline on the next pass.
            for (auto stateIt = g_fireRateLayers.begin();
                 stateIt != g_fireRateLayers.end();)
            {
                if (!context.character->Weapons.Contains(stateIt->first))
                {
                    stateIt = g_fireRateLayers.erase(stateIt);
                }
                else
                {
                    ++stateIt;
                }
            }
        }
        return foundWeapon;
    }

    void ApplyAbilityCooldownOverride(SDK::UCrabAbilityDA *abilityData)
    {
        if (!abilityData || !g_isNoAbilityMeleeCooldownEnabled)
            return;

        ValueOverrideState &state = g_abilityCooldownOverrides[abilityData];
        if (!state.hasAppliedValue)
            state.gameValue = abilityData->Cooldown;
        else if (!NearlyEqual(abilityData->Cooldown, state.lastAppliedValue))
            state.gameValue = abilityData->Cooldown;

        abilityData->Cooldown = 0.0f;
        state.lastAppliedValue = 0.0f;
        state.hasAppliedValue = true;
    }

    bool ApplyNoAbilityMeleeCooldown(const PlayerContext &context)
    {
        bool foundCooldownSource = false;
        if (context.playerState && context.playerState->AbilityDA)
        {
            ApplyAbilityCooldownOverride(context.playerState->AbilityDA);
            foundCooldownSource = true;
        }
        if (context.playerState && context.playerState->MeleeDA)
            foundCooldownSource = true;
        if (context.playerState)
        {
            for (SDK::FCrabInventoryCooldown &cooldown : context.playerState->InventoryCooldowns)
            {
                if (!cooldown.InventoryDA)
                    continue;

                if (cooldown.InventoryDA->IsA(SDK::UCrabAbilityDA::StaticClass()))
                {
                    cooldown.CurrentCooldown = 0;
                    foundCooldownSource = true;
                }
            }
        }
        if (context.character && context.character->AbilityDA)
        {
            ApplyAbilityCooldownOverride(context.character->AbilityDA);
            foundCooldownSource = true;
        }
        if (context.character && context.character->MeleeDA)
            foundCooldownSource = true;
        return foundCooldownSource;
    }

    void RestoreNoAbilityMeleeCooldown()
    {
        for (auto &[abilityData, state] : g_abilityCooldownOverrides)
        {
            if (abilityData && state.hasAppliedValue &&
                NearlyEqual(abilityData->Cooldown, state.lastAppliedValue))
            {
                abilityData->Cooldown = state.gameValue;
            }
        }
        g_abilityCooldownOverrides.clear();
        g_lastMeleeInputCharacter = nullptr;
        g_meleeInputLatched = false;
        g_lastMeleeInputAt = 0;
    }

    bool SetGodMode(bool isEnabled)
    {
        PlayerContext context = ResolvePlayerContext();
        std::lock_guard lock(g_sessionCheatMutex);
        if (!isEnabled)
        {
            if (!context.character)
                return false;
            RestoreGodMode(context);
            return true;
        }
        if (!context.character)
            return false;

        g_godModeState.isEnabled = true;
        if (ApplyGodMode(context))
            return true;

        RestoreGodMode(context);
        return false;
    }

    bool SetMovementSpeedMultiplier(bool isEnabled, float multiplier)
    {
        PlayerContext context = ResolvePlayerContext();
        std::lock_guard lock(g_sessionCheatMutex);
        if (!isEnabled)
        {
            RestoreMovementSpeedLayer(context);
            return true;
        }
        if (!IsValidMultiplier(multiplier) || !context.character ||
            !context.character->CharacterMovement)
            return false;

        g_movementSpeedLayer.isEnabled = true;
        g_movementSpeedLayer.multiplier = multiplier;
        if (ApplyMovementSpeedLayer(context))
            return true;

        RestoreMovementSpeedLayer(context);
        return false;
    }

    void RefreshPlayerStatUI()
    {
        PlayerContext context = ResolvePlayerContext();
        if (context.controller && context.controller->IsA(SDK::ACrabPC::StaticClass()))
            static_cast<SDK::ACrabPC *>(context.controller)->ClientRefreshPSUI();
    }

    bool SetStatMultiplier(
        bool isEnabled,
        float multiplier,
        std::atomic_bool &enabledState,
        std::atomic<float> &multiplierState)
    {
        if (isEnabled && !IsValidMultiplier(multiplier, true))
            return false;

        multiplierState.store(isEnabled ? multiplier : 1.0f);
        enabledState.store(isEnabled);
        RefreshPlayerStatUI();
        return true;
    }

    bool SetWeaponDamageMultiplier(bool isEnabled, float multiplier)
    {
        return SetStatMultiplier(
            isEnabled,
            multiplier,
            g_isWeaponDamageEnabled,
            g_weaponDamageMultiplier);
    }

    bool SetAbilityDamageMultiplier(bool isEnabled, float multiplier)
    {
        return SetStatMultiplier(
            isEnabled,
            multiplier,
            g_isAbilityDamageEnabled,
            g_abilityDamageMultiplier);
    }

    bool SetMeleeDamageMultiplier(bool isEnabled, float multiplier)
    {
        return SetStatMultiplier(
            isEnabled,
            multiplier,
            g_isMeleeDamageEnabled,
            g_meleeDamageMultiplier);
    }

    bool SetCriticalHitChanceMultiplier(bool isEnabled, float multiplier)
    {
        return SetStatMultiplier(
            isEnabled,
            multiplier,
            g_isCriticalHitChanceEnabled,
            g_criticalHitChanceMultiplier);
    }

    bool SetCriticalHitDamageMultiplier(bool isEnabled, float multiplier)
    {
        return SetStatMultiplier(
            isEnabled,
            multiplier,
            g_isCriticalHitDamageEnabled,
            g_criticalHitDamageMultiplier);
    }

    bool SetFiringRateMultiplier(bool isEnabled, float multiplier)
    {
        PlayerContext context = ResolvePlayerContext();
        std::lock_guard lock(g_sessionCheatMutex);
        if (!isEnabled)
        {
            g_isFiringRateEnabled = false;
            RestoreFiringRateLayers(context);
            RefreshPlayerStatUI();
            return true;
        }
        if (!IsValidMultiplier(multiplier))
            return false;

        g_firingRateMultiplier = multiplier;
        g_isFiringRateEnabled = true;
        if (ApplyWeaponCheats(context))
        {
            RefreshPlayerStatUI();
            return true;
        }

        g_isFiringRateEnabled = false;
        RestoreFiringRateLayers(context);
        return false;
    }

    bool SetReloadTime(bool isEnabled, float reloadTime)
    {
        PlayerContext context = ResolvePlayerContext();
        std::lock_guard lock(g_sessionCheatMutex);
        if (!isEnabled)
        {
            g_isReloadTimeEnabled = false;
            RestoreReloadTimeOverrides();
            return true;
        }
        if (!std::isfinite(reloadTime) || reloadTime < 0.0f)
            return false;

        g_reloadTimeValue = reloadTime;
        g_isReloadTimeEnabled = true;
        if (ApplyWeaponCheats(context))
            return true;

        g_isReloadTimeEnabled = false;
        RestoreReloadTimeOverrides();
        return false;
    }

    bool SetAmmo(bool isEnabled, int ammo)
    {
        PlayerContext context = ResolvePlayerContext();
        std::lock_guard lock(g_sessionCheatMutex);
        if (!isEnabled)
        {
            g_isAmmoEnabled = false;
            RestoreAmmoOverrides();
            g_ammoRefreshStates.clear();
            RefreshWeaponAmmo(context);
            return true;
        }
        if (ammo < 0)
            return false;

        if (!g_isAmmoEnabled || g_ammoValue != ammo)
            g_ammoRefreshStates.clear();
        g_ammoValue = ammo;
        g_isAmmoEnabled = true;
        if (ApplyWeaponCheats(context))
            return true;

        g_isAmmoEnabled = false;
        RestoreAmmoOverrides();
        g_ammoRefreshStates.clear();
        return false;
    }

    bool SetNoAbilityMeleeCooldown(bool isEnabled)
    {
        PlayerContext context = ResolvePlayerContext();
        std::lock_guard lock(g_sessionCheatMutex);
        if (!isEnabled)
        {
            g_isNoAbilityMeleeCooldownEnabled = false;
            RestoreNoAbilityMeleeCooldown();
            return true;
        }

        g_isNoAbilityMeleeCooldownEnabled = true;
        if (ApplyNoAbilityMeleeCooldown(context))
            return true;

        g_isNoAbilityMeleeCooldownEnabled = false;
        RestoreNoAbilityMeleeCooldown();
        return false;
    }

    // Persistent weapon, ability, and melee ranks
    SDK::UEnum *GetCrabRankEnum()
    {
        return SDK::UObject::FindObjectFast<SDK::UEnum>(
            "ECrabRank",
            SDK::EClassCastFlags::Enum);
    }

    std::string GetRankName(SDK::UEnum *rankEnum, SDK::ECrabRank rank)
    {
        if (!rankEnum)
            return {};

        const auto rankValue = static_cast<SDK::int64>(rank);
        for (const auto &entry : rankEnum->Names)
        {
            if (entry.Value() != rankValue)
                continue;

            std::string name = entry.Key().ToString();
            const std::size_t separator = name.rfind("::");
            if (separator != std::string::npos)
                name.erase(0, separator + 2);
            return name;
        }

        return {};
    }

    const char *GetRankablePickupTypeName(SDK::ECrabPickupType pickupType)
    {
        switch (pickupType)
        {
        case SDK::ECrabPickupType::Weapon:
            return "Weapon";
        case SDK::ECrabPickupType::Ability:
            return "Ability";
        case SDK::ECrabPickupType::Melee:
            return "Melee";
        default:
            return nullptr;
        }
    }

    bool WriteWeaponAbilityMeleeList(char *output, std::size_t outputCapacity)
    {
        if (!output || outputCapacity == 0)
            return false;
        output[0] = '\0';

        PlayerContext context = ResolvePlayerContext();
        if (!context.gameInstance || !context.gameInstance->SG)
            return false;

        SDK::UEnum *rankEnum = GetCrabRankEnum();
        if (!rankEnum)
            return false;

        std::size_t used = 0;
        bool foundItem = false;
        auto &rankedWeapons = context.gameInstance->SG->RankedWeapons;
        for (int index = 0; index < rankedWeapons.Num(); ++index)
        {
            const SDK::FCrabRankedWeapon &entry = rankedWeapons[index];
            if (!entry.Weapon)
                continue;

            const char *typeName = GetRankablePickupTypeName(entry.Weapon->PickupType);
            if (!typeName)
                continue;

            std::string itemName = entry.Weapon->Name_0.ToString();
            for (char &character : itemName)
            {
                if (character == '>' || character == '\r' || character == '\n')
                    character = ' ';
            }

            const std::string rankName = GetRankName(rankEnum, entry.Rank);
            if (rankName.empty())
                continue;

            const int written = std::snprintf(
                output + used,
                outputCapacity - used,
                "%d>%s>%s>%s\n",
                index,
                typeName,
                itemName.c_str(),
                rankName.c_str());
            if (written < 0 || static_cast<std::size_t>(written) >= outputCapacity - used)
            {
                output[0] = '\0';
                return false;
            }

            used += static_cast<std::size_t>(written);
            foundItem = true;
        }

        return foundItem;
    }

    bool WriteRankList(char *output, std::size_t outputCapacity)
    {
        if (!output || outputCapacity == 0)
            return false;
        output[0] = '\0';

        SDK::UEnum *rankEnum = GetCrabRankEnum();
        if (!rankEnum)
            return false;

        std::size_t used = 0;
        for (int rankValue = static_cast<int>(SDK::ECrabRank::Bronze);
             rankValue <= static_cast<int>(SDK::ECrabRank::Prismatic);
             ++rankValue)
        {
            const std::string rankName = GetRankName(
                rankEnum,
                static_cast<SDK::ECrabRank>(rankValue));
            if (rankName.empty())
                return false;

            const int written = std::snprintf(
                output + used,
                outputCapacity - used,
                "%d>%s\n",
                rankValue,
                rankName.c_str());
            if (written < 0 || static_cast<std::size_t>(written) >= outputCapacity - used)
            {
                output[0] = '\0';
                return false;
            }

            used += static_cast<std::size_t>(written);
        }

        return used > 0;
    }

    bool SetWeaponAbilityMeleeRank(int itemId, int rankValue, bool applyToAll)
    {
        if (rankValue < static_cast<int>(SDK::ECrabRank::Bronze) ||
            rankValue > static_cast<int>(SDK::ECrabRank::Prismatic))
        {
            return false;
        }

        PlayerContext context = ResolvePlayerContext();
        if (!context.gameInstance || !context.gameInstance->SG)
            return false;

        auto *saveGame = context.gameInstance->SG;
        auto &rankedWeapons = saveGame->RankedWeapons;
        const SDK::ECrabRank rank = static_cast<SDK::ECrabRank>(rankValue);
        std::vector<std::pair<SDK::FCrabRankedWeapon *, SDK::ECrabRank>> previousRanks;

        auto applyRank = [&](SDK::FCrabRankedWeapon &entry)
        {
            if (!entry.Weapon || !GetRankablePickupTypeName(entry.Weapon->PickupType))
                return false;
            previousRanks.emplace_back(&entry, entry.Rank);
            entry.Rank = rank;
            return true;
        };

        if (applyToAll)
        {
            previousRanks.reserve(rankedWeapons.Num());
            for (SDK::FCrabRankedWeapon &entry : rankedWeapons)
                applyRank(entry);
        }
        else
        {
            if (!rankedWeapons.IsValidIndex(itemId) || !applyRank(rankedWeapons[itemId]))
                return false;
        }

        if (previousRanks.empty())
            return false;

        // RankedWeapons is the account-progression array. Persist the modified
        // live save object synchronously on the game thread to the game's slot.
        if (SDK::UGameplayStatics::SaveGameToSlot(
                saveGame,
                SDK::FString(L"SaveSlot"),
                0))
            return true;

        // Do not leave an unsaved mutation behind if serialization failed.
        for (const auto &[entry, previousRank] : previousRanks)
            entry->Rank = previousRank;
        return false;
    }

    // One-shot player values and session unlocks
    bool SetCrystals(int value)
    {
        if (value < 0)
            return false;

        PlayerContext context = ResolvePlayerContext();
        if (!context.playerState)
            return false;

        const std::uint32_t crystals = static_cast<std::uint32_t>(value);
        context.playerState->Crystals = crystals;

        // Keep a pending run autosave consistent with the live player state.
        if (context.gameInstance && context.gameInstance->SG)
            context.gameInstance->SG->AutoSave.Crystals = crystals;

        context.playerState->OnRep_Crystals();
        return true;
    }

    bool SetKeys(int value)
    {
        if (value < 0)
            return false;

        PlayerContext context = ResolvePlayerContext();
        if (!context.playerState)
            return false;

        // This is the game's reflected, reliable server RPC for refreshing the
        // account rank, level, and key balance. Preserve the first two values.
        context.playerState->ServerRefreshAccount(
            context.playerState->AccountRank,
            context.playerState->AccountLevel,
            value);

        // Apply the local mirror immediately so the UI does not need to wait for
        // a replication round trip. The save-game object is what the game uses
        // when it next persists account data.
        context.playerState->Keys = value;
        if (context.gameInstance && context.gameInstance->SG)
            context.gameInstance->SG->Keys = value;

        context.playerState->OnRep_Keys();
        return true;
    }

    bool SetCurrentHealth(int value)
    {
        if (value < 0)
            return false;

        PlayerContext context = ResolvePlayerContext();
        if (!context.playerState || !context.healthComponent)
            return false;

        const float desiredHealth = static_cast<float>(value);
        SDK::FCrabHealthInfo &health = context.healthComponent->HealthInfo;

        health.PreviousHealth = health.CurrentHealth;
        health.CurrentHealth = desiredHealth;

        // CrabPS contains a transient replicated mirror used by UI elements.
        // Do not touch CrabSG here: the lock is deliberately limited to the
        // current live run and must not alter saved health data.
        context.playerState->HealthInfo = health;
        context.healthComponent->OnRep_HealthInfo();
        return true;
    }

    bool SetHealth(bool isEnabled, int value)
    {
        if (!isEnabled)
        {
            g_isHealthLockEnabled.store(false);
            return true;
        }

        if (value < 0)
            return false;
        if (!SetCurrentHealth(value))
            return false;

        g_lockedHealth.store(value);
        g_lastSessionMaintenance.store(GetTickCount64());
        g_isHealthLockEnabled.store(true);
        return true;
    }

    bool SetMaxHealth(int value)
    {
        if (value < 0)
            return false;

        PlayerContext context = ResolvePlayerContext();
        if (!context.playerState || !context.healthComponent)
            return false;

        const float desiredMaxHealth = static_cast<float>(value);
        const float currentMaxHealthMultiplier = context.playerState->MaxHealthMultiplier;
        const float effectiveMaxHealthMultiplier =
            std::isfinite(currentMaxHealthMultiplier) &&
                    std::fabs(currentMaxHealthMultiplier) > 0.0001f
                ? currentMaxHealthMultiplier
                : 1.0f;
        const float authoritativeBaseHealth =
            desiredMaxHealth / effectiveMaxHealthMultiplier;

        // Damage handling rebuilds CurrentMaxHealth from these authoritative
        // base values. Keep the game's existing multiplier intact so perks and
        // other legitimate max-health bonuses still stack on top of this base.
        context.playerState->BaseMaxHealth = authoritativeBaseHealth;
        context.playerState->MaxHealthMultiplier = effectiveMaxHealthMultiplier;
        context.healthComponent->BaseMaxHealth = authoritativeBaseHealth;

        SDK::FCrabHealthInfo &health = context.healthComponent->HealthInfo;
        health.PreviousMaxHealth = health.CurrentMaxHealth;
        health.CurrentMaxHealth = desiredMaxHealth;

        // Keep both the replicated mirror and the current-run autosave source
        // synchronized. CurrentHealth remains independent except for clamping
        // an otherwise invalid value above the newly selected maximum.
        if (health.CurrentHealth > desiredMaxHealth)
        {
            health.PreviousHealth = health.CurrentHealth;
            health.CurrentHealth = desiredMaxHealth;
        }
        context.playerState->HealthInfo = health;
        if (context.gameInstance && context.gameInstance->SG)
        {
            auto &autoSave = context.gameInstance->SG->AutoSave;
            autoSave.BaseMaxHealth = authoritativeBaseHealth;
            autoSave.MaxHealthMultiplier = effectiveMaxHealthMultiplier;
            autoSave.HealthInfo = health;
        }
        context.healthComponent->OnRep_HealthInfo();
        return true;
    }

    template <typename PickupType>
    int UnlockPickupArray(SDK::TArray<PickupType *> &pickups)
    {
        int unlockedCount = 0;
        for (PickupType *pickup : pickups)
        {
            if (!pickup)
                continue;

            pickup->bRequiresUnlock = false;
            ++unlockedCount;
        }
        return unlockedCount;
    }

    template <typename PickupType>
    bool UnlockPickupCategory(
        SDK::TArray<PickupType *> SDK::UCrabSpawnablesDA::*category)
    {
        int unlockedCount = 0;
        SDK::UClass *spawnablesClass = SDK::UCrabSpawnablesDA::StaticClass();

        // SpawnablesDA owns the complete loadout lists and keeps all referenced
        // data assets loaded. Clearing bRequiresUnlock affects only these live
        // asset instances and cannot be serialized into CrabSG.
        for (int index = 0; index < SDK::UObject::GObjects->Num(); ++index)
        {
            SDK::UObject *object = SDK::UObject::GObjects->GetByIndex(index);
            if (!object || !object->IsA(spawnablesClass) || object->IsDefaultObject())
                continue;

            auto *spawnables = static_cast<SDK::UCrabSpawnablesDA *>(object);
            unlockedCount += UnlockPickupArray(spawnables->*category);
        }

        // Fall back to loaded objects if the current map does not own a level
        // manager yet. The loadout menu normally loads the same asset set.
        if (unlockedCount == 0)
        {
            SDK::UClass *pickupClass = PickupType::StaticClass();
            for (int index = 0; index < SDK::UObject::GObjects->Num(); ++index)
            {
                SDK::UObject *object = SDK::UObject::GObjects->GetByIndex(index);
                if (!object || !object->IsA(pickupClass) || object->IsDefaultObject())
                    continue;

                static_cast<PickupType *>(object)->bRequiresUnlock = false;
                ++unlockedCount;
            }
        }

        return unlockedCount > 0;
    }

    bool UnlockAllWeapons()
    {
        return UnlockPickupCategory(&SDK::UCrabSpawnablesDA::Weapons);
    }

    bool UnlockAllMelee()
    {
        return UnlockPickupCategory(&SDK::UCrabSpawnablesDA::MeleeWeapons);
    }

    bool UnlockAllAbilities()
    {
        return UnlockPickupCategory(&SDK::UCrabSpawnablesDA::Abilities);
    }

    void CompleteCosmeticChallenge(SDK::FCrabChallenge &challenge)
    {
        challenge.ChallengeProgress = (std::max)(
            challenge.ChallengeProgress,
            challenge.ChallengeGoal);
        challenge.bChallengeCompleted = true;
    }

    void CompleteLoadedCosmeticSlots()
    {
        SDK::UClass *slotClass = SDK::UCrabCosmeticSlotUI::StaticClass();
        for (int index = 0; index < SDK::UObject::GObjects->Num(); ++index)
        {
            SDK::UObject *object = SDK::UObject::GObjects->GetByIndex(index);
            if (!object || !object->IsA(slotClass) || object->IsDefaultObject())
                continue;

            auto *slot = static_cast<SDK::UCrabCosmeticSlotUI *>(object);
            CompleteCosmeticChallenge(slot->ChallengeToUnlock);
        }
    }

    bool UnlockAllCosmetics()
    {
        PlayerContext context = ResolvePlayerContext();
        if (!context.gameInstance)
            return false;

        // CrabGI holds the live challenge definitions used to populate the
        // cosmetics menu. CrabSG holds persistent progress and is intentionally
        // not modified.
        for (SDK::FCrabChallenge &challenge : context.gameInstance->Challenges)
            CompleteCosmeticChallenge(challenge);

        CompleteLoadedCosmeticSlots();
        g_areCosmeticsUnlocked.store(true);
        return true;
    }

    // Game-thread maintenance and request dispatch
    void MaintainSessionCheats(const SDK::UObject *eventObject)
    {
        bool hasTimedCheat = g_isHealthLockEnabled.load();
        {
            std::lock_guard lock(g_sessionCheatMutex);
            hasTimedCheat = hasTimedCheat ||
                            g_godModeState.isEnabled ||
                            g_godModeState.isClearing ||
                            g_movementSpeedLayer.isEnabled ||
                            g_isFiringRateEnabled ||
                            g_isReloadTimeEnabled ||
                            g_isAmmoEnabled ||
                            g_isNoAbilityMeleeCooldownEnabled;
        }

        if (hasTimedCheat)
        {
            const ULONGLONG now = GetTickCount64();
            ULONGLONG lastUpdate = g_lastSessionMaintenance.load();
            if (now - lastUpdate >= 16 &&
                g_lastSessionMaintenance.compare_exchange_strong(
                    lastUpdate,
                    now))
            {
                PlayerContext context = ResolvePlayerContext();
                if (g_isHealthLockEnabled.load() && context.healthComponent)
                {
                    const float desiredHealth = static_cast<float>(g_lockedHealth.load());
                    if (context.healthComponent->HealthInfo.CurrentHealth != desiredHealth)
                        SetCurrentHealth(g_lockedHealth.load());
                }

                std::lock_guard lock(g_sessionCheatMutex);
                if (g_godModeState.isEnabled)
                    ApplyGodMode(context);
                else if (g_godModeState.isClearing)
                    MaintainGodModeClear(context);
                if (g_movementSpeedLayer.isEnabled)
                    ApplyMovementSpeedLayer(context);
                if (g_isFiringRateEnabled || g_isReloadTimeEnabled || g_isAmmoEnabled)
                    ApplyWeaponCheats(context);
                if (g_isNoAbilityMeleeCooldownEnabled)
                    ApplyNoAbilityMeleeCooldown(context);
            }
        }

        // Cosmetic slot widgets copy their challenge record. Complete newly
        // constructed copies as they begin receiving events, so the session
        // unlock also applies to menus opened after the trainer button is used.
        if (g_areCosmeticsUnlocked.load() && eventObject &&
            eventObject->IsA(SDK::UCrabCosmeticSlotUI::StaticClass()))
        {
            auto *slot = const_cast<SDK::UCrabCosmeticSlotUI *>(
                static_cast<const SDK::UCrabCosmeticSlotUI *>(eventObject));
            CompleteCosmeticChallenge(slot->ChallengeToUnlock);
        }
    }

    bool ExecuteRequest(const Request &request)
    {
        switch (request.type)
        {
        case RequestType::SetCrystals:
            return SetCrystals(request.value);
        case RequestType::SetKeys:
            return SetKeys(request.value);
        case RequestType::SetHealth:
            return SetHealth(request.isEnabled, request.value);
        case RequestType::SetMaxHealth:
            return SetMaxHealth(request.value);
        case RequestType::SetGodMode:
            return SetGodMode(request.isEnabled);
        case RequestType::UnlockAllWeapons:
            return UnlockAllWeapons();
        case RequestType::UnlockAllMelee:
            return UnlockAllMelee();
        case RequestType::UnlockAllAbilities:
            return UnlockAllAbilities();
        case RequestType::UnlockAllCosmetics:
            return UnlockAllCosmetics();
        case RequestType::SetMovementSpeedMultiplier:
            return SetMovementSpeedMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetWeaponDamageMultiplier:
            return SetWeaponDamageMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetAbilityDamageMultiplier:
            return SetAbilityDamageMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetMeleeDamageMultiplier:
            return SetMeleeDamageMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetCriticalHitChanceMultiplier:
            return SetCriticalHitChanceMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetCriticalHitDamageMultiplier:
            return SetCriticalHitDamageMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetFiringRateMultiplier:
            return SetFiringRateMultiplier(request.isEnabled, request.floatValue);
        case RequestType::SetReloadTime:
            return SetReloadTime(request.isEnabled, request.floatValue);
        case RequestType::SetAmmo:
            return SetAmmo(request.isEnabled, request.value);
        case RequestType::SetNoAbilityMeleeCooldown:
            return SetNoAbilityMeleeCooldown(request.isEnabled);
        case RequestType::GetWeaponAbilityMeleeList:
            return WriteWeaponAbilityMeleeList(request.output, request.outputCapacity);
        case RequestType::GetRankList:
            return WriteRankList(request.output, request.outputCapacity);
        case RequestType::SetWeaponAbilityMeleeRank:
            return SetWeaponAbilityMeleeRank(
                request.value,
                request.secondaryValue,
                request.applyToAll);
        default:
            return false;
        }
    }

    void ProcessPendingRequest()
    {
        Request requestToRun{};
        {
            std::lock_guard lock(g_requestMutex);
            if (g_request.state != RequestState::Pending)
                return;

            g_request.state = RequestState::Processing;
            requestToRun = g_request;
        }

        const bool result = ExecuteRequest(requestToRun);

        {
            std::lock_guard lock(g_requestMutex);
            g_request.result = result;
            g_request.state = RequestState::Complete;
        }
        g_requestCondition.notify_all();
    }

    // Native hooks and bridge lifecycle
    float ApplyStatMultiplier(
        float gameValue,
        const std::atomic_bool &enabledState,
        const std::atomic<float> &multiplierState)
    {
        return enabledState.load()
                   ? gameValue * multiplierState.load()
                   : gameValue;
    }

    float __fastcall HookedWeaponDamageGetter(SDK::ACrabPS *playerState)
    {
        const float gameValue = g_originalWeaponDamageGetter
                                    ? g_originalWeaponDamageGetter(playerState)
                                    : 0.0f;
        return ApplyStatMultiplier(
            gameValue,
            g_isWeaponDamageEnabled,
            g_weaponDamageMultiplier);
    }

    float __fastcall HookedAbilityDamageGetter(SDK::ACrabPS *playerState)
    {
        const float gameValue = g_originalAbilityDamageGetter
                                    ? g_originalAbilityDamageGetter(playerState)
                                    : 0.0f;
        const std::uintptr_t imageBase =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const bool isStatsPanelCall =
            imageBase &&
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()) ==
                imageBase + kAbilityDamageStatsReturnRva;
        return isStatsPanelCall
                   ? ApplyStatMultiplier(
                         gameValue,
                         g_isAbilityDamageEnabled,
                         g_abilityDamageMultiplier)
                   : gameValue;
    }

    float __fastcall HookedMeleeDamageGetter(SDK::APlayerController *controller)
    {
        const float gameValue = g_originalMeleeDamageGetter
                                    ? g_originalMeleeDamageGetter(controller)
                                    : 0.0f;
        return ApplyStatMultiplier(
            gameValue,
            g_isMeleeDamageEnabled,
            g_meleeDamageMultiplier);
    }

    float __fastcall HookedCriticalHitChanceGetter(SDK::ACrabPS *playerState)
    {
        const float gameValue = g_originalCriticalHitChanceGetter
                                    ? g_originalCriticalHitChanceGetter(playerState)
                                    : 0.0f;
        return ApplyStatMultiplier(
            gameValue,
            g_isCriticalHitChanceEnabled,
            g_criticalHitChanceMultiplier);
    }

    float __fastcall HookedCriticalHitDamageGetter(SDK::ACrabPS *playerState)
    {
        const float gameValue = g_originalCriticalHitDamageGetter
                                    ? g_originalCriticalHitDamageGetter(playerState)
                                    : 0.0f;
        return ApplyStatMultiplier(
            gameValue,
            g_isCriticalHitDamageEnabled,
            g_criticalHitDamageMultiplier);
    }

    void __fastcall HookedFinalizeDamageInfo(
        SDK::ACrabPS *playerState,
        SDK::FCrabDamageInfo *damageInfo)
    {
        if (!g_originalFinalizeDamageInfo)
            return;

        g_originalFinalizeDamageInfo(playerState, damageInfo);
        if (!damageInfo)
            return;

        float trainerMultiplier = 1.0f;
        if (g_isAbilityDamageEnabled.load() &&
            damageInfo->CrabDamageType == SDK::ECrabDamageType::Ability)
        {
            trainerMultiplier *= g_abilityDamageMultiplier.load();
        }

        if (g_isCriticalHitDamageEnabled.load() &&
            damageInfo->DamageTags.Contains(
                SDK::ECrabDamageTagType::CriticalHit))
        {
            trainerMultiplier *= g_criticalHitDamageMultiplier.load();
        }

        damageInfo->Damage *= trainerMultiplier;
    }

    void __fastcall HookedMeleeInput(SDK::ACrabPlayerC *character)
    {
        if (!g_originalMeleeInput || !character)
            return;

        if (!g_isNoAbilityMeleeCooldownEnabled)
        {
            g_lastMeleeInputCharacter = nullptr;
            g_meleeInputLatched = false;
            g_lastMeleeInputAt = 0;
            g_originalMeleeInput(character);
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const bool isNewPress =
            character != g_lastMeleeInputCharacter ||
            !g_meleeInputLatched ||
            now - g_lastMeleeInputAt >= kMeleeReleaseGapMilliseconds;
        g_lastMeleeInputCharacter = character;
        g_lastMeleeInputAt = now;
        if (!isNewPress)
        {
            // The engine samples this input while the button remains held. A
            // zero data-asset cooldown therefore made one press execute the
            // attack several times. Preserve exactly one native call per edge.
            return;
        }

        g_meleeInputLatched = true;
        *reinterpret_cast<float *>(
            reinterpret_cast<std::uint8_t *>(character) +
            kCurrentMeleeCooldownOffset) = 0.0f;
        g_originalMeleeInput(character);
    }

    void __fastcall HookedProcessEvent(const SDK::UObject *object, SDK::UFunction *function, void *parameters)
    {
        if (!g_isProcessingRequest)
        {
            g_isProcessingRequest = true;
            ProcessPendingRequest();
            g_isProcessingRequest = false;
        }

        MaintainSessionCheats(object);

        g_originalProcessEvent(object, function, parameters);
    }

    bool CreateAndEnableHook(void *target, void *detour, void **original)
    {
        const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
        if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
            return false;

        const MH_STATUS enableStatus = MH_EnableHook(target);
        return enableStatus == MH_OK || enableStatus == MH_ERROR_ENABLED;
    }

    void *GetNativeTarget(std::uintptr_t imageBase, std::uintptr_t rva)
    {
        return reinterpret_cast<void *>(imageBase + rva);
    }

    template <std::size_t Size>
    bool HasExpectedPrologue(
        const void *target,
        const std::uint8_t (&expected)[Size])
    {
        return target && std::memcmp(target, expected, Size) == 0;
    }

    bool InstallHooks()
    {
        SDK::UWorld *world = GetWorld();
        if (!world)
            return false;

        void *processEvent = SDK::InSDKUtils::GetVirtualFunction<void *>(
            world,
            SDK::Offsets::ProcessEventIdx);
        if (!processEvent)
            return false;

        const MH_STATUS initializeStatus = MH_Initialize();
        if (initializeStatus != MH_OK && initializeStatus != MH_ERROR_ALREADY_INITIALIZED)
            return false;

        const std::uintptr_t imageBase =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!imageBase)
            return false;

        constexpr std::uint8_t expectedWeaponDamagePrologue[] = {
            0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70};
        constexpr std::uint8_t expectedAbilityDamagePrologue[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x30,
            0x48, 0x8D, 0x99, 0xF8, 0x03, 0x00, 0x00};
        constexpr std::uint8_t expectedMeleeDamagePrologue[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x40,
            0x48, 0x8B, 0x99, 0x40, 0x02, 0x00, 0x00};
        constexpr std::uint8_t expectedCriticalChancePrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40};
        constexpr std::uint8_t expectedCriticalDamagePrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50};
        constexpr std::uint8_t expectedFinalizeDamageInfoPrologue[] = {
            0x40, 0x55, 0x53, 0x41, 0x54, 0x41, 0x56,
            0x48, 0x8D, 0xAC, 0x24, 0x68, 0xFF, 0xFF, 0xFF};
        constexpr std::uint8_t expectedSetInvulnerablePrologue[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
        constexpr std::uint8_t expectedMeleeInputPrologue[] = {
            0x40, 0x55, 0x53, 0x48, 0x8D, 0x6C, 0x24, 0xB1,
            0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00};
        void *weaponDamageTarget = GetNativeTarget(imageBase, kWeaponDamageGetterRva);
        void *abilityDamageTarget = GetNativeTarget(imageBase, kAbilityDamageGetterRva);
        void *meleeDamageTarget = GetNativeTarget(imageBase, kMeleeDamageGetterRva);
        void *criticalChanceTarget = GetNativeTarget(imageBase, kCriticalHitChanceGetterRva);
        void *criticalDamageTarget = GetNativeTarget(imageBase, kCriticalHitDamageGetterRva);
        void *finalizeDamageInfoTarget = GetNativeTarget(imageBase, kFinalizeDamageInfoRva);
        void *setInvulnerableTarget = GetNativeTarget(imageBase, kSetInvulnerableRva);
        void *meleeInputTarget = GetNativeTarget(imageBase, kMeleeInputRva);
        if (!HasExpectedPrologue(weaponDamageTarget, expectedWeaponDamagePrologue) ||
            !HasExpectedPrologue(abilityDamageTarget, expectedAbilityDamagePrologue) ||
            !HasExpectedPrologue(meleeDamageTarget, expectedMeleeDamagePrologue) ||
            !HasExpectedPrologue(criticalChanceTarget, expectedCriticalChancePrologue) ||
            !HasExpectedPrologue(criticalDamageTarget, expectedCriticalDamagePrologue) ||
            !HasExpectedPrologue(finalizeDamageInfoTarget, expectedFinalizeDamageInfoPrologue) ||
            !HasExpectedPrologue(setInvulnerableTarget, expectedSetInvulnerablePrologue) ||
            !HasExpectedPrologue(meleeInputTarget, expectedMeleeInputPrologue))
        {
            // Do not detour an unknown game build at stale RVAs.
            return false;
        }

        g_setInvulnerable =
            reinterpret_cast<SetInvulnerableFn>(setInvulnerableTarget);

        if (!CreateAndEnableHook(
                weaponDamageTarget,
                reinterpret_cast<void *>(&HookedWeaponDamageGetter),
                reinterpret_cast<void **>(&g_originalWeaponDamageGetter)) ||
            !CreateAndEnableHook(
                abilityDamageTarget,
                reinterpret_cast<void *>(&HookedAbilityDamageGetter),
                reinterpret_cast<void **>(&g_originalAbilityDamageGetter)) ||
            !CreateAndEnableHook(
                meleeDamageTarget,
                reinterpret_cast<void *>(&HookedMeleeDamageGetter),
                reinterpret_cast<void **>(&g_originalMeleeDamageGetter)) ||
            !CreateAndEnableHook(
                criticalChanceTarget,
                reinterpret_cast<void *>(&HookedCriticalHitChanceGetter),
                reinterpret_cast<void **>(&g_originalCriticalHitChanceGetter)) ||
            !CreateAndEnableHook(
                criticalDamageTarget,
                reinterpret_cast<void *>(&HookedCriticalHitDamageGetter),
                reinterpret_cast<void **>(&g_originalCriticalHitDamageGetter)) ||
            !CreateAndEnableHook(
                finalizeDamageInfoTarget,
                reinterpret_cast<void *>(&HookedFinalizeDamageInfo),
                reinterpret_cast<void **>(&g_originalFinalizeDamageInfo)) ||
            !CreateAndEnableHook(
                meleeInputTarget,
                reinterpret_cast<void *>(&HookedMeleeInput),
                reinterpret_cast<void **>(&g_originalMeleeInput)) ||
            !CreateAndEnableHook(
                processEvent,
                reinterpret_cast<void *>(&HookedProcessEvent),
                reinterpret_cast<void **>(&g_originalProcessEvent)))
        {
            return false;
        }

        g_hookReady.store(true);
        return true;
    }

    bool QueueRequest(
        RequestType type,
        int value = 0,
        float floatValue = 0.0f,
        bool isEnabled = false,
        int secondaryValue = 0,
        char *output = nullptr,
        std::size_t outputCapacity = 0,
        bool applyToAll = false)
    {
        const auto hookDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!g_hookReady.load() &&
               std::chrono::steady_clock::now() < hookDeadline)
            Sleep(20);

        if (!g_hookReady.load())
            return false;

        std::unique_lock lock(g_requestMutex);
        if (g_request.state == RequestState::Complete)
            g_request = {};

        if (g_request.state != RequestState::Idle)
            return false;

        g_request.type = type;
        g_request.value = value;
        g_request.secondaryValue = secondaryValue;
        g_request.floatValue = floatValue;
        g_request.isEnabled = isEnabled;
        g_request.applyToAll = applyToAll;
        g_request.output = output;
        g_request.outputCapacity = outputCapacity;
        g_request.result = false;
        g_request.state = RequestState::Pending;

        const bool completed = g_requestCondition.wait_for(
            lock,
            std::chrono::seconds(4),
            [] { return g_request.state == RequestState::Complete; });

        if (!completed)
        {
            // A request which has not started can be cancelled safely. If the game
            // thread is already processing it, leave the state intact for completion.
            if (g_request.state == RequestState::Pending)
                g_request = {};
            return false;
        }

        const bool result = g_request.result;
        g_request = {};
        return result;
    }

    DWORD WINAPI InitializeBridge(LPVOID)
    {
        while (!InstallHooks())
            Sleep(100);

        return 1;
    }

    DWORD BridgeResult(bool result)
    {
        return result ? 1 : 0;
    }

    DWORD QueueActionBridgeRequest(RequestType type, LPVOID argument)
    {
        return BridgeResult(argument && QueueRequest(type));
    }

    DWORD QueueIntBridgeRequest(RequestType type, LPVOID argument)
    {
        if (!argument)
            return 0;

        return BridgeResult(QueueRequest(type, *static_cast<const int *>(argument)));
    }

    DWORD QueueToggleBridgeRequest(RequestType type, LPVOID argument)
    {
        if (!argument)
            return 0;

        return BridgeResult(QueueRequest(
            type,
            0,
            0.0f,
            *static_cast<const int *>(argument) != 0));
    }

    DWORD QueueToggleFloatBridgeRequest(RequestType type, LPVOID argument)
    {
        if (!argument)
            return 0;

        const auto &value = *static_cast<const ToggleFloatArgument *>(argument);
        return BridgeResult(QueueRequest(
            type,
            0,
            value.value,
            value.isEnabled != 0));
    }

    DWORD QueueToggleIntBridgeRequest(RequestType type, LPVOID argument)
    {
        if (!argument)
            return 0;

        const auto &value = *static_cast<const ToggleIntArgument *>(argument);
        return BridgeResult(QueueRequest(
            type,
            value.value,
            0.0f,
            value.isEnabled != 0));
    }
}

// Public bridge exports
extern "C" __declspec(dllexport) DWORD WINAPI SetCrystals(LPVOID argument)
{
    return QueueIntBridgeRequest(RequestType::SetCrystals, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetKeys(LPVOID argument)
{
    return QueueIntBridgeRequest(RequestType::SetKeys, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetHealth(LPVOID argument)
{
    return QueueToggleIntBridgeRequest(RequestType::SetHealth, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetMaxHealth(LPVOID argument)
{
    return QueueIntBridgeRequest(RequestType::SetMaxHealth, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetGodMode(LPVOID argument)
{
    return QueueToggleBridgeRequest(RequestType::SetGodMode, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI UnlockAllWeapons(LPVOID argument)
{
    return QueueActionBridgeRequest(RequestType::UnlockAllWeapons, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI UnlockAllMelee(LPVOID argument)
{
    return QueueActionBridgeRequest(RequestType::UnlockAllMelee, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI UnlockAllAbilities(LPVOID argument)
{
    return QueueActionBridgeRequest(RequestType::UnlockAllAbilities, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI UnlockAllCosmetics(LPVOID argument)
{
    return QueueActionBridgeRequest(RequestType::UnlockAllCosmetics, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetMovementSpeedMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetMovementSpeedMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetWeaponDamageMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetWeaponDamageMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetAbilityDamageMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetAbilityDamageMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetMeleeDamageMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetMeleeDamageMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetCriticalHitChanceMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetCriticalHitChanceMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetCriticalHitDamageMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetCriticalHitDamageMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetFiringRateMultiplier(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(
        RequestType::SetFiringRateMultiplier,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetReloadTime(LPVOID argument)
{
    return QueueToggleFloatBridgeRequest(RequestType::SetReloadTime, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetAmmo(LPVOID argument)
{
    return QueueToggleIntBridgeRequest(RequestType::SetAmmo, argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI SetNoAbilityMeleeCooldown(LPVOID argument)
{
    return QueueToggleBridgeRequest(
        RequestType::SetNoAbilityMeleeCooldown,
        argument);
}

extern "C" __declspec(dllexport) DWORD WINAPI GetWeaponAbilityMeleeList(LPVOID argument)
{
    if (!argument)
        return 0;

    auto *buffer = static_cast<WeaponAbilityMeleeListBuffer *>(argument);
    buffer->data[0] = '\0';
    return BridgeResult(QueueRequest(
        RequestType::GetWeaponAbilityMeleeList,
        0,
        0.0f,
        false,
        0,
        buffer->data,
        sizeof(buffer->data)));
}

extern "C" __declspec(dllexport) DWORD WINAPI GetRankList(LPVOID argument)
{
    if (!argument)
        return 0;

    auto *buffer = static_cast<RankListBuffer *>(argument);
    buffer->data[0] = '\0';
    return BridgeResult(QueueRequest(
        RequestType::GetRankList,
        0,
        0.0f,
        false,
        0,
        buffer->data,
        sizeof(buffer->data)));
}

extern "C" __declspec(dllexport) DWORD WINAPI SetWeaponAbilityMeleeRank(LPVOID argument)
{
    if (!argument)
        return 0;

    const auto &value =
        *static_cast<const WeaponAbilityMeleeRankArgument *>(argument);
    return BridgeResult(QueueRequest(
        RequestType::SetWeaponAbilityMeleeRank,
        value.itemId,
        0.0f,
        false,
        value.rank,
        nullptr,
        0,
        value.applyToAll != 0));
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        const HANDLE thread = CreateThread(nullptr, 0, InitializeBridge, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }

    return TRUE;
}
