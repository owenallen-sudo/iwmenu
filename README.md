# iwmenu

iwmenu is a tool like NetworkManager's nmtui, acting as a TUI menu for native iwd. Written in C using ncurses (~1170 lines), it allows manual configuration of SSID, passphrase, MTU, DNS, BSSID, and device name. The C source code is provided in this repository.

## Dependencies
```
sudo apt install build-essential libncurses-dev iwd
```

## Compile
```
gcc -Wall -o iwmenu iwmenu.c -lncurses
```

## Run
```
sudo ./iwmenu
```

## First-time setup
```
sudo systemctl disable --now NetworkManager
sudo systemctl mask NetworkManager
sudo systemctl enable --now iwd
sudo systemctl enable --now systemd-networkd
sudo systemctl enable --now systemd-resolved
sudo ln -sf /run/systemd/resolve/stub-resolv.conf /etc/resolv.conf
```
