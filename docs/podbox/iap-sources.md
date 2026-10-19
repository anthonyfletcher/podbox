# iAP: where the dock and remote work came from

An internals document, for anyone checking where PodBox's iAP code came from
or extending it. iAP is the protocol an iPod speaks to docks, receivers,
remotes and older cars, over the dock connector's serial pins or over USB. The
code is `apps-ipod/iap/` for serial and `firmware/usbstack/iap/` with
`usb_iap.c` for USB. The newer protocol cars use with an iPhone is iAP2, and
[`iap2-sources.md`](iap2-sources.md) covers it.

**Some of this code was taken from other projects**, all under the same GPLv2
as PodBox, and adapted. The rest was written for PodBox, from facts in the
sources below. Apple's MFi documentation was not consulted for this work.
RockPod's own comments cite Apple's *MFi Accessory Firmware Specification R46*,
so the facts that came through RockPod rest on it at one remove.

## Sources

| Source | Licence | Used for |
|---|---|---|
| [RockPod](https://github.com/nuxcodes/rockpod) by Nux Li, commit `951d17c057` and the July work before it | GPLv2: code taken | Most of the serial work: undock, identification without authentication, PlayControl, notifications, volume and mute, track information, buttons, power, chapters, IDPS and transaction IDs, authentication deadlines, paced album art; remote buttons and the backlight; volume buttons in lists |
| Paul Sauro's Rockbox Gerrit changes [7900](https://gerrit.rockbox.org/r/c/rockbox/+/7900), [7906](https://gerrit.rockbox.org/r/c/rockbox/+/7906), [7938](https://gerrit.rockbox.org/r/c/rockbox/+/7938), [7940](https://gerrit.rockbox.org/r/c/rockbox/+/7940) | GPLv2: code taken | USB: a new rate held until the dock has it, silence while the mixer changes, docks in charge-only mode, seeking from a held remote button and the seek commands |
| Rockbox's serial iAP code and its tables of each command's layout | GPLv2, already in the tree | The layouts of the chapter, artwork and model number commands; the track capability bits |
| [libiap](https://github.com/mojyack/libiap) by mojyack | MIT, already in Rockbox | The USB iAP core this work extends |
| [ipod-gadget](https://github.com/oandrew/ipod-gadget) and [ipod](https://github.com/oandrew/ipod) by Andrew Onyshchuk | MIT: facts only | How cars behave when they never set a rate or never authenticate; the iPhone 4's product ID and model number; Apple's product ID list |
| USB Audio Class 1.0 | USB-IF standard | Setting the stream's rate |

## The facts

### USB

| Fact | Source |
|---|---|
| A new rate stays silent until the dock acknowledges it, or for a second | Gerrit 7900; the second is PodBox's choice |
| Silence while a thread changes the mixer | Gerrit 7938 |
| Charge only still offers docks and cars the iAP configuration | Gerrit 7940 |
| A held Next or Previous scans after two seconds; the seek commands scan | Gerrit 7906 |
| An accessory may list any one of the player's rates | libiap |
| A car may never answer the request for its rates, and set one on the stream instead | observed, a tester's car |
| Several cars never set the stream's rate, and play the one announced | ipod-gadget |
| During IDPS a second StartIDPS restarts it; an abandon is answered and the accessory may then identify with IdentifyDeviceLingoes, which carries no transaction ID and ends their use | RockPod's serial IDPS handling (`951d17c057`), which the USB side matches |
| An accessory identifying with a device ID and no authentication is treated as authenticated | ipod |
| A Linux computer picks the first configuration whose first interface is not vendor-specific | Linux's USB core |

### Identify As

| Fact | Source |
|---|---|
| Product IDs: iPod Video `0x1209`, iPod Classic `0x1261`, iPhone 4 `0x1297` | Rockbox's target headers; ipod-gadget's product ID list |
| iPod Classic model `0x00130200`, "MC293" | Rockbox's serial iAP code |
| iPhone 4 model `0x00111349`, "MC676" | ipod |
| The model number reply carries a 4-byte model ID before the string | Rockbox's serial iAP code |

### Serial

| Fact | Source |
|---|---|
| Undock: the 6G's detect edge, the 5G's accessory-detect reading | RockPod |
| Accessories that do not authenticate; lingoes the player lacks left out | RockPod |
| PlayControl, the four-byte notification mask, volume and mute, track information, buttons, power | RockPod |
| Chapters from the track's cue sheet | RockPod; the command layouts from Rockbox's tables |
| IDPS, transaction IDs, the accessory's maximum payload | RockPod |
| Authentication deadlines: two seconds a certificate step, 7 s for a 1.0 signature, 75 s for a 2.0 one | RockPod; Rockbox's code for 75 s. PodBox allows five seconds a step |
| Album art: the commands, and sending a packet at a time | Rockbox's tables; RockPod. The 56 and 128 pixel sizes are PodBox's choice |
