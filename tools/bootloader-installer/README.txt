PodBox bootloader
=================

With this bootloader the player starts PodBox even when the hold switch is
on. To start Apple's firmware instead, hold Menu while the player starts.

On an iPod Classic whose disk is larger than 128 GB, Apple's firmware is
started only if it can address the whole disk. If it cannot, the screen says
"OF does not support LBA48" and stops. Holding Menu and Left together while
the player starts, until the screen says "Executing OF", starts it anyway,
at the risk of it writing to the wrong part of the disk.

The bootloader is installed once. Updating PodBox does not replace it.


iPod Video 5G/5.5G
------------------

1. Connect the iPod to the computer so that it shows up as a drive.
2. Run install-5g.cmd, and let it make changes when Windows asks.


iPod Classic 6G/7G
------------------

This needs Apple's iPod driver, which comes with iTunes or Apple Devices.

1. Run install-6g.cmd. It waits for the iPod.
2. Connect the iPod and put it in DFU mode: hold Menu and Select until the
   screen goes dark, then straight away hold Select and Play. The screen
   stays black, and the script carries on by itself.

If the script never carries on, Windows has no driver for the iPod in DFU
mode: install iTunes or Apple Devices and try again.


Going back
----------

Rockbox Utility installs the standard Rockbox bootloader over this one.
