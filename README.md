# Stackcomp (WIP Name)
A stacking, tiling, and scrolling hybrid compositor built from stratch with WL-Roots

# Current feature set
* stacking, tiling, scrooling mode for application windows
* keybinds with conditionals for each
* tiling and floating window rules found via regex for the application window

# Current protocal support
* wlr_layershell (tested with swaybg & waybar)
* wlr_screencopy (tested with grim)
* ext_workspaces (tested with waybar)
* zwlr_foreign_toplevel_manager_v1 (tested with waybar)

# Current repo structure
* \[config]: defult config for the compositor
* \[testing]: testing files for the compositor

# Deps
`xorg-xwayland` Runtime: Xwayland support
