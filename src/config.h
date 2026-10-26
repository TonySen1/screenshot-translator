/* config.h —— 配置项与 config.json 读写 */
#ifndef CONFIG_H
#define CONFIG_H

#include <windows.h>

typedef struct Config {
    wchar_t api_base_url[512];
    wchar_t api_key[512];
    wchar_t model[128];
    wchar_t hotkey[64];
    wchar_t exit_hotkey[64];
    wchar_t target_language[64];

    float   mask_opacity;      /* 0.05 ~ 1.0 */
    float   overlay_opacity;   /* 0.3  ~ 1.0 */
    wchar_t overlay_bg[16];    /* "#1e1e1e" */
    wchar_t overlay_fg[16];    /* "#ffffff" */

    int     font_size;         /* 磅 */
    int     request_timeout;   /* 秒 */
} Config;

/* 配置加载结果 */
typedef enum ConfigLoadStatus {
    CONFIG_LOAD_OK = 0,       /* 正常读取 */
    CONFIG_LOAD_CREATED,      /* 文件不存在，已写入一份默认值 */
    CONFIG_LOAD_CORRUPTED,    /* JSON 损坏，已备份并重写默认值 */
    CONFIG_LOAD_UNREADABLE    /* 文件存在但读不了，本次用默认值 */
} ConfigLoadStatus;

void config_defaults(Config *cfg);

/* 读配置。文件不存在时会自动写一份默认值，然后返回默认配置。
   任何单项非法都只回退该项，不影响其他项。
   文件损坏时备份为 <path>.broken 再写默认值。 */
ConfigLoadStatus config_load(Config *cfg, const wchar_t *path);

/* 原子保存：先写 .tmp，再 MoveFileEx 替换。 */
BOOL config_save(const Config *cfg, const wchar_t *path);

#endif /* CONFIG_H */
