# Connectivity testing

How to report a car, dock, receiver or DAC that doesn't work with PodBox - or
one that does. Follow the section for your accessory; each one points to the
shared steps under [How to](#how-to).

Working accessories are worth an issue too - just the accessory, your iPod and
what you tried.

## Issues with cars

1. [Note what happened](#note-what-happened)
2. [Capture the iPod's log](#capture-the-ipods-log)
3. If you have an iPhone that works with the car,
   [capture its log](#capture-an-iphones-log) as well. **Turn CarPlay off
   first.** A car running CarPlay talks to the iPhone differently from the way
   it talks to the iPod, so the log would be no use. Go to
   `Settings > Screen Time > Content & Privacy Restrictions`, switch it on,
   then under `Allowed Apps & Features` turn **CarPlay** off. Turn it back on
   after the test.
4. [Send it in](#send-it-in)

## Issues with docks

1. [Note what happened](#note-what-happened)
2. Turn on `Settings > System > Show Debug Menu`
3. [Capture the iPod's log](#capture-the-ipods-log). While docked, take a photo
   of `System > Debug > Serial iAP`.
4. For a dock that connects over USB, and only if you have an iPhone that works
   with it, [capture the iPhone's log](#capture-an-iphones-log) as well.
5. [Send it in](#send-it-in), with the photo.

## Other issues

Receivers, DACs and anything else.

1. [Note what happened](#note-what-happened)
2. [Capture the iPod's log](#capture-the-ipods-log)
3. [Send it in](#send-it-in)

## How to

### Note what happened

- The accessory's make, model and year
- Your iPod model and the PodBox build (`System > System Info`)
- What you did and what the accessory showed - a photo of its screen helps

### Capture the iPod's log

1. Turn on `Settings > Settings Mode` **Everything**, then
   `Settings > System > Debug Logs > USB and Accessory Log`.
2. Connect to the accessory and repeat the problem
3. Copy `.rockbox/logs/usb.log` off the iPod (and `usb.1.log` to `usb.4.log`,
   the older ones, if there are any), then turn `USB and Accessory Log` off - it slows every USB
   connection. If you forget, it switches itself off after
   `Settings > System > Debug Logs > Switch Off After`, 24 hours by default.

### Capture an iPhone's log

It shows what the accessory expects to hear. This part is more technical.

**Install Apple's iAP profile - in Safari.** iOS only installs a profile that
Safari downloaded.

1. In Safari, open Apple's
   [Profiles and Logs](https://developer.apple.com/bug-reporting/profiles-and-logs/)
   page, sign in with any Apple ID and download the **iAP** profile
2. In Settings, tap **Profile Downloaded**, then **Install**. Restart if asked

The profile expires after a few days.

**Capture:**

1. Connect the iPhone to the accessory and repeat what you did with the iPod
2. Press **both volume buttons and the side button** together for a second. The
   phone vibrates
3. After about ten minutes, find the newest `sysdiagnose_` in
   `Settings > Privacy & Security > Analytics & Improvements > Analytics Data`

**Send only the file needed.** The sysdiagnose is hundreds of megabytes and full
of personal data - only one small file inside it is needed.

1. Share it to a computer (AirDrop, or Save to Files then iCloud Drive)
2. Unpack it - double-click on a Mac, or `tar -xzf sysdiagnose_<name>.tar.gz` on
   Windows
3. Take `sysdiagnose_<name>/crashes_and_spins/accessoryd-packets-<date>.log.ips`

### Send it in

[Open an issue](https://github.com/anthonyfletcher/podbox/issues/new) with your
notes, and drag in a zip of `usb.log`, the `accessoryd-packets` file and any
photos.

Can't unpack the sysdiagnose? Say so in the issue rather than posting it
publicly, and a private route will be arranged.
