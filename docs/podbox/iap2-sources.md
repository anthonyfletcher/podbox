# iAP2: where the protocol facts came from

An internals document, for anyone checking where PodBox's iAP2 code came from
or extending it. iAP2 is the protocol an iPhone speaks to a car; PodBox speaks
it so that a car which takes an iPhone, but no longer an iPod, plays from the
player. The code is `firmware/usbstack/usb_iap2.c`, `usb_iap2_control.c` and
the iAP2 half of `apps-ipod/iap/iap-library.c`.

**No code was copied from any source.** Every line was written for PodBox.
What the sources below supplied are facts: message numbers, the layout of
their parameters, and how a car and an iPhone behave. Apple's MFi
documentation was not consulted.

## How the facts were found

- **Observed.** The car and the player, in the player's own USB
  log. And an iPhone talking to the same car, with Apple's iAP logging profile
  installed: it records every iAP2 packet the phone exchanges with an
  accessory, which a sysdiagnose then collects. Most of what PodBox does is
  what that iPhone was seen to do.
- **Public domain.** carplayd, whose message table is decoded from Apple's
  CarPlay Simulator.
- **Read for facts only.** Projects whose licence cannot come into a GPLv2
  tree. They were read for numbers and behaviour; nothing was copied, not even
  in shape.
- **Standard.** The USB-IF's HID specification and usage tables.

## Sources

| Source | Licence | Used for |
|---|---|---|
| [carplayd](https://github.com/lvalen91/carplayd) by lvalen91 | Unlicense (public domain) | The message table; the link layer's framing and checksums; the order of authentication and identification; CarPlayAvailability's layout |
| [Nocturne](https://github.com/usenocturne/nocturne) | GPL-3.0 with its own API licence: facts only | Now playing's and the car's buttons' parameter layouts; two file transfer piece codes |
| [carplay-wifi-extractor](https://github.com/HaToan/carplay-wifi-extractor) by HaToan | none: facts only | The link synchronisation payload, and the phone's side of it |
| JJTech0130's iAP2 gists | none: facts only | Message names, and how to talk iAP2 to a real iPhone |
| [usbmuxd](https://github.com/libimobiledevice/usbmuxd) | LGPL-2.1: facts only | Apple's USB mode requests, to rule them out |
| libiap, already in Rockbox | GPLv2-compatible | iAP's HID report framing |
| HID 1.11 and the HID Usage Tables | USB-IF standard | The car's buttons |

## The facts

### The link

| Fact | Source |
|---|---|
| The probe `FF 55 02 00 EE 10`, echoed by the device | carplayd; observed |
| Packets: `FF 5A`, length, control, sequence, acknowledgement, session, header checksum, payload checksum | carplayd |
| Control bits SYN, ACK, EAK, RST | carplayd (SYN, ACK); carplay-wifi-extractor (EAK, RST) |
| The link sync payload, and the phone answering with the accessory's own parameters | carplay-wifi-extractor; observed |
| Session types: 0 control, 1 file transfer | carplayd; observed |
| An iPhone sends no bare acknowledgement and never retransmits; this car stops reading after a bare one | observed |

### Authentication, identification and power

| Fact | Source |
|---|---|
| The order AA00 to AA05, certificate and challenge in parameter 0 | carplayd |
| A 32-byte challenge for this car's certificate; 20 bytes for a larger one | observed; carplayd's capture |
| Identification 1D00 to 1D02; the messages each side sends and takes | carplayd |
| What an iPhone sends next: UUID, language, name, then Bluetooth pairing start and stop | observed; Nocturne for the strings' layout |
| CarPlayAvailability after the car's PowerSourceUpdate | carplayd for the layout; observed for the timing |
| Power updates: the two an iPhone sends | observed |
| No mass storage while the car is answered, or it resets | observed |

### Now playing, audio and buttons

| Fact | Source |
|---|---|
| 5000 and 5001: the attributes asked for and their types | Nocturne; observed |
| The position about twice a second while playing, or the car's progress bar stays at 0:00 | observed |
| The queue: playback attribute 0x0E, 1 with the queue's list, 0 when the car chose what plays | observed |
| USB audio: DA00 to DA02, and the rate as an index into an iPhone's nine rates | carplayd for the IDs; observed for the index |
| The car's buttons: 6800 and 6802's layout, the consumer page usages | Nocturne; HID usage tables; observed |

### The media library

| Fact | Source |
|---|---|
| The library's information, ID and type | observed |
| The car's request: the revision it holds, the properties it wants | observed |
| Updates: items, playlists, deletions, reset, progress and revision; a change sent as one complete update | observed |
| An item's properties and their types | observed |
| A playlist: its ID, name, folder, and the transfer carrying its tracks | observed |
| Playing: the car sends the tracks' IDs and a start index, never a playlist's | observed |
| This car reloads its library at every complete update, and lists playlists flat | observed |
| A playlist marked as a folder | **not from any source**: an assumption, untested, since this car shows no folders |

### File transfers and artwork

| Fact | Source |
|---|---|
| A transfer's setup and the car's answers; artwork, the queue's list and a playlist's tracks as its three types | observed |
| The pieces: only, first | observed |
| The pieces: middle, last | Nocturne |
| A transfer with every track, empty when there is no cover; a late cover sent again under a new transfer | observed |
| The cover as a 300x300 baseline JPEG with a segment per table, which this car needs | observed |

### Outside the protocol

| Fact | Source |
|---|---|
| An iPhone answers the car's vendor request `0x53` with four zero bytes | observed |
| `0x45` and `0x52` are Apple's mode requests, so `0x53` is not one | usbmuxd |
| With a sound card beside the disk, this car stops after `0x53` with an authentication error | observed |
| An iPhone's iAP configuration: audio control, audio streaming, HID | observed |
