# CamTune

Keeps a USB webcam's picture settings (exposure, backlight compensation, brightness, gamma, mains frequency...) the way
you want them on macOS, with no app open.

macOS drives USB webcams with its own built-in driver, which never adjusts these settings. A laptop camera that relies on
its Windows driver to set them looks dark on macOS. CamTune runs quietly in the background, finds the webcam, and sets
each control to your value (in percent of the camera's own range). It checks again every 3 seconds, so the settings come
back after sleep, after replugging and after an app resets them. A control is only written when it differs.

Default settings (made for a dim laptop camera): mains frequency 50 Hz, auto exposure that may lower the frame rate in dim
light, backlight compensation 100 %, brightness 60 %, gamma 65 %.

## Build
Push this folder to a GitHub repository, open Actions > "CamTune build" > Run workflow, and download the `CamTune`
artifact (camtune, install.sh, uninstall.sh, README.md).

## Install (no sudo)
1. Unzip the artifact and open Terminal in that folder (`cd ~/Downloads/CamTune`).
2. `bash install.sh`
   It installs camtune for your user, starts it at every login, and prints the camera's controls.

## Change the settings
- `~/Library/Application\ Support/CamTune/camtune --set brightness=70` (saved and applied at once), or
- edit `~/Library/Application Support/CamTune/camtune.conf`; changes apply within 3 seconds.
- See ranges, defaults and current values: `~/Library/Application\ Support/CamTune/camtune --list`

Settings: `powerline` (50hz/60hz/off/auto), `exposure` (auto or manual:<percent>), `autopriority` (1/0), `backlight`,
`brightness`, `gamma`, `gain`, `contrast`, `saturation`, `sharpness` (percent or `default`).

## Remove
`bash uninstall.sh`

Log: `~/Library/Logs/camtune.log`
