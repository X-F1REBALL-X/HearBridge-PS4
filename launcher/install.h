/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HB_LAUNCHER_INSTALL_H
#define HB_LAUNCHER_INSTALL_H
/* Each field: 1 changed now, 0 already fine, -1 failed. */
struct install_result {
    int plugin;      /* game_tap / boot / daemon / usbd copies (worst of them) */
    int plugins_ini; /* [default] lines in /data/GoldHEN/plugins.ini */
    int loader;      /* PluginLoader_Enabled in /data/GoldHEN/config.ini */
};
void install_all(void (*logf)(const char *fmt, ...), struct install_result *r);
#endif
