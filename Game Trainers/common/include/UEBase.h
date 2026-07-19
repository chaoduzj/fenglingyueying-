#pragma once

#include "TrainerBase.h"

#include <Psapi.h>
#include <algorithm>
#include <cstdio>
#include <type_traits>

// Shared trainer-side support for game-specific Unreal Engine bridge DLLs.
//
// UE bridge exports use the Win32 thread-procedure ABI:
//     DWORD WINAPI ExportName(LPVOID argument);
//
// The game-specific DLL is embedded in the trainer as the UE_DLL resource.
class UEBase : public TrainerBase
{
public:
    explicit UEBase(const std::wstring &processIdentifier, bool useWindowTitle = false)
        : TrainerBase(processIdentifier, useWindowTitle)
    {
    }

    ~UEBase() override
    {
        resetInjectionState();
    }

    void cleanUp() override
    {
        resetInjectionState();
        TrainerBase::cleanUp();
    }

protected:
    static inline constexpr const char *DLL_RESOURCE_NAME = "UE_DLL";

    HMODULE localDll = nullptr;
    uintptr_t localModuleBase = 0;
    uintptr_t remoteModuleBase = 0;
    LPVOID remoteArgument = nullptr;
    size_t remoteArgumentSize = 0;
    std::string extractedDllPath;

    std::string extractDllFromResource()
    {
        const HMODULE trainerModule = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceA(trainerModule, DLL_RESOURCE_NAME, MAKEINTRESOURCEA(10));
        if (!resource)
        {
            std::cerr << "[UE] Failed to find embedded resource: " << DLL_RESOURCE_NAME << "\n";
            return {};
        }

        const HGLOBAL loadedResource = LoadResource(trainerModule, resource);
        const void *resourceData = loadedResource ? LockResource(loadedResource) : nullptr;
        const DWORD resourceSize = SizeofResource(trainerModule, resource);
        if (!resourceData || resourceSize == 0)
        {
            std::cerr << "[UE] Failed to load the embedded bridge DLL.\n";
            return {};
        }

        char temporaryDirectory[MAX_PATH] = {};
        if (!GetTempPathA(MAX_PATH, temporaryDirectory))
        {
            std::cerr << "[UE] Failed to resolve the temporary directory.\n";
            return {};
        }

        char temporaryFile[MAX_PATH] = {};
        const int length = snprintf(
            temporaryFile,
            sizeof(temporaryFile),
            "%sUE_%lu.dll",
            temporaryDirectory,
            GetCurrentProcessId());
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(temporaryFile))
        {
            std::cerr << "[UE] Temporary bridge path is too long.\n";
            return {};
        }

        const HANDLE file = CreateFileA(
            temporaryFile,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            std::cerr << "[UE] Failed to create temporary bridge DLL: " << GetLastError() << "\n";
            return {};
        }

        DWORD bytesWritten = 0;
        const bool wroteFile = WriteFile(file, resourceData, resourceSize, &bytesWritten, nullptr) != FALSE;
        CloseHandle(file);

        if (!wroteFile || bytesWritten != resourceSize)
        {
            DeleteFileA(temporaryFile);
            std::cerr << "[UE] Failed to extract the complete bridge DLL.\n";
            return {};
        }

        return temporaryFile;
    }

    bool initializeDllInjection()
    {
        if (localDll && remoteModuleBase)
            return true;

        if (!isProcessRunning())
            return false;

        if (extractedDllPath.empty())
        {
            extractedDllPath = extractDllFromResource();
            if (extractedDllPath.empty())
                return false;
        }

        const size_t pathBytes = extractedDllPath.size() + 1;
        LPVOID remotePath = VirtualAllocEx(
            hProcess,
            nullptr,
            pathBytes,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!remotePath)
        {
            std::cerr << "[UE] Failed to allocate the remote DLL path.\n";
            return false;
        }

        if (!WriteProcessMemory(hProcess, remotePath, extractedDllPath.c_str(), pathBytes, nullptr))
        {
            std::cerr << "[UE] Failed to write the remote DLL path.\n";
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            return false;
        }

        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const FARPROC loadLibrary = kernel32 ? GetProcAddress(kernel32, "LoadLibraryA") : nullptr;
        if (!loadLibrary)
        {
            std::cerr << "[UE] Failed to resolve LoadLibraryA.\n";
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            return false;
        }

        const HANDLE injectionThread = CreateRemoteThread(
            hProcess,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary),
            remotePath,
            0,
            nullptr);
        if (!injectionThread)
        {
            std::cerr << "[UE] Failed to start the bridge injection thread: " << GetLastError() << "\n";
            VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
            return false;
        }

        const DWORD injectionWait = WaitForSingleObject(injectionThread, 10000);
        CloseHandle(injectionThread);
        VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

        if (injectionWait != WAIT_OBJECT_0)
        {
            std::cerr << "[UE] Bridge injection timed out or failed.\n";
            return false;
        }

        if (!findRemoteModule())
        {
            std::cerr << "[UE] The injected bridge module could not be located.\n";
            return false;
        }

        // Map without resolving imports or running DllMain. This mapping is used only
        // to obtain export RVAs; executing the game-specific DLL in the trainer would
        // initialize it against the wrong process.
        localDll = LoadLibraryExA(extractedDllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!localDll)
        {
            std::cerr << "[UE] Failed to map the bridge locally for export lookup.\n";
            return false;
        }

        localModuleBase = reinterpret_cast<uintptr_t>(localDll);
        std::cout << "[UE] Bridge injected at 0x" << std::hex << remoteModuleBase << std::dec << ".\n";
        return true;
    }

    template <typename T>
    bool invokeMethod(const char *functionName, const T &argument)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Remote bridge arguments must be trivially copyable");

        if (!initializeDllInjection())
            return false;

        const FARPROC localFunction = GetProcAddress(localDll, functionName);
        if (!localFunction)
        {
            std::cerr << "[UE] Export not found: " << functionName << "\n";
            return false;
        }

        if (!ensureRemoteArgumentCapacity(sizeof(T)))
            return false;

        if (!WriteProcessMemory(hProcess, remoteArgument, &argument, sizeof(T), nullptr))
        {
            std::cerr << "[UE] Failed to write arguments for " << functionName << ".\n";
            return false;
        }

        const uintptr_t functionRva = reinterpret_cast<uintptr_t>(localFunction) - localModuleBase;
        const auto remoteFunction = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteModuleBase + functionRva);
        const HANDLE callThread = CreateRemoteThread(
            hProcess,
            nullptr,
            0,
            remoteFunction,
            remoteArgument,
            0,
            nullptr);
        if (!callThread)
        {
            std::cerr << "[UE] Failed to invoke " << functionName << ": " << GetLastError() << "\n";
            return false;
        }

        const DWORD callWait = WaitForSingleObject(callThread, 7000);
        DWORD exitCode = 0;
        if (callWait == WAIT_OBJECT_0)
            GetExitCodeThread(callThread, &exitCode);
        CloseHandle(callThread);

        if (callWait != WAIT_OBJECT_0)
        {
            std::cerr << "[UE] " << functionName << " timed out.\n";
            return false;
        }

        if (exitCode == 0)
            std::cerr << "[UE] " << functionName << " was rejected by the game bridge.\n";

        return exitCode != 0;
    }

    // Invokes a bridge export and copies the (possibly modified) argument
    // block back from the game. This is useful for small game-thread queries
    // such as selector lists without introducing a separate IPC channel.
    template <typename T>
    bool invokeMethodReadBack(const char *functionName, T &argument)
    {
        static_assert(std::is_trivially_copyable_v<T>, "Remote bridge arguments must be trivially copyable");

        if (!invokeMethod(functionName, argument))
            return false;

        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                hProcess,
                remoteArgument,
                &argument,
                sizeof(T),
                &bytesRead) ||
            bytesRead != sizeof(T))
        {
            std::cerr << "[UE] Failed to read the result from " << functionName << ".\n";
            return false;
        }

        return true;
    }

private:
    bool findRemoteModule()
    {
        const std::string wantedName = extractedDllPath.substr(extractedDllPath.find_last_of("\\/") + 1);
        const DWORD start = GetTickCount();

        while (GetTickCount() - start < 3000)
        {
            HMODULE modules[1024] = {};
            DWORD bytesNeeded = 0;
            if (EnumProcessModulesEx(hProcess, modules, sizeof(modules), &bytesNeeded, LIST_MODULES_ALL))
            {
                const DWORD moduleCount = (std::min)(bytesNeeded, static_cast<DWORD>(sizeof(modules))) / sizeof(HMODULE);
                for (DWORD index = 0; index < moduleCount; ++index)
                {
                    char modulePath[MAX_PATH] = {};
                    if (!GetModuleFileNameExA(hProcess, modules[index], modulePath, MAX_PATH))
                        continue;

                    const std::string path(modulePath);
                    const std::string name = path.substr(path.find_last_of("\\/") + 1);
                    if (_stricmp(name.c_str(), wantedName.c_str()) == 0)
                    {
                        remoteModuleBase = reinterpret_cast<uintptr_t>(modules[index]);
                        return true;
                    }
                }
            }

            Sleep(50);
        }

        return false;
    }

    bool ensureRemoteArgumentCapacity(size_t requiredSize)
    {
        if (remoteArgument && remoteArgumentSize >= requiredSize)
            return true;

        if (remoteArgument)
        {
            VirtualFreeEx(hProcess, remoteArgument, 0, MEM_RELEASE);
            remoteArgument = nullptr;
            remoteArgumentSize = 0;
        }

        remoteArgument = VirtualAllocEx(
            hProcess,
            nullptr,
            requiredSize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (!remoteArgument)
        {
            std::cerr << "[UE] Failed to allocate remote argument memory.\n";
            return false;
        }

        remoteArgumentSize = requiredSize;
        return true;
    }

    void resetInjectionState()
    {
        if (remoteArgument && hProcess)
            VirtualFreeEx(hProcess, remoteArgument, 0, MEM_RELEASE);

        remoteArgument = nullptr;
        remoteArgumentSize = 0;
        remoteModuleBase = 0;

        if (localDll)
            FreeLibrary(localDll);

        localDll = nullptr;
        localModuleBase = 0;

        if (!extractedDllPath.empty())
            DeleteFileA(extractedDllPath.c_str());

        extractedDllPath.clear();
    }
};
