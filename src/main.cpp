#include <conio.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int KEYCODE_ENTER = 66;
constexpr int KEYCODE_DEL = 67;
constexpr int KEYCODE_TAB = 61;
constexpr int KEYCODE_SPACE = 62;
constexpr int KEYCODE_BACK = 4;
constexpr int KEYCODE_APOSTROPHE = 75;
constexpr int KEYCODE_DPAD_UP = 19;
constexpr int KEYCODE_DPAD_DOWN = 20;
constexpr int KEYCODE_DPAD_LEFT = 21;
constexpr int KEYCODE_DPAD_RIGHT = 22;
constexpr int KEYCODE_FORWARD_DEL = 112;
constexpr int KEYCODE_MOVE_HOME = 122;
constexpr int KEYCODE_MOVE_END = 123;
constexpr int KEYCODE_APP_SWITCH = 187;

constexpr wchar_t CTRL_C = 0x03;
constexpr UINT TOGGLE_KEY = 'K';
constexpr UINT QUIT_KEY = 'Q';

/**
 * Asks ToUnicodeEx not to change the keyboard state, so translating a key here leaves dead keys pending for no one
 * (Windows 10 1607 and newer).
 */
constexpr UINT TRANSLATE_WITHOUT_STATE_CHANGE = 0x4;

const std::unordered_map<DWORD, int> KEY_EVENTS = {
    {VK_RETURN, KEYCODE_ENTER},
    {VK_BACK, KEYCODE_DEL},
    {VK_TAB, KEYCODE_TAB},
    {VK_SPACE, KEYCODE_SPACE},
    {VK_ESCAPE, KEYCODE_BACK},
    {VK_UP, KEYCODE_DPAD_UP},
    {VK_DOWN, KEYCODE_DPAD_DOWN},
    {VK_LEFT, KEYCODE_DPAD_LEFT},
    {VK_RIGHT, KEYCODE_DPAD_RIGHT},
    {VK_DELETE, KEYCODE_FORWARD_DEL},
    {VK_HOME, KEYCODE_MOVE_HOME},
    {VK_END, KEYCODE_MOVE_END},
};

/**
 * How the device's virtual keyboard map types a character outside ASCII: an Alt combination, which is either a dead
 * accent key followed by the base letter, or the character itself for ç.
 */
struct AccentedKey {
    const char *altKey;
    const char *letterKey;
    bool shift;
};

/**
 * One accent: the dead key typing it on the device, the dead character Windows reports for it, and the letters it
 * combines with.
 */
struct Accent {
    const char *deviceDeadKey;
    wchar_t windowsDeadChar;
    const wchar_t *lower;
    const wchar_t *upper;
    const wchar_t *letters;
};

const Accent ACCENTS[] = {
    {"KEYCODE_GRAVE", L'`', L"àèìòù", L"ÀÈÌÒÙ", L"aeiou"},
    {"KEYCODE_E", L'´', L"áéíóúý", L"ÁÉÍÓÚÝ",
     L"aeiouy"},
    {"KEYCODE_I", L'^', L"âêîôû", L"ÂÊÎÔÛ", L"aeiou"},
    {"KEYCODE_U", L'¨', L"äëïöüÿ", L"ÄËÏÖÜŸ",
     L"aeiouy"},
    {"KEYCODE_N", L'~', L"ãñõ", L"ÃÑÕ", L"ano"},
};

const char *letterKey(wchar_t letter) {
    static const char *const KEYS[] = {
        "KEYCODE_A", "KEYCODE_B", "KEYCODE_C", "KEYCODE_D", "KEYCODE_E", "KEYCODE_F", "KEYCODE_G",
        "KEYCODE_H", "KEYCODE_I", "KEYCODE_J", "KEYCODE_K", "KEYCODE_L", "KEYCODE_M", "KEYCODE_N",
        "KEYCODE_O", "KEYCODE_P", "KEYCODE_Q", "KEYCODE_R", "KEYCODE_S", "KEYCODE_T", "KEYCODE_U",
        "KEYCODE_V", "KEYCODE_W", "KEYCODE_X", "KEYCODE_Y", "KEYCODE_Z",
    };
    return KEYS[letter - L'a'];
}

std::unordered_map<wchar_t, AccentedKey> buildAccentedKeys() {
    std::unordered_map<wchar_t, AccentedKey> keys;
    for (const Accent &accent : ACCENTS) {
        for (size_t i = 0; accent.letters[i] != 0; ++i) {
            keys[accent.lower[i]] = {accent.deviceDeadKey, letterKey(accent.letters[i]), false};
            keys[accent.upper[i]] = {accent.deviceDeadKey, letterKey(accent.letters[i]), true};
        }
    }
    keys[L'ç'] = {"KEYCODE_C", nullptr, false};
    keys[L'Ç'] = {"KEYCODE_C", nullptr, true};
    return keys;
}

const std::unordered_map<wchar_t, AccentedKey> ACCENTED_KEYS = buildAccentedKeys();

/**
 * The character a Windows dead key followed by a letter produces, or 0 when they do not combine.
 */
wchar_t compose(wchar_t deadChar, wchar_t letter) {
    for (const Accent &accent : ACCENTS) {
        if (accent.windowsDeadChar != deadChar) {
            continue;
        }
        for (size_t i = 0; accent.letters[i] != 0; ++i) {
            if (accent.letters[i] == letter) {
                return accent.lower[i];
            }
            if (accent.letters[i] - L'a' + L'A' == letter) {
                return accent.upper[i];
            }
        }
    }
    return 0;
}

/**
 * One adb shell kept open for the whole session, so each key costs a write instead of a new adb process.
 */
class AdbShell {
public:
    bool open(const std::string &serial) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;

        HANDLE readEnd = nullptr;
        if (!CreatePipe(&readEnd, &input_, &attributes, 0)) {
            return false;
        }
        SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = readEnd;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::string command = "adb";
        if (!serial.empty()) {
            command += " -s " + serial;
        }
        command += " shell";

        const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                            &startup, &process_);
        CloseHandle(readEnd);
        return started == TRUE;
    }

    void send(const std::string &line) {
        const std::string data = line + "\n";
        DWORD written = 0;
        WriteFile(input_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    }

    void keyEvent(int code) {
        send("input keyevent " + std::to_string(code));
    }

    void ctrlLetter(wchar_t letter) {
        send(std::string("input keycombination KEYCODE_CTRL_LEFT ") + letterKey(letter));
    }

    void text(wchar_t character) {
        if (character == L'\'') {
            keyEvent(KEYCODE_APOSTROPHE);
            return;
        }
        std::string escaped;
        if (character == L'"' || character == L'\\' || character == L'$' || character == L'`') {
            escaped += '\\';
        }
        escaped += static_cast<char>(character);
        send("input text \"" + escaped + "\"");
    }

    void accented(const AccentedKey &key) {
        const std::string shift = key.shift ? " KEYCODE_SHIFT_LEFT" : "";
        if (key.letterKey == nullptr) {
            send("input keycombination KEYCODE_ALT_LEFT" + shift + " " + key.altKey);
            return;
        }
        send(std::string("input keycombination KEYCODE_ALT_LEFT ") + key.altKey);
        if (key.shift) {
            send(std::string("input keycombination KEYCODE_SHIFT_LEFT ") + key.letterKey);
        } else {
            send(std::string("input keyevent ") + key.letterKey);
        }
    }

    void close() {
        if (input_ == nullptr) {
            return;
        }
        send("exit");
        CloseHandle(input_);
        input_ = nullptr;
        WaitForSingleObject(process_.hProcess, 5000);
        CloseHandle(process_.hProcess);
        CloseHandle(process_.hThread);
    }

    ~AdbShell() {
        close();
    }

private:
    HANDLE input_ = nullptr;
    PROCESS_INFORMATION process_{};
};

/**
 * Runs adb writes off the keyboard hook. Windows drops a low-level hook that takes too long to return, so the hook
 * only queues what to send.
 */
class Sender {
public:
    explicit Sender(AdbShell &shell) : shell_(shell), worker_([this] { run(); }) {
    }

    ~Sender() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_one();
        worker_.join();
    }

    void post(std::function<void(AdbShell &)> action) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            actions_.push_back(std::move(action));
        }
        ready_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void(AdbShell &)> action;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !actions_.empty(); });
                if (actions_.empty()) {
                    return;
                }
                action = std::move(actions_.front());
                actions_.pop_front();
            }
            action(shell_);
        }
    }

    AdbShell &shell_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void(AdbShell &)>> actions_;
    bool stopping_ = false;
    std::thread worker_;
};

Sender *sender = nullptr;
DWORD mainThreadId = 0;
std::atomic<bool> forwarding{false};
bool shiftDown = false;
bool ctrlDown = false;
bool altDown = false;
bool altGrDown = false;
bool capsLock = false;
wchar_t pendingDeadChar = 0;

void echo(wchar_t character) {
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &character, 1, &written, nullptr);
}

void printStatus() {
    std::printf(forwarding ? "\n[ON]  Keys go to the phone. Ctrl+Alt+K to stop.\n"
                           : "\n[OFF] Keys stay on this PC. Ctrl+Alt+K to type on the phone.\n");
}

/**
 * Tracks the modifiers from the hook itself: keys swallowed here never reach the system key state.
 */
bool trackModifier(DWORD vk, bool down) {
    switch (vk) {
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_SHIFT:
        shiftDown = down;
        return true;
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_CONTROL:
        ctrlDown = down;
        return true;
    case VK_LMENU:
    case VK_MENU:
        altDown = down;
        return true;
    case VK_RMENU:
        altGrDown = down;
        return true;
    case VK_CAPITAL:
        if (down && forwarding) {
            capsLock = !capsLock;
        }
        return true;
    default:
        return false;
    }
}

/**
 * Translates a key with the current Windows layout, so an AZERTY or any other keyboard types what it shows. Returns
 * -1 for a dead key, the number of characters otherwise.
 */
int translate(const KBDLLHOOKSTRUCT &key, wchar_t &character) {
    BYTE state[256] = {};
    if (shiftDown) {
        state[VK_SHIFT] = 0x80;
    }
    if (capsLock) {
        state[VK_CAPITAL] = 0x01;
    }
    if (altGrDown) {
        state[VK_CONTROL] = 0x80;
        state[VK_MENU] = 0x80;
        state[VK_RMENU] = 0x80;
    }

    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    wchar_t buffer[4] = {};
    const int count = ToUnicodeEx(key.vkCode, key.scanCode, state, buffer, 4, TRANSLATE_WITHOUT_STATE_CHANGE, layout);
    character = buffer[0];
    return count;
}

void sendCharacter(wchar_t character) {
    const auto accented = ACCENTED_KEYS.find(character);
    if (accented != ACCENTED_KEYS.end()) {
        const AccentedKey key = accented->second;
        sender->post([key](AdbShell &shell) { shell.accented(key); });
        echo(character);
        return;
    }
    if (character < 0x20 || character > 0x7E) {
        std::printf("\n[skipped U+%04X: no key types it on the phone]\n", static_cast<unsigned>(character));
        return;
    }
    sender->post([character](AdbShell &shell) { shell.text(character); });
    echo(character);
}

void forwardKey(const KBDLLHOOKSTRUCT &key) {
    if (altDown && key.vkCode == VK_TAB) {
        pendingDeadChar = 0;
        sender->post([](AdbShell &shell) { shell.keyEvent(KEYCODE_APP_SWITCH); });
        return;
    }

    const auto event = KEY_EVENTS.find(key.vkCode);
    if (event != KEY_EVENTS.end()) {
        if (pendingDeadChar != 0 && key.vkCode == VK_SPACE) {
            sendCharacter(pendingDeadChar);
            pendingDeadChar = 0;
            return;
        }
        pendingDeadChar = 0;
        const int code = event->second;
        sender->post([code](AdbShell &shell) { shell.keyEvent(code); });
        if (key.vkCode == VK_RETURN) {
            echo(L'\n');
        } else if (key.vkCode == VK_SPACE) {
            echo(L' ');
        }
        return;
    }

    if (ctrlDown && !altGrDown) {
        if (key.vkCode >= 'A' && key.vkCode <= 'Z') {
            const wchar_t letter = static_cast<wchar_t>(L'a' + (key.vkCode - 'A'));
            sender->post([letter](AdbShell &shell) { shell.ctrlLetter(letter); });
        }
        return;
    }
    if (altDown) {
        return;
    }

    wchar_t character = 0;
    const int count = translate(key, character);
    if (count < 0) {
        pendingDeadChar = character;
        return;
    }
    if (count == 0) {
        return;
    }

    if (pendingDeadChar != 0) {
        const wchar_t composed = compose(pendingDeadChar, character);
        if (composed != 0) {
            character = composed;
        } else {
            sendCharacter(pendingDeadChar);
        }
        pendingDeadChar = 0;
    }
    sendCharacter(character);
}

LRESULT CALLBACK keyboardHook(int code, WPARAM message, LPARAM data) {
    if (code != HC_ACTION) {
        return CallNextHookEx(nullptr, code, message, data);
    }

    const auto &key = *reinterpret_cast<const KBDLLHOOKSTRUCT *>(data);
    if (key.flags & LLKHF_INJECTED) {
        return CallNextHookEx(nullptr, code, message, data);
    }

    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (trackModifier(key.vkCode, down)) {
        return forwarding ? 1 : CallNextHookEx(nullptr, code, message, data);
    }

    if (down && ctrlDown && (altDown || altGrDown)) {
        if (key.vkCode == TOGGLE_KEY) {
            forwarding = !forwarding;
            pendingDeadChar = 0;
            printStatus();
            return 1;
        }
        if (key.vkCode == QUIT_KEY) {
            PostThreadMessage(mainThreadId, WM_QUIT, 0, 0);
            return 1;
        }
    }

    if (!forwarding) {
        return CallNextHookEx(nullptr, code, message, data);
    }
    if (down) {
        forwardKey(key);
    }
    return 1;
}

constexpr const char *WIRELESS_PORT = "5555";

/**
 * Runs a command and returns what it printed, or false when it could not be started or exited with an error.
 */
bool runCommand(const std::string &command, std::string &output) {
    output.clear();
    FILE *pipe = _popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    return _pclose(pipe) == 0;
}

struct Device {
    std::string serial;
    std::string state;
};

/**
 * The devices adb sees, with their state: "device" when usable, "unauthorized" or "offline" otherwise. Returns
 * false when adb itself could not be run.
 */
bool listDevices(std::vector<Device> &devices) {
    devices.clear();
    FILE *output = _popen("adb devices 2>nul", "r");
    if (output == nullptr) {
        return false;
    }

    char line[512];
    bool sawHeader = false;
    while (std::fgets(line, sizeof(line), output) != nullptr) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        if (text.rfind("List of devices", 0) == 0) {
            sawHeader = true;
            continue;
        }
        const size_t tab = text.find('\t');
        if (tab != std::string::npos) {
            devices.push_back({text.substr(0, tab), text.substr(tab + 1)});
        }
    }
    return _pclose(output) == 0 && sawHeader;
}

void printConnectionGuide() {
    std::printf(
        "\nNo Android device is connected.\n"
        "\n"
        "Over USB\n"
        "  1. On the phone, open Settings > About phone > Software information and tap Build number 7 times\n"
        "     to unlock the developer options.\n"
        "  2. In Settings > Developer options, turn on USB debugging.\n"
        "  3. Plug the phone into this PC. If it asks for a USB mode, pick File transfer.\n"
        "  4. Accept the \"Allow USB debugging?\" prompt on the phone.\n"
        "\n"
        "Wireless (Android 11 or newer, phone and PC on the same Wi-Fi)\n"
        "  1. In Developer options, turn on Wireless debugging and open it.\n"
        "  2. Tap \"Pair device with pairing code\", then run on this PC:\n"
        "       adb pair <IP>:<pairing port>      and type the code shown on the phone\n"
        "  3. Back on the Wireless debugging screen, note the IP address and port, then run:\n"
        "       adb connect <IP>:<port>\n"
        "\n"
        "Wireless (any version, with the cable once)\n"
        "  1. Connect the phone over USB as above, with the phone on the same Wi-Fi as this PC.\n"
        "  2. Run:  KeyboardToAndroid.exe --wireless\n"
        "     It switches the phone to wireless debugging and connects to it. You can then unplug the cable.\n"
        "\n"
        "Waiting for a device... (Ctrl+C to quit)\n");
}

/**
 * Waits until a usable device is connected and picks it, or keeps the serial given on the command line. With several
 * devices and no serial, it lists them and stops, since adb would not know which one to type on.
 */
bool chooseDevice(std::string &serial) {
    bool guideShown = false;
    bool unauthorizedShown = false;
    for (;;) {
        std::vector<Device> devices;
        if (!listDevices(devices)) {
            std::printf("Could not run adb. Install the Android platform tools and add adb to PATH.\n");
            return false;
        }

        std::vector<std::string> ready;
        bool unauthorized = false;
        for (const Device &device : devices) {
            if (device.state == "device") {
                ready.push_back(device.serial);
            } else if (device.state == "unauthorized") {
                unauthorized = true;
            }
        }

        if (!serial.empty()) {
            for (const std::string &candidate : ready) {
                if (candidate == serial) {
                    return true;
                }
            }
        } else if (ready.size() == 1) {
            serial = ready.front();
            std::printf("Using device %s.\n", serial.c_str());
            return true;
        } else if (ready.size() > 1) {
            std::printf("Several devices are connected. Pass the one to use as an argument:\n");
            for (const std::string &candidate : ready) {
                std::printf("  KeyboardToAndroid.exe %s\n", candidate.c_str());
            }
            return false;
        }

        if (unauthorized && !unauthorizedShown) {
            std::printf("A device is connected but not authorized: accept the USB debugging prompt on the phone.\n");
            unauthorizedShown = true;
        } else if (!serial.empty() && !guideShown) {
            std::printf("Device %s is not connected yet.\n", serial.c_str());
        }
        if (!guideShown) {
            printConnectionGuide();
            guideShown = true;
        }

        if (_kbhit() && _getwch() == CTRL_C) {
            return false;
        }
        Sleep(2000);
    }
}

/**
 * The phone's IPv4 address on Wi-Fi, read from its wlan0 interface.
 */
std::string wifiAddress(const std::string &serial) {
    std::string output;
    if (!runCommand("adb -s " + serial + " shell ip -f inet addr show wlan0", output)) {
        return "";
    }
    const size_t inet = output.find("inet ");
    if (inet == std::string::npos) {
        return "";
    }
    const size_t start = inet + 5;
    const size_t end = output.find('/', start);
    return end == std::string::npos ? "" : output.substr(start, end - start);
}

/**
 * Switches a USB device to wireless debugging and connects to it, replacing serial with its IP:port.
 */
bool switchToWireless(std::string &serial) {
    if (serial.find(':') != std::string::npos) {
        std::printf("%s is already connected wirelessly.\n", serial.c_str());
        return true;
    }

    const std::string address = wifiAddress(serial);
    if (address.empty()) {
        std::printf("The phone has no Wi-Fi address. Connect it to the same Wi-Fi as this PC and try again.\n");
        return false;
    }

    std::string output;
    std::printf("Switching %s to wireless debugging on port %s...\n", serial.c_str(), WIRELESS_PORT);
    if (!runCommand("adb -s " + serial + " tcpip " + WIRELESS_PORT, output)) {
        std::printf("adb tcpip failed: %s", output.c_str());
        return false;
    }

    const std::string target = address + ":" + WIRELESS_PORT;
    for (int attempt = 0; attempt < 5; ++attempt) {
        Sleep(1500);
        if (runCommand("adb connect " + target, output) && output.find("connected to") != std::string::npos) {
            serial = target;
            std::printf("Connected to %s over Wi-Fi. You can unplug the cable.\n", target.c_str());
            return true;
        }
    }
    std::printf("Could not connect to %s: %s", target.c_str(), output.c_str());
    return false;
}

}

int main(int argc, char **argv) {
    std::string serial;
    bool wireless = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--wireless") {
            wireless = true;
        } else {
            serial = argument;
        }
    }

    if (!chooseDevice(serial)) {
        return 1;
    }
    if (wireless && !switchToWireless(serial)) {
        return 1;
    }

    AdbShell shell;
    if (!shell.open(serial)) {
        std::printf("Could not start adb. Is it in PATH?\n");
        return 1;
    }

    {
        Sender queue(shell);
        sender = &queue;
        mainThreadId = GetCurrentThreadId();

        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
        if (hook == nullptr) {
            std::printf("Could not install the keyboard hook (error %lu).\n", GetLastError());
            return 1;
        }

        std::printf("KeyboardToAndroid works from any window.\n"
                    "  Ctrl+Alt+K  type on the phone / back to the PC\n"
                    "  Ctrl+Alt+Q  quit\n"
                    "While on, Esc is the phone's Back button and Alt+Tab opens its recent apps.\n");
        printStatus();

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        UnhookWindowsHookEx(hook);
        sender = nullptr;
    }

    shell.close();
    std::printf("\nKeyboardToAndroid stopped.\n");
    return 0;
}
