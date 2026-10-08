# Connectivity testing

How to report a car, dock, receiver or DAC that doesn't work with PodBox - or
one that does.

## 1. Note what happened

- The accessory's make, model and year
- Your iPod model and the PodBox build (`System > System Info`)
- What you did and what the accessory showed - a photo of its screen helps

## 2. Capture the iPod's log

1. Turn on `Settings > Settings Mode` **Everything**, then
   `Settings > System > Show Debug Menu` and
   `Settings > System > USB > Write Debug Log`
2. Delete `.rockbox/logs/usb.log` from the iPod if it's there
3. Connect to the accessory and repeat the problem
4. Copy `.rockbox/logs/usb.log` off the iPod, then turn `Write Debug Log` off -
   it slows every USB connection

If `usb.log` has nothing after its `== USB Log` lines, your dock uses the
serial pins instead. Take a photo of `System > Debug > Serial iAP`
while docked.

## 3. Capture an iPhone's log (cars and USB docks)

Only if you have an iPhone that works with the accessory. It shows what the
accessory expects to hear.  This is a bit more technical...

**Install Apple's iAP profile - in Safari.** iOS only installs a profile that
Safari downloaded.

1. In Safari, open Apple's
   [Profiles and Logs](https://developer.apple.com/bug-reporting/profiles-and-logs/)
   page, sign in with any Apple ID and download the **iAP** profile
2. In Settings, tap **Profile Downloaded**, then **Install**. Restart if asked

The profile expires after a few days.

**Turn CarPlay off.** A car running CarPlay talks to the iPhone differently from
the way it talks to the iPod, so the log would be no use. Go to
`Settings > Screen Time > Content & Privacy Restrictions`, switch it on, then
under `Allowed Apps & Features` turn **CarPlay** off. Turn it back on after the
test.

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
3. Take `crashes_and_spins/accessoryd-packets-<date>.log.ips`

## 4. Send it in

[Open an issue](https://github.com/anthonyfletcher/podbox/issues/new) with your
notes, and drag in a zip of `usb.log`, the `accessoryd-packets` file and any
photos.

Can't unpack the sysdiagnose? Say so in the issue rather than posting it
publicly, and a private route will be arranged.

Working accessories are worth an issue too - just the accessory, your iPod and
what you tried.
