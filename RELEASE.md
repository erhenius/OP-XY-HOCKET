# Hocket 0.1.26

Hocket is a player for the OP-XY that lets instrument tracks trigger one another. A single played or sequenced note can travel through all eight tracks with Hocket active. Each track plays its own sound and can send onward to two destinations with independent delays.

- **Note** follows the incoming gate. Turn K2 to split by incoming velocity. Hard hits take the upper send and soft hits take the lower, even with instrument velocity sensitivity at zero. Fork Off enables both sends.
- **Arp** plays up to eight semitone offsets. Turn K2 for swing from −95% to +95%. Try +34% for a triplet feel. Use rests or skip muted slots, shuffle the order, and repeat while held.
- **Spray** draws pitches from the same editor. Turn K2 to vary attack timing by up to half a Rate step early or late. Hold a note for steady hits and release it for a tail of up to five seconds.

Import the source ZIP with **File → Import Machine ZIP…** in TE Builder. Build for **OP-XY OS 1.2.1**, then select Hocket in the player/arp menu on each participating instrument track.

### Updating a project

Existing swing settings keep their timing. The former 67% pair setting appears as +34% on the signed scale. Existing Nudge settings keep their knob position and use the wider timing range. Keep a project’s matching .xy and .oxy files together when transferring it.

[Read the illustrated manual](docs/Hocket-Manual.pdf) · [See the routing animation](docs/hocket.gif) · [See the menu tour](docs/hocket-menus.gif)
