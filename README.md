# KeyboardToAndroid

Type on an Android phone with your PC keyboard, from any window, over adb.

## Requirements

- Windows 10 or newer
- [adb](https://developer.android.com/tools/releases/platform-tools) in `PATH`
- USB debugging enabled on the phone

## Usage

Download `KeyboardToAndroid.exe` from the [releases](../../releases) and run it.

| Shortcut | Action |
|---|---|
| `Ctrl+Alt+K` | Start or stop sending keys to the phone |
| `Ctrl+Alt+Q` | Quit |

While sending, keys go to the phone instead of the PC, `Esc` is the phone's Back button and `Alt+Tab` opens its
recent apps. Accented letters
(é, è, à, ç, ê, ë…) are supported.

With no phone connected, the app explains how to connect one and waits for it.

### Wireless

Plug the phone in once, on the same Wi-Fi as the PC, then run:

```
KeyboardToAndroid.exe --wireless
```

You can unplug the cable afterwards. With several devices connected, pass the one to use:

```
KeyboardToAndroid.exe 192.168.1.42:5555
```

## Build

With MSYS2 UCRT64, CMake and Ninja:

```
build.bat
```
