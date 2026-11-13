# Alt+Tab Anywhere
Pressing Alt+Tab shows the switcher on the primary display. This mod can show it on the monitor where the cursor is, or on a specific monitor, and can limit the list to the windows on that monitor.

Alt+Tab and Win+Alt+Tab have separate settings.

## Specific monitor
Enter the monitor as either:

- A number: 1, 2, 3, ...
- A device name: \\.\DISPLAY2 (or just DISPLAY2)

In the Advanced settings JSON, the value must be quoted ("2", not 2).

Monitor numbers follow the order Windows enumerates monitors in, which may not match the numbers in Settings > System > Display, and can change after sleep, reconnecting a monitor or a driver update. Device names are usually more stable.

To see the number and device name of each monitor, enable debug logging for this mod in Windhawk and check the log. The monitors are listed when the mod loads and whenever its settings change.

If the specified monitor isn't found, the primary monitor is used. "Primary" means whichever monitor Windows currently treats as primary, which other mods can change.
