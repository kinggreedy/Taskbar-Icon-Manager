// ==WindhawkMod==
// @id              taskbar-icon-manager
// @name            Taskbar icon manager
// @description     Assign custom window/taskbar icons by process name, command-line substring, and optional window-title substring.
// @version         0.3
// @author          kinggreedy
// @include         *
// @compilerOptions -lcomctl32 -lshell32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar icon manager

Assign icons to app windows using rules. A rule can match by process name,
command-line substring, and optionally by window-title substring. Rules can also
exclude windows by command-line or title substring.

Icon values can be:

- `C:\\Path\\Icon.ico`
- `C:\\Path\\App.exe,0`
- `C:\\Path\\Library.dll,3`

If more than one rule matches a process, title-specific rules are checked first
for each window. If no title-specific rule matches, the first matching rule with
an empty title is used.

Use `excludeCmdline` and `excludeTitle` to skip matching rules. Multiple exclude
values can be separated with semicolons, for example:

`New Private Tab;Mozilla Firefox Private Browsing`
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- rules:
  - - process: ""
      $name: "Process name"
      $description: "Example: firefox.exe. Leave empty to match any process."
    - cmdline: ""
      $name: "Command-line substring"
      $description: "Leave empty if only the process name should be matched."
    - title: ""
      $name: "Window-title substring"
      $description: "Optional. Used to distinguish windows from the same process."
    - excludeCmdline: ""
      $name: "Exclude command-line substring"
      $description: "Optional. Skip this rule if the command line contains this text. Use semicolons for multiple values."
    - excludeTitle: ""
      $name: "Exclude window-title substring"
      $description: "Optional. Skip this rule for windows whose title contains this text. Use semicolons for multiple values."
    - icon: ""
      $name: "Icon path"
      $description: "An .ico path, or an executable/DLL path with an icon index, for example: app.exe,0."
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace {

struct UserRule {
    std::wstring processName;
    std::wstring commandLinePart;
    std::wstring titlePart;
    std::wstring excludeCommandLineParts;
    std::wstring excludeTitleParts;
    std::wstring iconSpec;
};

struct ActiveRule {
    UserRule match;
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
};

struct WindowIconState {
    HWND hwnd = nullptr;
    HICON originalLargeIcon = nullptr;
    HICON originalSmallIcon = nullptr;
};

struct DelayedWindowJob {
    HWND hwnd = nullptr;
    DWORD generation = 0;
};

std::vector<UserRule> g_rules;
std::vector<ActiveRule> g_activeRules;
std::vector<WindowIconState> g_windowIconStates;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
bool g_active = false;
bool g_unloading = false;
DWORD g_processId = 0;
DWORD g_generation = 1;

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                        int, int, int, HWND, HMENU, HINSTANCE,
                                        LPVOID);
using SetWindowTextW_t = BOOL(WINAPI*)(HWND, LPCWSTR);

CreateWindowExW_t g_originalCreateWindowExW = nullptr;
SetWindowTextW_t g_originalSetWindowTextW = nullptr;

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

bool ContainsInsensitive(const std::wstring& haystack,
                         const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }

    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

std::wstring TrimWhitespace(const std::wstring& value) {
    size_t first = 0;
    while (first < value.length() && std::iswspace(value[first])) {
        ++first;
    }

    size_t last = value.length();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }

    return value.substr(first, last - first);
}

std::vector<std::wstring> SplitSemicolonList(const std::wstring& value) {
    std::vector<std::wstring> parts;
    size_t start = 0;

    while (start <= value.length()) {
        size_t end = value.find(L';', start);
        std::wstring part = value.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);

        part = TrimWhitespace(part);
        if (!part.empty()) {
            parts.push_back(part);
        }

        if (end == std::wstring::npos) {
            break;
        }

        start = end + 1;
    }

    return parts;
}

bool ContainsAnySemicolonValueInsensitive(const std::wstring& haystack,
                                          const std::wstring& semicolonList) {
    for (const std::wstring& item : SplitSemicolonList(semicolonList)) {
        if (ContainsInsensitive(haystack, item)) {
            return true;
        }
    }

    return false;
}

std::wstring ExpandEnvStrings(const std::wstring& value) {
    if (value.empty()) {
        return value;
    }

    DWORD charsNeeded = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (charsNeeded == 0) {
        return value;
    }

    std::wstring expanded(charsNeeded, L'\0');
    DWORD charsWritten = ExpandEnvironmentStringsW(value.c_str(),
                                                   expanded.data(),
                                                   charsNeeded);
    if (charsWritten == 0 || charsWritten > charsNeeded) {
        return value;
    }

    if (!expanded.empty() && expanded.back() == L'\0') {
        expanded.pop_back();
    }

    return expanded;
}

std::wstring CurrentExeNameLower() {
    wchar_t path[MAX_PATH] = {};
    DWORD chars = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (chars == 0) {
        return L"";
    }

    std::wstring fullPath(path, chars);
    size_t slash = fullPath.find_last_of(L"\\/");
    std::wstring fileName =
        slash == std::wstring::npos ? fullPath : fullPath.substr(slash + 1);
    return ToLower(fileName);
}

std::wstring CurrentCommandLine() {
    PCWSTR commandLine = GetCommandLineW();
    return commandLine ? commandLine : L"";
}

std::wstring TakeSetting(PCWSTR name, int index) {
    PCWSTR raw = Wh_GetStringSetting(name, index);
    std::wstring value = raw ? raw : L"";
    Wh_FreeStringSetting(raw);
    return value;
}

std::vector<UserRule> LoadUserRules() {
    std::vector<UserRule> rules;

    for (int i = 0;; ++i) {
        UserRule rule;
        rule.processName = TakeSetting(L"rules[%d].process", i);
        rule.commandLinePart = TakeSetting(L"rules[%d].cmdline", i);
        rule.titlePart = TakeSetting(L"rules[%d].title", i);
        rule.excludeCommandLineParts = TakeSetting(L"rules[%d].excludeCmdline", i);
        rule.excludeTitleParts = TakeSetting(L"rules[%d].excludeTitle", i);
        rule.iconSpec = TakeSetting(L"rules[%d].icon", i);

        bool blankRule = rule.processName.empty() && rule.commandLinePart.empty() &&
                         rule.titlePart.empty() &&
                         rule.excludeCommandLineParts.empty() &&
                         rule.excludeTitleParts.empty() && rule.iconSpec.empty();
        if (blankRule) {
            break;
        }

        if (!rule.iconSpec.empty()) {
            rules.push_back(std::move(rule));
        }
    }

    return rules;
}

bool ParseIconSpec(const std::wstring& spec,
                   std::wstring* filePath,
                   int* iconIndex) {
    *filePath = ExpandEnvStrings(spec);
    *iconIndex = 0;

    size_t comma = filePath->find_last_of(L',');
    if (comma == std::wstring::npos || comma + 1 >= filePath->length()) {
        return !filePath->empty();
    }

    std::wstring possibleIndex = filePath->substr(comma + 1);
    wchar_t* parseEnd = nullptr;
    long parsedIndex = wcstol(possibleIndex.c_str(), &parseEnd, 10);
    if (parseEnd && *parseEnd == L'\0') {
        *iconIndex = static_cast<int>(parsedIndex);
        filePath->erase(comma);
    }

    return !filePath->empty();
}

bool EndsWithIco(const std::wstring& path) {
    const std::wstring lower = ToLower(path);
    return lower.length() >= 4 && lower.substr(lower.length() - 4) == L".ico";
}

HICON LoadIcoAtSize(const std::wstring& path, int edge) {
    return reinterpret_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                             edge, edge, LR_LOADFROMFILE));
}

void LoadIconHandles(const std::wstring& spec, HICON* largeIcon, HICON* smallIcon) {
    *largeIcon = nullptr;
    *smallIcon = nullptr;

    std::wstring path;
    int index = 0;
    if (!ParseIconSpec(spec, &path, &index)) {
        return;
    }

    if (EndsWithIco(path)) {
        *largeIcon = LoadIcoAtSize(path, 48);
        *smallIcon = LoadIcoAtSize(path, 16);
        return;
    }

    HICON extractedLarge = nullptr;
    HICON extractedSmall = nullptr;
    UINT count = ExtractIconExW(path.c_str(), index, &extractedLarge,
                                &extractedSmall, 1);
    if (count > 0) {
        *largeIcon = extractedLarge;
        *smallIcon = extractedSmall;
    }
}

void DestroyLoadedIcons(std::vector<ActiveRule>* rules) {
    for (ActiveRule& rule : *rules) {
        if (rule.largeIcon) {
            DestroyIcon(rule.largeIcon);
            rule.largeIcon = nullptr;
        }

        if (rule.smallIcon) {
            DestroyIcon(rule.smallIcon);
            rule.smallIcon = nullptr;
        }
    }

    rules->clear();
}

bool RuleMatchesThisProcess(const UserRule& rule,
                            const std::wstring& exeNameLower,
                            const std::wstring& commandLine) {
    if (!rule.processName.empty() && ToLower(rule.processName) != exeNameLower) {
        return false;
    }

    if (!rule.commandLinePart.empty() &&
        !ContainsInsensitive(commandLine, rule.commandLinePart)) {
        return false;
    }

    if (!rule.excludeCommandLineParts.empty() &&
        ContainsAnySemicolonValueInsensitive(commandLine,
                                             rule.excludeCommandLineParts)) {
        return false;
    }

    return true;
}

std::vector<ActiveRule> CompileRulesForThisProcess(
    const std::vector<UserRule>& allRules) {
    const std::wstring exeNameLower = CurrentExeNameLower();
    const std::wstring commandLine = CurrentCommandLine();

    std::vector<ActiveRule> compiled;
    for (const UserRule& rule : allRules) {
        if (!RuleMatchesThisProcess(rule, exeNameLower, commandLine)) {
            continue;
        }

        ActiveRule activeRule;
        activeRule.match = rule;
        LoadIconHandles(rule.iconSpec, &activeRule.largeIcon,
                        &activeRule.smallIcon);

        if (activeRule.largeIcon || activeRule.smallIcon) {
            compiled.push_back(std::move(activeRule));
        } else {
            Wh_Log(L"[taskbar-icon-manager] Couldn't load icon: %s",
                   rule.iconSpec.c_str());
        }
    }

    return compiled;
}

bool IsCandidateTopLevelWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == g_processId;
}

std::wstring WindowTitle(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) {
        return L"";
    }

    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) {
        return L"";
    }

    title.resize(static_cast<size_t>(copied));
    return title;
}

const ActiveRule* SelectRuleForWindowLocked(HWND hwnd) {
    if (g_activeRules.empty()) {
        return nullptr;
    }

    bool needsTitle = false;
    for (const ActiveRule& rule : g_activeRules) {
        if (!rule.match.titlePart.empty()) {
            needsTitle = true;
            break;
        }
    }

    if (needsTitle) {
        std::wstring title = WindowTitle(hwnd);
        for (const ActiveRule& rule : g_activeRules) {
            if (!rule.match.excludeTitleParts.empty() &&
                ContainsAnySemicolonValueInsensitive(title,
                                                     rule.match.excludeTitleParts)) {
                continue;
            }

            if (!rule.match.titlePart.empty() &&
                ContainsInsensitive(title, rule.match.titlePart)) {
                return &rule;
            }
        }
    }

    std::wstring title;
    bool titleFetched = false;

    for (const ActiveRule& rule : g_activeRules) {
        if (rule.match.titlePart.empty()) {
            if (!rule.match.excludeTitleParts.empty()) {
                if (!titleFetched) {
                    title = WindowTitle(hwnd);
                    titleFetched = true;
                }

                if (ContainsAnySemicolonValueInsensitive(
                        title, rule.match.excludeTitleParts)) {
                    continue;
                }
            }

            return &rule;
        }
    }

    return nullptr;
}

HICON GetWindowIconOrClassIcon(HWND hwnd, WPARAM whichIcon) {
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, whichIcon, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result) &&
        result) {
        return reinterpret_cast<HICON>(result);
    }

    if (whichIcon == ICON_BIG) {
        return reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
    }

    return reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
}

bool HasSavedWindowStateLocked(HWND hwnd) {
    for (const WindowIconState& state : g_windowIconStates) {
        if (state.hwnd == hwnd) {
            return true;
        }
    }

    return false;
}

void SaveWindowStateIfNeededLocked(HWND hwnd) {
    if (HasSavedWindowStateLocked(hwnd)) {
        return;
    }

    WindowIconState state;
    state.hwnd = hwnd;
    state.originalLargeIcon = GetWindowIconOrClassIcon(hwnd, ICON_BIG);
    state.originalSmallIcon = GetWindowIconOrClassIcon(hwnd, ICON_SMALL);
    g_windowIconStates.push_back(state);
}

void SendIconMessage(HWND hwnd, WPARAM whichIcon, HICON icon) {
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(hwnd, WM_SETICON, whichIcon, reinterpret_cast<LPARAM>(icon),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &ignored);
}

void RefreshWindowFrame(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

void RestoreTrackedWindowIconsLocked() {
    for (const WindowIconState& state : g_windowIconStates) {
        if (!IsWindow(state.hwnd)) {
            continue;
        }

        SendIconMessage(state.hwnd, ICON_BIG, state.originalLargeIcon);
        SendIconMessage(state.hwnd, ICON_SMALL, state.originalSmallIcon);
        RefreshWindowFrame(state.hwnd);
    }

    g_windowIconStates.clear();
}

void ApplyRuleToWindow(HWND hwnd) {
    if (!IsCandidateTopLevelWindow(hwnd)) {
        return;
    }

    EnterCriticalSection(&g_lock);

    if (!g_active || g_unloading) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    const ActiveRule* rule = SelectRuleForWindowLocked(hwnd);
    if (!rule) {
        LeaveCriticalSection(&g_lock);
        return;
    }

    HICON largeIcon = rule->largeIcon;
    HICON smallIcon = rule->smallIcon;
    std::wstring iconSpec = rule->match.iconSpec;

    SaveWindowStateIfNeededLocked(hwnd);

    SendIconMessage(hwnd, ICON_BIG, largeIcon);
    SendIconMessage(hwnd, ICON_SMALL, smallIcon);
    RefreshWindowFrame(hwnd);

    LeaveCriticalSection(&g_lock);

    Wh_Log(L"[taskbar-icon-manager] Applied %s to hwnd=%p", iconSpec.c_str(), hwnd);
}

BOOL CALLBACK ApplyAllWindowsCallback(HWND hwnd, LPARAM) {
    ApplyRuleToWindow(hwnd);
    return TRUE;
}

void ApplyToExistingWindows() {
    EnumWindows(ApplyAllWindowsCallback, 0);
}

DWORD CurrentGeneration() {
    EnterCriticalSection(&g_lock);
    DWORD generation = g_generation;
    LeaveCriticalSection(&g_lock);
    return generation;
}

bool IsGenerationStillCurrent(DWORD generation) {
    EnterCriticalSection(&g_lock);
    bool current = g_active && !g_unloading && generation == g_generation;
    LeaveCriticalSection(&g_lock);
    return current;
}

DWORD WINAPI DelayedWindowRefreshProc(void* param) {
    DelayedWindowJob* job = static_cast<DelayedWindowJob*>(param);

    const DWORD waits[] = {150, 600, 1600, 4000};
    for (DWORD waitMs : waits) {
        Sleep(waitMs);
        if (!IsGenerationStillCurrent(job->generation) || !IsWindow(job->hwnd)) {
            delete job;
            return 0;
        }

        ApplyRuleToWindow(job->hwnd);
    }

    delete job;
    return 0;
}

DWORD WINAPI DelayedProcessRefreshProc(void* param) {
    DWORD generation = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(param));

    const DWORD waits[] = {250, 900, 2200, 5000};
    for (DWORD waitMs : waits) {
        Sleep(waitMs);
        if (!IsGenerationStillCurrent(generation)) {
            return 0;
        }

        ApplyToExistingWindows();
    }

    return 0;
}

void QueueWindowRefresh(HWND hwnd) {
    DWORD generation = CurrentGeneration();
    DelayedWindowJob* job = new DelayedWindowJob{hwnd, generation};
    if (!QueueUserWorkItem(DelayedWindowRefreshProc, job, WT_EXECUTEDEFAULT)) {
        delete job;
    }
}

void QueueProcessRefresh() {
    DWORD generation = CurrentGeneration();
    QueueUserWorkItem(DelayedProcessRefreshProc,
                      reinterpret_cast<void*>(static_cast<ULONG_PTR>(generation)),
                      WT_EXECUTEDEFAULT);
}

void ReplaceActiveRules(std::vector<ActiveRule> newRules) {
    EnterCriticalSection(&g_lock);

    ++g_generation;
    g_active = false;
    RestoreTrackedWindowIconsLocked();
    DestroyLoadedIcons(&g_activeRules);

    g_activeRules = std::move(newRules);
    g_active = !g_activeRules.empty();

    LeaveCriticalSection(&g_lock);

    if (g_active) {
        ApplyToExistingWindows();
        QueueProcessRefresh();
    }
}

void ReloadConfiguration() {
    std::vector<UserRule> loadedRules = LoadUserRules();
    std::vector<ActiveRule> processRules = CompileRulesForThisProcess(loadedRules);

    EnterCriticalSection(&g_lock);
    g_rules = std::move(loadedRules);
    LeaveCriticalSection(&g_lock);

    ReplaceActiveRules(std::move(processRules));
}

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle, LPCWSTR className,
                                 LPCWSTR windowName, DWORD style, int x, int y,
                                 int width, int height, HWND parent,
                                 HMENU menu, HINSTANCE instance, LPVOID param) {
    HWND hwnd = g_originalCreateWindowExW(
        exStyle, className, windowName, style, x, y, width, height, parent, menu,
        instance, param);

    if (hwnd && !(style & WS_CHILD)) {
        EnterCriticalSection(&g_lock);
        bool shouldQueue = g_active && !g_unloading;
        LeaveCriticalSection(&g_lock);

        if (shouldQueue) {
            QueueWindowRefresh(hwnd);
        }
    }

    return hwnd;
}

BOOL WINAPI SetWindowTextW_Hook(HWND hwnd, LPCWSTR text) {
    BOOL result = g_originalSetWindowTextW(hwnd, text);

    if (result && hwnd) {
        EnterCriticalSection(&g_lock);
        bool shouldQueue = g_active && !g_unloading;
        LeaveCriticalSection(&g_lock);

        if (shouldQueue && IsCandidateTopLevelWindow(hwnd)) {
            QueueWindowRefresh(hwnd);
        }
    }

    return result;
}

void InstallHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    void* createWindowExW = reinterpret_cast<void*>(
        GetProcAddress(user32, "CreateWindowExW"));
    if (createWindowExW) {
        Wh_SetFunctionHook(createWindowExW,
                           reinterpret_cast<void*>(CreateWindowExW_Hook),
                           reinterpret_cast<void**>(&g_originalCreateWindowExW));
    }

    void* setWindowTextW = reinterpret_cast<void*>(
        GetProcAddress(user32, "SetWindowTextW"));
    if (setWindowTextW) {
        Wh_SetFunctionHook(setWindowTextW,
                           reinterpret_cast<void*>(SetWindowTextW_Hook),
                           reinterpret_cast<void**>(&g_originalSetWindowTextW));
    }
}

}  // namespace

BOOL Wh_ModInit() {
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
    g_processId = GetCurrentProcessId();

    ReloadConfiguration();
    InstallHooks();

    return TRUE;
}

void Wh_ModBeforeUninit() {
    if (!g_lockReady) {
        return;
    }

    EnterCriticalSection(&g_lock);
    g_unloading = true;
    g_active = false;
    ++g_generation;
    RestoreTrackedWindowIconsLocked();
    LeaveCriticalSection(&g_lock);
}

void Wh_ModUninit() {
    if (!g_lockReady) {
        return;
    }

    EnterCriticalSection(&g_lock);
    RestoreTrackedWindowIconsLocked();
    DestroyLoadedIcons(&g_activeRules);
    g_rules.clear();
    LeaveCriticalSection(&g_lock);

    DeleteCriticalSection(&g_lock);
    g_lockReady = false;
}

void Wh_ModSettingsChanged() {
    ReloadConfiguration();
}
