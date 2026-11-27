Saved beemu machines, copied from ~/Library/Application Support/beemu on the old Mac (2026-11-25).

To restore on a new Mac:
    mkdir -p ~/Library/Application\ Support/beemu/VMs
    cp saved-machines/*.beemuvm ~/Library/Application\ Support/beemu/VMs/
    cp saved-machines/last-vm ~/Library/Application\ Support/beemu/

The hda lines are absolute paths (/Users/drake3d/Desktop/BeOS/Emulator/beemu/images/...).
If the project folder or user name differs on the new Mac, open the machine in the launcher
(Edit...) and choose the disk again, or edit the hda line in the .beemuvm file.
