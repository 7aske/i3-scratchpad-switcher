# i3-scratchpad-switcher

`i3-scratchpad-switcher` is a lightweight GTK3 window switcher for the i3 scratchpad.
It opens a centered grid of scratchpad windows, supports keyboard navigation and filtering, and focuses the selected window through native i3 IPC.

## Features

- Native i3 IPC (no `i3-msg` shell calls)
- Xresources-driven styling (colors, font, alpha) via Xlib
- Keyboard-first navigation (`Tab`, arrows, `h/j/k/l`, `Enter`, `Esc`)
- Search/filter by window title, class, or instance
- Single-instance lock to avoid duplicate overlays

## Dependencies

Runtime:
- i3 window manager

Build:
- C toolchain (`cc`, `make`)
- `pkg-config`
- `gtk+-3.0`
- `gio-2.0`
- `json-glib-1.0`
- `x11` (libX11)

Arch Linux example:

```bash
sudo pacman -S base-devel gtk3 json-glib libx11 pkgconf
```

## Build

```bash
make
```

## Install

Default install location is `/usr/local/bin`.

```bash
./scripts/install.sh
```

Install under a custom prefix:

```bash
./scripts/install.sh --prefix "$HOME/.local"
```

Package/staging install:

```bash
./scripts/install.sh --destdir /tmp/pkgroot
```

You can also use `make` directly:

```bash
make install PREFIX="$HOME/.local"
make uninstall PREFIX="$HOME/.local"
```

## Usage

```bash
i3-scratchpad-switcher
```

## i3 key binding

Add this to your i3 config:

```i3
bindsym $mod+Tab exec --no-startup-id i3-scratchpad-switcher
```

## Contributing

Issues and pull requests are welcome.
Please run `make` before submitting changes.

## License

This project is licensed under the MIT License. See `LICENSE`.
