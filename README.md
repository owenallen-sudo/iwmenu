# iwmenu

iwmenu is a tool governed by the MPL v. 2.0 license. It acts like NetworkManager's nmtui (without the overhead from NetworkManager or scanning for networks), acting as a TUI menu for native iwd. Written in C using ncurses (~1170 lines), it allows manual configuration of SSID, passphrase, MTU, DNS, BSSID, and device name. The C source code is provided in this repository.

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
This project is licensed under the Mozilla Public License 2.0. Any modified version of my iwmenu.c file that gets distributed must remain under this license and stay open-source due to legal rights. Please see the LICENSE file for the full terms. 
