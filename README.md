# Taskbar Icon Rules

A Windhawk mod that assigns custom icons to application windows based on process name, command-line substring, and optional window-title substring.

This is useful for apps that run multiple profiles or instances under the same executable, such as Firefox, Chrome, Edge, or Electron apps.

## Features

* Match windows by process name.
* Match instances by command-line substring.
* Optionally match specific windows by title substring.
* Exclude rules by command-line substring.
* Exclude windows by title substring.
* Supports `.ico` files.
* Supports icons from `.exe` and `.dll` files using `path,index` syntax.

## Example

```yaml
- rules:
  - - process: "firefox.exe"
    - cmdline: "Profile 1"
    - title: ""
    - excludeCmdline: ""
    - excludeTitle: "Mozilla Firefox Private Browsing"
    - icon: "C:\\Icons\\firefox-profile1.ico"
```

This applies `firefox-profile1.ico` to Firefox windows launched with a command line containing `Profile 1`, except windows whose title contains `Mozilla Firefox Private Browsing`.

## Settings

### `process`

The executable name to match.

Example:

```yaml
process: "firefox.exe"
```

Leave empty to match any process.

### `cmdline`

A substring that must appear in the process command line.

Example:

```yaml
cmdline: "Profile 1"
```

Leave empty if the process name is enough.

### `title`

An optional window-title substring.

Example:

```yaml
title: "Work"
```

If multiple rules match the same process, title-specific rules are checked first. If no title-specific rule matches, the first matching rule with an empty `title` is used.

### `excludeCmdline`

Optional command-line substring that prevents the rule from matching.

Multiple values can be separated with semicolons:

```yaml
excludeCmdline: "private;temporary"
```

### `excludeTitle`

Optional window-title substring that prevents the rule from applying to a window.

Multiple values can be separated with semicolons:

```yaml
excludeTitle: "Mozilla Firefox Private Browsing;New Private Tab"
```

### `icon`

Path to the icon to apply.

Examples:

```yaml
icon: "C:\\Icons\\profile.ico"
icon: "C:\\Program Files\\Example\\app.exe,0"
icon: "C:\\Windows\\System32\\shell32.dll,3"
```

## Firefox profile example

Firefox profile command lines can include a long profile path, such as:

```text
--profile "...\\randomstring.Profile 1" --profile-activate
```

For this case, matching the readable profile name is usually enough:

```yaml
cmdline: "Profile 1"
```

To avoid applying the profile icon to private windows, use:

```yaml
excludeTitle: "Mozilla Firefox Private Browsing"
```

## Compatibility note

If you are using the **"Disable grouping on the taskbar"** Windhawk mod, enable its **"Use window icons"** option.

Without that option, the taskbar may keep showing the application icon instead of the custom per-window icon.

## Current limitations

* Matching is substring-based, not regex-based.
* Title matching depends on the current window title, so apps that frequently change titles may need broader or multiple title values.

## Acknowledgment

This project is inspired, however, an independent implementation of [taskbar icon changer for Windhawk](https://gist.github.com/pongo/d9beaa142fe7d1f898ffe5c0ed24e15d).