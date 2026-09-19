# File Explorer App — Design Spec

## Style
- Dark blue gradient background (#0A1628 → #0D47A1)
- Semi-transparent panel cards (rgba(13,71,161, 0.4), radius 12)
- White text, muted text (#80CBC4 accent teal)
- Fonts: lv_font_cyr_14 (titles), lv_font_cyr_12 (items), lv_font_cyr_10 (hints)
- PS Vita LiveArea aesthetic: rounded cards, clean spacing, minimal borders

## Layout (320x240)
```
┌──────────────────────────────────────┐
│ 📁 Проводник          [Flash] [SD]   │  32px header
├──────────────────────────────────────┤
│ /path/to/current/dir                 │  20px breadcrumb
├──────────────────────────────────────┤
│ 📁 ..                                │
│ 📁 photos                           │
│ 📄 readme.txt          1.2 KB       │  scrollable list
│ 🖼 photo.jpg           245 KB       │
│ 🔧 config.json         0.5 KB       │
│                                      │
├──────────────────────────────────────┤
│ ▲▼ select  ◄► open/back  ◄► source  │  16px hint bar
└──────────────────────────────────────┘
```

## Navigation
- UP/DOWN: scroll through file list
- LEFT: go back (parent directory), if at root → show source picker
- RIGHT: enter directory / open file info
- Long OK: exit app

## Source Selection
- At root level, LEFT cycles between Internal Flash and SD Card
- Sources shown as tabs in header, active one highlighted in teal
- If SD card not mounted, show error message

## File Icons
- Folder: LV_SYMBOL_DIRECTORY (📁)
- .txt/.log/.csv/.json/.xml: LV_SYMBOL_FILE (📄)
- .jpg/.jpeg/.png/.bmp/.gif: LV_SYMBOL_IMAGE (🖼)
- .mp3/.wav/.ogg: LV_SYMBOL_AUDIO (🔊)
- .mp4/.avi/.mov: LV_SYMBOL_VIDEO (🎬)
- Unknown: LV_SYMBOL_EDIT (🔧)
- Parent (..): LV_SYMBOL_PREV (◀)

## File Info
- Directories show item count "N files"
- Files show human-readable size (B, KB, MB)

## Filesystem Support
- Internal: LittleFS (/littlefs mount point)
- SD: SD_MMC 1-bit mode (GPIO 38/39/40, 20MHz)
- Both use POSIX-like API (File class)
