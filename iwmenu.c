/*******************************************************************************
This Source code Form is subject to the terms of the Mozilla Public License, v. 2.0.
If a copy of the MPL was not distributed with this file, You can obtain one at
http://mozilla.org/MPL/2.0/.
 * iwmenu.c  —  nmtui-style TUI WiFi manager for pure iwd on Debian
 *
 * DESCRIPTION
 *   Replaces NetworkManager entirely.  Stores WiFi profiles in
 *   /etc/iwmenu/networks.conf, writes iwd profile files to /var/lib/iwd/,
 *   writes systemd-networkd configs to /etc/systemd/network/, and drives
 *   connections via iwctl.  On reboot iwd auto-connects to the last-used
 *   network because that profile has AutoConnect=true.
 *
 * COMPILE
 *   gcc -Wall -Wextra -o iwmenu iwmenu.c -lncurses
 *
 * RUN  (requires root — writes to /var/lib/iwd/ and /etc/systemd/network/)
 *   sudo ./iwmenu
 *
 * FIRST-TIME SETUP  (run once after install)
 *   sudo systemctl disable NetworkManager   # stop NM
 *   sudo systemctl enable  --now iwd        # start/enable iwd
 *   sudo systemctl enable  --now systemd-networkd  # IP/DHCP manager
 *
 * APT DEPENDENCIES
 *   build-essential   — gcc, make, binutils
 *   libncurses-dev    — ncurses headers + libncurses.so  (-lncurses)
 *   iwd               — wireless daemon + iwctl command
 *   (systemd-networkd is part of the systemd package, already installed)
 *
 * ARCHITECTURE
 *   iwd              → 802.11 authentication only (WPA2 handshake)
 *   systemd-networkd → DHCP, DNS, MTU  (IP layer)
 *   /etc/iwd/main.conf                 → global iwd settings
 *   /var/lib/iwd/<SSID>.psk            → per-network iwd profile
 *   /etc/systemd/network/25-<dev>.network → per-interface networkd config
 *   /etc/iwmenu/networks.conf          → our full settings database
 ******************************************************************************/

/* ── Standard C library headers ──────────────────────────────────────────── */
#include <ncurses.h>   /* Terminal UI: initscr, newwin, wgetch, box, …        */
#include <stdio.h>     /* File I/O: fopen, fclose, fgets, fprintf, snprintf   */
#include <stdlib.h>    /* Process: exit, system; memory: malloc (unused here) */
#include <string.h>    /* strcpy, strcmp, strlen, strncpy, memmove, memset    */
#include <stdarg.h>    /* Variadic args: va_list, va_start, va_end, vsnprintf */
#include <unistd.h>    /* POSIX: access, getuid, unlink                       */
#include <sys/stat.h>  /* mkdir, chmod, stat, S_ISDIR                         */
#include <sys/types.h> /* POSIX types: uid_t, mode_t                          */
#include <errno.h>     /* errno global + EEXIST constant                      */
#include <ctype.h>     /* isspace, isprint — whitespace/printable tests       */

/* ── Compile-time constants ──────────────────────────────────────────────── */
#define MAX_NETWORKS   64           /* Maximum number of saved profiles        */
#define MAX_STR        256          /* Maximum length of a generic string field*/
#define CONFIG_DIR     "/etc/iwmenu"                   /* Our working directory*/
#define CONFIG_FILE    "/etc/iwmenu/networks.conf"     /* Profile database file*/
#define LAST_CONN_FILE "/etc/iwmenu/last_connected"   /* Last-connected record */
#define IWD_DIR        "/var/lib/iwd"                 /* iwd profile store    */
#define IWD_CONF_DIR   "/etc/iwd"                     /* iwd config directory */
#define IWD_MAIN_CONF  "/etc/iwd/main.conf"           /* iwd daemon config    */
#define NETWORKD_DIR   "/etc/systemd/network"         /* networkd config dir  */

/* Field defaults — applied when the user leaves a field blank */
#define DEFAULT_DEVICE "wlan0"    /* Wireless interface name                  */
#define DEFAULT_DNS1   "8.8.8.8" /* Primary DNS resolver (Google Public DNS) */
#define DEFAULT_MTU    "1360"    /* MTU in bytes — safe for VPN/PPPoE paths  */

/* ncurses colour-pair IDs (registered via init_pair) */
#define CP_NORMAL    1   /* White on black   — body text                      */
#define CP_HILITE    2   /* Black on cyan    — highlighted / selected row     */
#define CP_TITLE     3   /* Black on blue    — window title bar               */
#define CP_CONNECTED 4   /* Green on black   — "● connected" status marker   */
#define CP_BTN       5   /* Black on white   — normal button                  */
#define CP_BTN_SEL   6   /* Black on cyan    — focused button                 */
#define CP_FIELD     7   /* Black on white   — editable text field            */

/* ── Data structures ─────────────────────────────────────────────────────── */

/*
 * NetworkProfile — stores every configurable parameter for one WiFi network.
 * All fields are fixed-size char arrays; no heap allocation needed.
 */
typedef struct {
    char ssid[MAX_STR];          /* 802.11 SSID — the visible network name    */
    char password[MAX_STR];      /* WPA2 passphrase; empty string = open net  */
    char device[64];             /* Interface name: wlan0, wlp2s0, etc.       */
    char dns_primary[64];        /* Primary DNS IP, e.g. "8.8.8.8"           */
    char dns_secondary[64];      /* Secondary DNS IP; empty = none            */
    char mtu[16];                /* MTU as decimal string, e.g. "1360"        */
    char device_mac[18];         /* Spoof our NIC's MAC; empty = use real MAC */
    char bssid[18];              /* Lock to this AP MAC; empty = any AP       */
} NetworkProfile;

/* ── Global state ────────────────────────────────────────────────────────── */
static NetworkProfile profiles[MAX_NETWORKS]; /* All saved WiFi profiles      */
static int            nprofiles    = 0;       /* Count of saved profiles      */
static int            connected_idx = -1;     /* Index of active connection   */

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 1 — Utility helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * trim — remove all leading and trailing ASCII whitespace from string s.
 * Modifies s in-place.  No return value.
 */
static void trim(char *s)
{
    int i = 0;
    /* Count leading whitespace characters */
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    /* Shift the non-space content to the beginning of the buffer */
    if (i > 0)
        memmove(s, s + i, strlen(s) - i + 1);
    /* Walk backwards from end, replacing whitespace with null terminator */
    int j = (int)strlen(s);
    while (j > 0 && isspace((unsigned char)s[j - 1]))
        s[--j] = '\0';
}

/*
 * ensure_dir — create directory at path if it does not already exist.
 * Uses mode 0755 (rwxr-xr-x).
 * Returns 0 on success, -1 on a real error (errno set by mkdir).
 */
static int ensure_dir(const char *path)
{
    struct stat st;
    /* stat() returns 0 if the path exists; S_ISDIR checks it is a directory */
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;                           /* Already exists — nothing to do */
    /* mkdir returns -1 on failure; EEXIST means another process beat us to it*/
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;                          /* Real failure — propagate error  */
    return 0;
}

/*
 * run_silent — execute a printf-formatted shell command, discarding all output.
 * Stdout and stderr are redirected to /dev/null so ncurses is not clobbered.
 * Returns the exit status code of the command (0 = success).
 */
static int run_silent(const char *fmt, ...)
{
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);                        /* Begin reading variadic args   */
    vsnprintf(cmd, sizeof(cmd), fmt, ap);     /* Build command string          */
    va_end(ap);                               /* Clean up variadic state       */
    /* Append redirection to suppress output from reaching the terminal */
    strncat(cmd, " >/dev/null 2>&1",
            sizeof(cmd) - strlen(cmd) - 1);
    return system(cmd);                       /* Execute via /bin/sh -c        */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 2 — Config file I/O  (/etc/iwmenu/networks.conf)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * write_config — serialise profiles[] to CONFIG_FILE in INI-block format.
 *
 * File format (one [network] block per profile):
 *   [network]
 *   ssid=My Network
 *   password=secret
 *   device=wlan0
 *   dns_primary=8.8.8.8
 *   dns_secondary=
 *   mtu=1360
 *   device_mac=
 *   bssid=
 *
 * Called after every add or remove so the file is always up-to-date.
 */
static void write_config(void)
{
    int i;
    ensure_dir(CONFIG_DIR);               /* Create /etc/iwmenu if missing    */
    FILE *f = fopen(CONFIG_FILE, "w");    /* Open for writing; truncates file */
    if (!f) return;                       /* Cannot open — skip silently      */
    for (i = 0; i < nprofiles; i++) {    /* Iterate every saved profile      */
        NetworkProfile *p = &profiles[i];
        fprintf(f, "[network]\n");
        fprintf(f, "ssid=%s\n",          p->ssid);
        fprintf(f, "password=%s\n",      p->password);
        fprintf(f, "device=%s\n",        p->device);
        fprintf(f, "dns_primary=%s\n",   p->dns_primary);
        fprintf(f, "dns_secondary=%s\n", p->dns_secondary);
        fprintf(f, "mtu=%s\n",           p->mtu);
        fprintf(f, "device_mac=%s\n",    p->device_mac);
        fprintf(f, "bssid=%s\n",         p->bssid);
        fprintf(f, "\n");                /* Blank line separates blocks       */
    }
    fclose(f);                           /* Flush kernel buffer and close     */
}

/*
 * read_config — deserialise CONFIG_FILE into profiles[].
 * Silently returns if the file does not yet exist (first run).
 * Resets nprofiles to 0 before loading.
 */
static void read_config(void)
{
    char line[512];                      /* Buffer for one file line          */
    char key[128], val[MAX_STR];         /* Parsed key and value strings      */
    NetworkProfile *cur = NULL;          /* Pointer to the block being built  */

    FILE *f = fopen(CONFIG_FILE, "r");   /* Open read-only                   */
    if (!f) return;                      /* File absent = first run, OK      */

    nprofiles = 0;                       /* Reset global count before loading */

    while (fgets(line, sizeof(line), f)) {   /* Read line by line            */
        trim(line);                           /* Remove surrounding whitespace */
        if (line[0] == '#' || line[0] == '\0')
            continue;                         /* Skip comments + blank lines  */

        if (strcmp(line, "[network]") == 0) { /* Start of a new profile block */
            if (nprofiles < MAX_NETWORKS) {
                cur = &profiles[nprofiles++]; /* Advance to next free slot    */
                memset(cur, 0, sizeof(*cur)); /* Zero all fields in the slot  */
            } else {
                cur = NULL;                   /* Limit reached; discard block */
            }
            continue;
        }

        if (!cur) continue;               /* No active block yet — skip line  */

        /* Split "key=value" on the first '=' character */
        char *eq = strchr(line, '=');
        if (!eq) continue;                /* Malformed line — skip            */
        *eq = '\0';                       /* Null-terminate key portion       */
        /* Copy key and value into separate buffers, guard against overflow  */
        strncpy(key, line,   sizeof(key) - 1); key[sizeof(key)-1] = '\0';
        strncpy(val, eq + 1, sizeof(val) - 1); val[sizeof(val)-1] = '\0';
        trim(key); trim(val);             /* Strip any lingering whitespace   */

        /* Assign parsed value to the correct field of the current profile   */
        if      (!strcmp(key, "ssid"))          strncpy(cur->ssid,          val, MAX_STR-1);
        else if (!strcmp(key, "password"))       strncpy(cur->password,      val, MAX_STR-1);
        else if (!strcmp(key, "device"))         strncpy(cur->device,        val, 63);
        else if (!strcmp(key, "dns_primary"))    strncpy(cur->dns_primary,   val, 63);
        else if (!strcmp(key, "dns_secondary"))  strncpy(cur->dns_secondary, val, 63);
        else if (!strcmp(key, "mtu"))            strncpy(cur->mtu,           val, 15);
        else if (!strcmp(key, "device_mac"))     strncpy(cur->device_mac,    val, 17);
        else if (!strcmp(key, "bssid"))          strncpy(cur->bssid,         val, 17);
    }
    fclose(f);
}

/*
 * write_last_connected — persist the SSID of the most recently connected
 * network to LAST_CONN_FILE.  This is informational; actual auto-connect
 * on reboot is handled by iwd's AutoConnect=true in the profile file.
 */
static void write_last_connected(const char *ssid)
{
    ensure_dir(CONFIG_DIR);
    FILE *f = fopen(LAST_CONN_FILE, "w");
    if (!f) return;
    fprintf(f, "%s\n", ssid);           /* One SSID per file, newline-ended  */
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 3 — iwd profile file generation  (/var/lib/iwd/)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * write_iwd_profile — create or overwrite the iwd network profile for p.
 *
 * iwd reads profiles from /var/lib/iwd/.  Each file is named after the
 * SSID with a .psk extension for WPA2 or .open for unencrypted networks.
 *
 * IMPORTANT: iwd refuses to load .psk files that are world-readable.
 * We chmod the file to 0600 (owner read/write only) after writing.
 *
 * Profile file structure:
 *   [Security]
 *   Passphrase=<password>       ← WPA2 only (.psk files)
 *
 *   [Settings]
 *   AutoConnect=true            ← iwd will connect this on boot
 *   BSSID=xx:xx:xx:xx:xx:xx    ← optional; requires iwd >= 1.14
 */
static void write_iwd_profile(const NetworkProfile *p)
{
    char path[600];
    /* Determine file extension: open networks use .open, WPA2 uses .psk    */
    int is_open = (p->password[0] == '\0');

    /* Build the full path, e.g. /var/lib/iwd/MyNetwork.psk                 */
    snprintf(path, sizeof(path), "%s/%s.%s",
             IWD_DIR, p->ssid, is_open ? "open" : "psk");

    ensure_dir(IWD_DIR);                /* /var/lib/iwd must exist            */

    FILE *f = fopen(path, "w");         /* Create/overwrite the profile file  */
    if (!f) return;

    if (!is_open) {                     /* WPA2: write [Security] section     */
        fprintf(f, "[Security]\n");
        fprintf(f, "Passphrase=%s\n", p->password);   /* The WPA2 passphrase  */
        fprintf(f, "\n");
    }

    fprintf(f, "[Settings]\n");
    /* AutoConnect=true tells iwd to connect this network on startup        */
    fprintf(f, "AutoConnect=true\n");

    /* Optional: lock iwd to a specific BSSID (AP's MAC address)            */
    if (p->bssid[0] != '\0')
        fprintf(f, "BSSID=%s\n", p->bssid);

    fclose(f);

    /* Set permissions to 0600 (-rw-------) — required by iwd for .psk     */
    if (!is_open)
        chmod(path, 0600);
}

/*
 * delete_iwd_profile — remove the iwd profile file(s) for the given SSID.
 * Tries both extensions (.psk and .open) since we don't know which exists.
 * unlink() silently fails with ENOENT if the file is not found — that is OK.
 */
static void delete_iwd_profile(const char *ssid)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.psk",  IWD_DIR, ssid);
    unlink(path);                       /* Remove .psk file; ignore ENOENT   */
    snprintf(path, sizeof(path), "%s/%s.open", IWD_DIR, ssid);
    unlink(path);                       /* Remove .open file; ignore ENOENT  */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 4 — systemd-networkd config generation
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * write_networkd_config — write a .network file for systemd-networkd.
 *
 * systemd-networkd handles DHCP, assigns the IP address, configures DNS,
 * and sets the MTU.  iwd only does the 802.11 handshake; once it has
 * associated with the AP, networkd takes over for IP layer setup.
 *
 * File location: /etc/systemd/network/25-<device>.network
 * (The "25-" prefix controls priority among multiple .network files.)
 *
 * File format:
 *   [Match]
 *   Name=wlan0                  ← apply only to this interface
 *
 *   [Network]
 *   DHCP=ipv4                   ← request an IP via DHCPv4
 *   DNS=8.8.8.8                 ← primary resolver
 *   DNS=8.8.4.4                 ← secondary resolver (if provided)
 *
 *   [DHCP]
 *   UseDNS=false                ← ignore DNS servers pushed by DHCP server
 *   UseMTU=false                ← ignore MTU advertised by DHCP server
 *
 *   [Link]
 *   MTUBytes=1360               ← hard-set MTU on the interface
 */
static void write_networkd_config(const NetworkProfile *p)
{
    char path[256];
    /* Filename includes device name so each interface gets its own file    */
    snprintf(path, sizeof(path), "%s/25-%s.network", NETWORKD_DIR, p->device);
    ensure_dir(NETWORKD_DIR);           /* Ensure /etc/systemd/network exists */

    FILE *f = fopen(path, "w");
    if (!f) return;

    /* [Match] — tells networkd which NIC this file applies to              */
    fprintf(f, "[Match]\n");
    fprintf(f, "Name=%s\n", p->device);  /* Exact interface name match       */
    fprintf(f, "\n");

    /* [Network] — IP layer settings                                         */
    fprintf(f, "[Network]\n");
    fprintf(f, "DHCP=ipv4\n");                        /* Enable DHCPv4       */
    fprintf(f, "DNS=%s\n", p->dns_primary);           /* Primary DNS server  */
    if (p->dns_secondary[0] != '\0')                  /* Only if provided    */
        fprintf(f, "DNS=%s\n", p->dns_secondary);     /* Secondary DNS server*/
    fprintf(f, "\n");

    /* [DHCP] — prevent the DHCP server from overriding our settings        */
    fprintf(f, "[DHCP]\n");
    fprintf(f, "UseDNS=false\n");   /* Keep our explicit DNS, ignore DHCP's */
    fprintf(f, "UseMTU=false\n");   /* Keep our explicit MTU, ignore DHCP's */
    fprintf(f, "\n");

    /* [Link] — low-level interface settings                                 */
    fprintf(f, "[Link]\n");
    fprintf(f, "MTUBytes=%s\n", p->mtu);  /* Set MTU in bytes on the NIC    */

    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 5 — iwd main.conf  (/etc/iwd/main.conf)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * write_iwd_main_conf — write the global iwd daemon configuration.
 *
 * We set EnableNetworkConfiguration=false because systemd-networkd is
 * responsible for DHCP and IP configuration.  If this were true, iwd would
 * also try to run DHCP and there would be a conflict.
 */
static void write_iwd_main_conf(void)
{
    ensure_dir(IWD_CONF_DIR);           /* Create /etc/iwd if it doesn't exist*/
    FILE *f = fopen(IWD_MAIN_CONF, "w");
    if (!f) return;
    fprintf(f, "[General]\n");
    /* Disable iwd's built-in DHCP client; systemd-networkd handles IP      */
    fprintf(f, "EnableNetworkConfiguration=false\n");
    fclose(f);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 6 — Connection management
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * do_connect — activate the WiFi profile at index idx.
 *
 * Order of operations:
 *  1. Optionally spoof the NIC's MAC address with 'ip link'
 *  2. Write the systemd-networkd .network file for this interface
 *  3. Reload networkd so it picks up the new DNS/MTU config
 *  4. Write (or refresh) the iwd profile file in /var/lib/iwd/
 *  5. Use iwctl to associate with the network
 *  6. Record SSID as last-connected for boot-time reference
 */
static void do_connect(int idx)
{
    if (idx < 0 || idx >= nprofiles) return;  /* Bounds-check the index      */
    NetworkProfile *p = &profiles[idx];

    /* Step 1 — MAC address spoofing (optional) */
    if (p->device_mac[0] != '\0') {
        /* Interface must be down before the kernel allows a MAC change      */
        run_silent("ip link set %s down", p->device);
        /* Set the spoofed MAC address on the interface                     */
        run_silent("ip link set %s address %s", p->device, p->device_mac);
        /* Bring the interface back up so iwd can use it                    */
        run_silent("ip link set %s up", p->device);
    }

    /* Step 2 — write systemd-networkd config for this connection's settings */
    write_networkd_config(p);

    /* Step 3 — reload networkd so it reads the updated .network file       */
    run_silent("networkctl reload");

    /* Step 4 — write (or refresh) the iwd profile file                    */
    write_iwd_profile(p);

    /* Step 5 — iwctl: instruct iwd to associate with the SSID              */
    /* The profile file must already exist for iwd to use the passphrase    */
    run_silent("iwctl station %s connect \"%s\"", p->device, p->ssid);

    /* Step 6 — record which network is active so iwd prioritises it on boot*/
    write_last_connected(p->ssid);

    connected_idx = idx;                  /* Update in-memory connected state */
}

/*
 * do_disconnect — drop the currently active WiFi connection.
 * Uses iwctl to tell iwd to disassociate, then asks networkd to release
 * the IP on that interface.
 */
static void do_disconnect(void)
{
    if (connected_idx < 0) return;       /* Nothing connected — nothing to do */
    NetworkProfile *p = &profiles[connected_idx];

    /* Tell iwd to send a deauthentication frame and disassociate from AP   */
    run_silent("iwctl station %s disconnect", p->device);

    /* Ask networkd to stop managing (and release DHCP lease on) the iface  */
    run_silent("networkctl down %s", p->device);

    connected_idx = -1;                  /* Clear active connection record    */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 7 — ncurses helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * init_colors — register ncurses colour pairs used throughout the TUI.
 * Must be called after initscr() and only when has_colors() returns TRUE.
 * init_pair(id, foreground_colour, background_colour) binds a pair.
 */
static void init_colors(void)
{
    start_color();           /* Activate ncurses colour subsystem             */
    use_default_colors();    /* Allow -1 = terminal's own default colour      */
    init_pair(CP_NORMAL,    COLOR_WHITE,  COLOR_BLACK);  /* Normal body text  */
    init_pair(CP_HILITE,    COLOR_BLACK,  COLOR_CYAN);   /* Selected/focused  */
    init_pair(CP_TITLE,     COLOR_BLACK,  COLOR_BLUE);   /* Title bars        */
    init_pair(CP_CONNECTED, COLOR_GREEN,  COLOR_BLACK);  /* Connected marker  */
    init_pair(CP_BTN,       COLOR_BLACK,  COLOR_WHITE);  /* Normal button     */
    init_pair(CP_BTN_SEL,   COLOR_BLACK,  COLOR_CYAN);   /* Focused button    */
    init_pair(CP_FIELD,     COLOR_BLACK,  COLOR_WHITE);  /* Input field bg    */
}

/*
 * draw_title — draw a border around win and embed title in the top edge.
 * Centres the title text horizontally.
 */
static void draw_title(WINDOW *win, const char *title)
{
    int h, w;
    getmaxyx(win, h, w);                /* Query window dimensions           */
    (void)h;                            /* Height unused here; suppress warn  */
    box(win, 0, 0);                     /* Draw single-line border all around */
    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    int tx = (w - (int)strlen(title) - 2) / 2; /* Centre calculation         */
    if (tx < 1) tx = 1;
    mvwprintw(win, 0, tx, " %s ", title);  /* Print " Title " in top border  */
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
}

/*
 * show_message — display a centred popup message and wait for any key.
 * Used to report errors or confirmations to the user.
 */
static void show_message(const char *msg)
{
    int scr_h, scr_w;
    getmaxyx(stdscr, scr_h, scr_w);    /* Full terminal dimensions           */
    int mlen = (int)strlen(msg);
    int w = mlen + 6;                  /* Window width: message + padding    */
    int h = 5;                         /* Fixed height: border+msg+space+key */
    if (w > scr_w - 2) w = scr_w - 2; /* Clamp to terminal width            */
    int y = (scr_h - h) / 2;          /* Vertically centred                 */
    int x = (scr_w - w) / 2;          /* Horizontally centred               */

    WINDOW *pop = newwin(h, w, y, x);  /* Create the popup window            */
    keypad(pop, TRUE);                 /* Enable special key codes           */
    wbkgd(pop, COLOR_PAIR(CP_NORMAL)); /* Set background colour              */
    draw_title(pop, "Notice");
    mvwprintw(pop, 2, 3, "%.*s", w - 4, msg); /* Print message, clipped     */
    mvwprintw(pop, 3, (w - 16) / 2, "[ Press any key ]");
    wrefresh(pop);                     /* Flush window contents to terminal  */
    wgetch(pop);                       /* Block until user presses a key     */
    delwin(pop);                       /* Destroy the popup window           */
    touchwin(stdscr);                  /* Mark underlying screen as dirty    */
    refresh();                         /* Redraw the base screen             */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 8 — Inline text field editor
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * get_string — interactive single-line text editor rendered inside win.
 *
 * Draws a highlighted text field of 'width' characters at position (y, x)
 * in window win.  Pre-fills it with the contents of 'def' (the default).
 * The user edits it with regular keys and backspace.
 *
 * Keys handled:
 *   Printable ASCII  — append character to buffer
 *   KEY_BACKSPACE/127/8  — delete last character
 *   Enter (\n or \r) — confirm and return 0
 *   ESC (27)         — cancel and return -1 (buf unchanged)
 *
 * Parameters:
 *   win   — ncurses window to draw in
 *   y, x  — position within win
 *   width — visible width of the field (also max input length)
 *   def   — default/initial string (may be empty string "")
 *   buf   — output buffer; filled with final text on confirm
 *   bufsz — size of buf in bytes
 *
 * Returns 0 on Enter (confirm), -1 on ESC (cancel).
 */
static int get_string(WINDOW *win, int y, int x, int width,
                      const char *def, char *buf, int bufsz)
{
    /* Copy default value into a working buffer */
    char tmp[MAX_STR] = {0};
    if (def && def[0])
        strncpy(tmp, def, sizeof(tmp) - 1);

    int len = (int)strlen(tmp);  /* Current length of text in the field     */
    int ch;                      /* Character/key code from wgetch           */

    while (1) {
        /* Draw the field background in CP_FIELD colour */
        wattron(win, COLOR_PAIR(CP_FIELD));
        /* Print a blank field of 'width' spaces to form the background     */
        mvwprintw(win, y, x, "%*s", width, "");
        /* Overlay the current buffer contents on top of the blank field    */
        mvwprintw(win, y, x, "%.*s", width, tmp);
        wattroff(win, COLOR_PAIR(CP_FIELD));

        /* Move the terminal cursor to the end of the typed text            */
        wmove(win, y, x + (len < width ? len : width - 1));
        wrefresh(win);           /* Push changes to screen                  */

        ch = wgetch(win);        /* Wait for a key press                    */

        if (ch == 27) {          /* ESC — abort, leave buf unchanged        */
            return -1;
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            /* Confirm: copy working buffer to output buffer                */
            strncpy(buf, tmp, bufsz - 1);
            buf[bufsz - 1] = '\0';
            return 0;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            /* Backspace: remove last character if any                      */
            if (len > 0)
                tmp[--len] = '\0';
        } else if (isprint(ch) && len < bufsz - 1 && len < width) {
            /* Printable character: append to buffer                        */
            tmp[len++] = (char)ch;
            tmp[len]   = '\0';
        }
        /* Any other key (arrows etc.) is silently ignored                  */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 9 — Generic arrow-key menu
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * run_menu — display a navigable list of items inside a centred window.
 *
 * The user moves the highlight with UP/DOWN arrow keys and confirms with
 * Enter.  ESC cancels and returns -1.
 *
 * Parameters:
 *   title    — text to show in the window title bar
 *   items    — NULL-terminated array of C string pointers to display
 *   nitems   — number of items in the array
 *   win_h    — requested height of the popup window
 *   win_w    — requested width  of the popup window
 *
 * Returns the 0-based index of the selected item, or -1 on ESC.
 *
 * Layout:
 *   ┌─────── Title ───────┐
 *   │                     │
 *   │   Item 0            │
 *   │ ► Item 1  (hilite)  │
 *   │   Item 2            │
 *   │                     │
 *   └─────────────────────┘
 */
static int run_menu(const char *title, const char **items, int nitems,
                    int win_h, int win_w)
{
    int scr_h, scr_w;
    getmaxyx(stdscr, scr_h, scr_w);   /* Full terminal size                  */
    /* Clamp window dimensions to fit within the terminal */
    if (win_h > scr_h - 2) win_h = scr_h - 2;
    if (win_w > scr_w - 2) win_w = scr_w - 2;
    /* Centre the window on screen */
    int y0 = (scr_h - win_h) / 2;
    int x0 = (scr_w - win_w) / 2;

    WINDOW *win = newwin(win_h, win_w, y0, x0); /* Create the menu window    */
    keypad(win, TRUE);                  /* Translate arrow keys to KEY_UP etc */
    wbkgd(win, COLOR_PAIR(CP_NORMAL));  /* Apply normal background colour     */

    int sel = 0;                        /* Currently highlighted item index   */
    int ch;

    while (1) {
        /* Redraw window every iteration so highlight moves cleanly          */
        werase(win);                    /* Clear window contents              */
        draw_title(win, title);         /* Border + title bar                 */

        int i;
        for (i = 0; i < nitems; i++) {
            int row = 2 + i;            /* Items start at row 2 (row 0=border,
                                           row 1 = blank padding)             */
            if (row >= win_h - 1) break;/* Stop before bottom border          */

            if (i == sel) {
                /* Highlight the selected row */
                wattron(win, COLOR_PAIR(CP_HILITE) | A_BOLD);
                /* Print a full-width highlighted bar */
                mvwprintw(win, row, 1, "%-*s", win_w - 2, "");
                mvwprintw(win, row, 2, " %-*s", win_w - 4, items[i]);
                wattroff(win, COLOR_PAIR(CP_HILITE) | A_BOLD);
            } else {
                /* Normal (unselected) row */
                wattron(win, COLOR_PAIR(CP_NORMAL));
                mvwprintw(win, row, 2, " %-*s", win_w - 4, items[i]);
                wattroff(win, COLOR_PAIR(CP_NORMAL));
            }
        }
        wrefresh(win);                  /* Flush to terminal                  */

        ch = wgetch(win);               /* Wait for user input                */

        if (ch == KEY_UP) {
            /* Move selection up, wrapping at the top */
            sel = (sel > 0) ? sel - 1 : nitems - 1;
        } else if (ch == KEY_DOWN) {
            /* Move selection down, wrapping at the bottom */
            sel = (sel < nitems - 1) ? sel + 1 : 0;
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            /* Confirm selection */
            delwin(win);
            return sel;
        } else if (ch == 27) {          /* ESC — cancel */
            delwin(win);
            return -1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 10 — Add / Edit connection form
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * run_add_form — show a form to fill in all fields for one NetworkProfile.
 *
 * Navigation:
 *   UP / DOWN arrows — move between fields and buttons
 *   ENTER on a field — opens the inline text editor for that field
 *   ENTER on OK      — validates mandatory fields and returns 0
 *   ENTER on Cancel  — returns -1
 *   ESC anywhere     — returns -1 (cancel)
 *
 * Parameters:
 *   p     — pointer to the profile to fill in (may be pre-populated for edit)
 *   title — window title string
 *
 * Returns 0 if user pressed OK with valid data, -1 if cancelled.
 *
 * Mandatory field: SSID must be non-empty.
 * Defaults applied automatically:
 *   device      → "wlan0"   if left blank
 *   dns_primary → "8.8.8.8" if left blank
 *   mtu         → "1360"    if left blank
 */
static int run_add_form(NetworkProfile *p, const char *title)
{
    /* ── Form layout constants ── */
    /* Each entry: label text, pointer to the profile field, max field len */
    struct { const char *label; char *field; int maxlen; } fields[] = {
        { "SSID:          ", p->ssid,          MAX_STR - 1 },
        { "Password:      ", p->password,      MAX_STR - 1 },
        { "Device:        ", p->device,        63          },
        { "Primary DNS:   ", p->dns_primary,   63          },
        { "Secondary DNS: ", p->dns_secondary, 63          },
        { "MTU:           ", p->mtu,           15          },
        { "Device MAC:    ", p->device_mac,    17          },
        { "BSSID (AP MAC):", p->bssid,         17          },
    };
    int nfields = (int)(sizeof(fields) / sizeof(fields[0])); /* Count fields */

    /* Total rows in form window:
       1 (top border) + 1 (blank) + nfields rows + 1 (blank) + 1 (buttons)
       + 1 (blank) + 1 (bottom border) */
    int win_h = nfields + 6;
    int win_w = 60;                     /* Fixed width                        */

    int scr_h, scr_w;
    getmaxyx(stdscr, scr_h, scr_w);
    if (win_h > scr_h - 2) win_h = scr_h - 2;
    if (win_w > scr_w - 2) win_w = scr_w - 2;
    int y0 = (scr_h - win_h) / 2;
    int x0 = (scr_w - win_w) / 2;

    WINDOW *win = newwin(win_h, win_w, y0, x0);
    keypad(win, TRUE);
    wbkgd(win, COLOR_PAIR(CP_NORMAL));

    /* Cursor position:
       0 .. nfields-1 = field rows
       nfields         = OK button
       nfields+1       = Cancel button */
    int cur = 0;        /* Currently selected row in the form */
    int ch;

    while (1) {
        werase(win);
        draw_title(win, title);

        int i;
        for (i = 0; i < nfields; i++) {
            int row = 2 + i;             /* Row 0=border, 1=blank, 2=first field*/

            /* Print the field label in normal colour */
            wattron(win, COLOR_PAIR(CP_NORMAL));
            mvwprintw(win, row, 2, "%s", fields[i].label);
            wattroff(win, COLOR_PAIR(CP_NORMAL));

            /* Width of the value area = window width - label - padding */
            int llen   = (int)strlen(fields[i].label);
            int fwidth = win_w - llen - 4; /* 4 = 2 left pad + 2 right pad    */
            if (fwidth < 4) fwidth = 4;

            /* Draw the input field box, highlighted if focused */
            int pair = (cur == i) ? CP_HILITE : CP_FIELD;
            wattron(win, COLOR_PAIR(pair));
            /* Blank field background */
            mvwprintw(win, row, 2 + llen, "%-*s", fwidth, "");
            /* Overlay current field value */
            mvwprintw(win, row, 2 + llen, "%-*.*s",
                      fwidth, fwidth, fields[i].field);
            wattroff(win, COLOR_PAIR(pair));
        }

        /* Draw OK and Cancel buttons on the row after the last field */
        int btn_row = nfields + 3;       /* Buttons row index within win     */
        int ok_x    = win_w / 2 - 10;   /* OK button x position             */
        int can_x   = win_w / 2 + 2;    /* Cancel button x position         */

        /* OK button: highlighted when cur == nfields */
        wattron(win, COLOR_PAIR(cur == nfields ? CP_BTN_SEL : CP_BTN) | A_BOLD);
        mvwprintw(win, btn_row, ok_x, "  < OK >  ");
        wattroff(win, COLOR_PAIR(cur == nfields ? CP_BTN_SEL : CP_BTN) | A_BOLD);

        /* Cancel button: highlighted when cur == nfields+1 */
        wattron(win, COLOR_PAIR(cur == nfields+1 ? CP_BTN_SEL : CP_BTN) | A_BOLD);
        mvwprintw(win, btn_row, can_x, " <Cancel> ");
        wattroff(win, COLOR_PAIR(cur == nfields+1 ? CP_BTN_SEL : CP_BTN) | A_BOLD);

        wrefresh(win);

        ch = wgetch(win);

        if (ch == KEY_UP) {
            /* Move cursor up; wrap around from first row to Cancel button  */
            cur = (cur > 0) ? cur - 1 : nfields + 1;
        } else if (ch == KEY_DOWN) {
            /* Move cursor down; wrap from Cancel back to first field       */
            cur = (cur < nfields + 1) ? cur + 1 : 0;
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (cur < nfields) {
                /* Activate inline editor for this field */
                int llen   = (int)strlen(fields[cur].label);
                int fwidth = win_w - llen - 4;
                if (fwidth < 4) fwidth = 4;
                /* get_string edits the field in place */
                get_string(win, 2 + cur, 2 + llen, fwidth,
                           fields[cur].field,
                           fields[cur].field,
                           fields[cur].maxlen + 1);
            } else if (cur == nfields) {
                /* OK button pressed — apply defaults, validate, confirm    */

                /* Apply defaults for blank fields */
                if (p->device[0] == '\0')
                    strncpy(p->device, DEFAULT_DEVICE, 63);
                if (p->dns_primary[0] == '\0')
                    strncpy(p->dns_primary, DEFAULT_DNS1, 63);
                if (p->mtu[0] == '\0')
                    strncpy(p->mtu, DEFAULT_MTU, 15);

                /* SSID is mandatory */
                if (p->ssid[0] == '\0') {
                    show_message("SSID cannot be empty!");
                } else {
                    delwin(win);
                    return 0;            /* OK — profile is populated         */
                }
            } else {
                /* Cancel button pressed */
                delwin(win);
                return -1;
            }
        } else if (ch == 27) {           /* ESC — cancel immediately          */
            delwin(win);
            return -1;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 11 — TUI screens
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * screen_edit_connections — "Edit a connection" screen.
 *
 * Lists all saved networks + an "< Add a connection >" entry + "< Back >".
 * Selecting a saved network opens an edit form.
 * Selecting Add opens a blank add form.
 * Inside the edit form the user can also delete the connection (via a
 * separate confirmation prompt after they choose "Remove").
 *
 * After adding, the iwd profile and networkd config are written immediately.
 */
static void screen_edit_connections(void)
{
    while (1) {
        /* Build the item list: one entry per saved profile + Add + Back    */
        /* +3 extra slots for "Add", "Remove", "Back" */
        const char *items[MAX_NETWORKS + 3];
        int i;
        char labels[MAX_NETWORKS][MAX_STR + 4]; /* "  SSID name" with marker */

        for (i = 0; i < nprofiles; i++) {
            /* Show a green bullet for the connected network */
            if (i == connected_idx)
                snprintf(labels[i], sizeof(labels[i]), "* %s", profiles[i].ssid);
            else
                snprintf(labels[i], sizeof(labels[i]), "  %s", profiles[i].ssid);
            items[i] = labels[i];
        }
        items[nprofiles]     = "< Add a connection >";
        items[nprofiles + 1] = "< Back >";

        int total = nprofiles + 2;
        int h = total + 5;               /* Window height: items + chrome     */
        if (h < 10) h = 10;

        int sel = run_menu("Edit a connection", items, total, h, 54);
        if (sel < 0 || sel == nprofiles + 1) return; /* Back or ESC          */

        if (sel == nprofiles) {
            /* ── Add a new connection ──────────────────────────── */
            if (nprofiles >= MAX_NETWORKS) {
                show_message("Maximum number of networks reached!");
                continue;
            }
            NetworkProfile np;
            memset(&np, 0, sizeof(np)); /* Start with all fields blank       */
            if (run_add_form(&np, "Add Connection") == 0) {
                profiles[nprofiles++] = np; /* Append to profile array       */
                write_iwd_profile(&np);     /* Create iwd profile file       */
                write_config();             /* Persist our database           */
                show_message("Connection saved.");
            }
        } else {
            /* ── Edit or delete an existing connection ─────────── */
            /* Let user edit the selected profile in a copy          */
            NetworkProfile edited = profiles[sel];
            int action = run_add_form(&edited, "Edit Connection");

            if (action == 0) {
                /* User pressed OK — also offer Remove option        */
                /* We re-use a small sub-menu: Save / Remove / Cancel */
                const char *sub[] = { "Save changes", "Remove connection", "Cancel" };
                int sc = run_menu("Apply changes", sub, 3, 8, 36);
                if (sc == 0) {
                    /* Save: update profile, rewrite files           */
                    /* Remove old iwd profile if SSID changed        */
                    if (strcmp(profiles[sel].ssid, edited.ssid) != 0)
                        delete_iwd_profile(profiles[sel].ssid);
                    profiles[sel] = edited;
                    write_iwd_profile(&edited);
                    write_config();
                    show_message("Connection updated.");
                } else if (sc == 1) {
                    /* Remove: delete files, shift array left        */
                    delete_iwd_profile(profiles[sel].ssid);
                    /* If we're removing the active connection, disconnect */
                    if (sel == connected_idx) {
                        do_disconnect();
                    }
                    /* Adjust connected_idx if it was after the removed one*/
                    if (connected_idx > sel) connected_idx--;
                    /* Shift profiles left by one position           */
                    int j;
                    for (j = sel; j < nprofiles - 1; j++)
                        profiles[j] = profiles[j + 1];
                    nprofiles--;
                    write_config();
                    show_message("Connection removed.");
                }
                /* sc == 2 (Cancel) — do nothing                    */
            }
        }
    }
}

/*
 * screen_activate_connections — "Activate a connection" screen.
 *
 * Lists all saved networks with status markers, plus a "< Back >" option.
 * Selecting a connected network disconnects it.
 * Selecting a disconnected network connects it.
 */
static void screen_activate_connections(void)
{
    while (1) {
        const char *items[MAX_NETWORKS + 2];
        char labels[MAX_NETWORKS][MAX_STR + 80]; /* +80 for device name + markers */
        int i;

        for (i = 0; i < nprofiles; i++) {
            if (i == connected_idx)
                /* Show "● SSID  [device] (connected)" marker */
                snprintf(labels[i], sizeof(labels[i]),
                         "* %-30s [%s]",
                         profiles[i].ssid, profiles[i].device);
            else
                snprintf(labels[i], sizeof(labels[i]),
                         "  %-30s [%s]",
                         profiles[i].ssid, profiles[i].device);
            items[i] = labels[i];
        }

        /* Add a separator-style entry and Back button */
        if (nprofiles == 0) {
            items[0] = "  (no saved connections)";
            items[1] = "< Back >";
            int sel2 = run_menu("Activate a connection", items, 2, 8, 54);
            (void)sel2;
            return;
        }

        items[nprofiles] = "< Back >";
        int total = nprofiles + 1;
        int h = total + 5;
        if (h < 8) h = 8;

        int sel = run_menu("Activate a connection", items, total, h, 58);
        if (sel < 0 || sel == nprofiles) return; /* Back or ESC              */

        if (sel == connected_idx) {
            /* User selected the currently connected network — disconnect    */
            const char *opts[] = { "Disconnect", "Cancel" };
            int c = run_menu("Disconnect?", opts, 2, 7, 30);
            if (c == 0) {
                do_disconnect();
                show_message("Disconnected.");
            }
        } else {
            /* User selected a different network — connect to it             */
            const char *opts[] = { "Connect", "Cancel" };
            int c = run_menu("Connect?", opts, 2, 7, 30);
            if (c == 0) {
                /* If something else is connected, disconnect first          */
                if (connected_idx >= 0) do_disconnect();
                do_connect(sel);
                show_message("Connect command sent. Check status with:\n"
                             "iwctl station wlan0 show");
            }
        }
    }
}

/*
 * screen_main — the top-level menu, equivalent to nmtui's first screen.
 *
 * Options:
 *   Edit a connection    → screen_edit_connections()
 *   Activate a connection → screen_activate_connections()
 *   Quit                 → exit(0)
 */
static void screen_main(void)
{
    const char *items[] = {
        "Edit a connection",       /* Manage saved profiles                  */
        "Activate a connection",   /* Connect / disconnect                   */
        "Quit",                    /* Exit the program                       */
    };
    int nitems = 3;

    while (1) {
        /* Background: fill screen with a subtle title */
        clear();
        wattron(stdscr, COLOR_PAIR(CP_TITLE) | A_BOLD);
        int scr_h, scr_w;
        getmaxyx(stdscr, scr_h, scr_w);
        (void)scr_h;
        mvprintw(0, (scr_w - 24) / 2, "  iwd Network Manager  ");
        wattroff(stdscr, COLOR_PAIR(CP_TITLE) | A_BOLD);
        refresh();

        int sel = run_menu("iwmenu", items, nitems, 10, 40);

        if (sel == 0) {
            screen_edit_connections();
        } else if (sel == 1) {
            screen_activate_connections();
        } else {
            /* Quit or ESC */
            break;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SECTION 12 — Program entry point
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * main — initialise everything and launch the TUI.
 *
 * Steps:
 *  1. Require root (UID 0) — we write to system directories
 *  2. Write /etc/iwd/main.conf (disable iwd's built-in DHCP)
 *  3. Load our saved network profiles from /etc/iwmenu/networks.conf
 *  4. Initialise ncurses
 *  5. Run the main TUI loop
 *  6. Tear down ncurses on exit
 */
int main(void)
{
    /* ── 1. Root check ─────────────────────────────────────────────────── */
    /* getuid() returns 0 only for the root user                            */
    if (getuid() != 0) {
        fprintf(stderr, "iwmenu: must be run as root (sudo ./iwmenu)\n");
        return 1;
    }

    /* ── 2. Write iwd global config ─────────────────────────────────────── */
    /* Do this first so iwd is configured correctly before any connection    */
    write_iwd_main_conf();

    /* ── 3. Load saved profiles ─────────────────────────────────────────── */
    read_config();   /* Fills profiles[] and nprofiles from CONFIG_FILE      */

    /* Attempt to detect which network is currently connected by asking iwctl*/
    /* We run iwctl and redirect output to a temp file, then parse it        */
    {
        FILE *pipe = popen("iwctl station wlan0 show 2>/dev/null", "r");
        if (pipe) {
            char line[256];
            while (fgets(line, sizeof(line), pipe)) {
                /* Look for "Connected network" line in iwctl output         */
                if (strstr(line, "Connected network")) {
                    /* Extract the SSID from the line                        */
                    char *p = strrchr(line, ' ');  /* Last space before SSID */
                    if (p && *(p+1)) {
                        char ssid[MAX_STR] = {0};
                        strncpy(ssid, p + 1, sizeof(ssid) - 1);
                        trim(ssid);                /* Remove trailing newline */
                        /* Find matching profile */
                        int i;
                        for (i = 0; i < nprofiles; i++) {
                            if (strcmp(profiles[i].ssid, ssid) == 0) {
                                connected_idx = i;
                                break;
                            }
                        }
                    }
                }
            }
            pclose(pipe);
        }
    }

    /* ── 4. Initialise ncurses ─────────────────────────────────────────── */
    initscr();           /* Initialise the ncurses library + stdscr window   */
    cbreak();            /* Disable line buffering — get keys immediately     */
    noecho();            /* Don't print typed characters automatically        */
    curs_set(0);         /* Hide the hardware cursor (we draw our own)        */
    keypad(stdscr, TRUE);/* Enable arrow keys / function keys on stdscr      */

    if (has_colors())    /* Only use colours if the terminal supports them    */
        init_colors();

    /* ── 5. Main TUI loop ──────────────────────────────────────────────── */
    screen_main();       /* Runs until the user selects Quit or presses ESC  */

    /* ── 6. Ncurses teardown ───────────────────────────────────────────── */
    endwin();            /* Restore the terminal to its original state        */
    return 0;
}
